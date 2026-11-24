#include "gc/h/hBarrierSet.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/h/hBarrierSet.inline.hpp"
#include "gc/h/hBarrierSetAssembler.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hRegionRemSet.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "code/nmethod.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "gc/shared/cardTableBarrierSetAssembler.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/thread.hpp"
#ifdef COMPILER1
#include "gc/shared/c1/cardTableBarrierSetC1.hpp"
#include "gc/h/c1/hBarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/shared/c2/cardTableBarrierSetC2.hpp"
#include "gc/h/c2/hBarrierSetC2.hpp"
#endif

class CardTableBarrierSetC1;
class CardTableBarrierSetC2;

HGCBarrierSet::HGCBarrierSet(HGCCardTable* card_table) :
  CardTableBarrierSet(make_barrier_set_assembler<HGCBarrierSetAssembler>(),
#ifdef COMPILER1
                      make_barrier_set_c1<HGCBarrierSetC1>(),
#else
                      make_barrier_set_c1<CardTableBarrierSetC1>(),
#endif
#ifdef COMPILER2
                      make_barrier_set_c2<HGCBarrierSetC2>(),
#else
                      make_barrier_set_c2<CardTableBarrierSetC2>(),
#endif
                      card_table,
                      new HGCBarrierSetNMethod(),
                      BarrierSet::FakeRtti(BarrierSet::HGCBarrierSet)),
  _satb_mark_queue_buffer_allocator("HGC SATB Buffer Allocator", 1 * K),
  _satb_mark_queue_set(&_satb_mark_queue_buffer_allocator),
  _dirty_card_queue_buffer_allocator("HGC Dirty Card Buffer Allocator", 1 * K),
  _dirty_card_queue_set(&_dirty_card_queue_buffer_allocator) {}

bool HGCBarrierSetNMethod::nmethod_entry_barrier(nmethod* nm) {
  if (!is_armed(nm)) {
    return true;
  }

  if (nm->is_unloading()) {
    return false;
  }

  HGCnmethodData* data = HGCnmethodData::for_nmethod(nm);
  assert(data != nullptr, "nmethod without HGC data");
  HGCReentrantLocker locker(data->lock());

  if (!is_armed(nm)) {
    return true;
  }
  if (nm->is_unloading()) {
    nm->unlink_from_method();
    return false;
  }

  return BarrierSetNMethod::nmethod_entry_barrier(nm);
}

HGCReentrantLock::HGCReentrantLock() :
  _owner(nullptr), _count(0) {}

HGCReentrantLock::~HGCReentrantLock() {
  assert(_count == 0, "unbalanced lock");
}

void HGCReentrantLock::lock() {
  Thread* const thread = Thread::current();
  Thread* const owner = Atomic::load(&_owner);

  if (owner != thread) {
    _monitor.lock();
    Atomic::store(&_owner, thread);
  }

  _count++;
}

void HGCReentrantLock::unlock() {
  assert(owned_by_self(), "invalid owner");
  assert(_count > 0, "invalid count");

  _count--;
  if (_count == 0) {
    Atomic::store(&_owner, (Thread*)nullptr);
    _monitor.unlock();
  }
}

bool HGCReentrantLock::owned_by_self() const {
  return Atomic::load(&_owner) == Thread::current();
}

HGCnmethodData* HGCnmethodData::for_nmethod(nmethod* nm) {
  return nm->gc_data<HGCnmethodData>();
}

void HGCBarrierSet::on_thread_create(Thread *thread) {
  HGCThreadLocalData::create(thread, &_satb_mark_queue_set, &_dirty_card_queue_set);
}

void HGCBarrierSet::on_thread_destroy(Thread *thread) {

  HGCArena* arena = HGCThreadLocalData::get_arena(thread);
  size_t tlab_size = HGCThreadLocalData::get_tlab_size(thread);
  if (tlab_size > 0) {
    HGCRegion* region = (arena != nullptr) ? arena->region() : nullptr;
    if (region != nullptr) {
      region->sub_tlab_size(tlab_size);
    }
    HGCThreadLocalData::set_tlab_size(thread, 0);
  }
  HGCThreadLocalData::set_arena(thread, nullptr);
  HGCThreadLocalData::destroy(thread);
}

void HGCBarrierSet::on_thread_attach(Thread* thread) {
  BarrierSet::on_thread_attach(thread);
  SATBMarkQueue& satbq = HGCThreadLocalData::satb_mark_queue(thread);
  assert(!satbq.is_active(), "SATB queue should not be active");
  assert(satbq.buffer() == nullptr, "SATB queue should not have a buffer");
  assert(satbq.index() == 0, "SATB queue index should be zero");

  satbq.set_active(_satb_mark_queue_set.is_active());
}

void HGCBarrierSet::on_thread_detach(Thread* thread) {
  CardTableBarrierSet::on_thread_detach(thread);
  SATBMarkQueue& queue = HGCThreadLocalData::satb_mark_queue(thread);
  HGCBarrierSet::satb_mark_queue_set().flush_queue(queue);
  HGCDirtyCardQueue& dirty_queue = HGCThreadLocalData::dirty_card_queue(thread);
  HGCBarrierSet::dirty_card_queue_set().flush_queue(dirty_queue);
}

template <class T> void
HGCBarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  HGCSATBMarkQueueSet& queue_set = HGCBarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  SATBMarkQueue& queue = HGCThreadLocalData::satb_mark_queue(Thread::current());

  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = RawAccess<>::oop_load(elem_ptr);
    if (!CompressedOops::is_null(heap_oop)) {
      queue_set.enqueue_known_active(queue, CompressedOops::decode_not_null(heap_oop));
    }
  }
}

void HGCBarrierSet::write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

static void enqueue_young_target(const void* addr);
static bool young_card_in_frozen_cset(volatile CardTable::CardValue* card);

void HGCBarrierSet::write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void HGCBarrierSet::write_ref_array(HeapWord* start, size_t count) {
  if (count == 0) {
    return;
  }
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  if (!heap->is_in(start)) {
    return;
  }
  HGCCardTable* ct = heap->card_table();
  char* p = reinterpret_cast<char*>(start);
  char* end = p + count * heapOopSize;

  while (p < end) {
    volatile CardValue* card = ct->byte_for(p);
    CardValue value = Atomic::load(card);
    char* card_end = reinterpret_cast<char*>(ct->addr_for(const_cast<CardValue*>(card))) +
                     HGCCardTable::card_size_in_words() * HeapWordSize;
    card_end = MIN2(card_end, end);

    if (value == HGCCardTable::young_card_val()) {
      if (young_card_in_frozen_cset(card)) {
        dirty_card(card);
      } else {
        for (char* e = p; e < card_end; e += heapOopSize) {
          enqueue_young_target(e);
        }
      }
    } else if (value == HGCCardTable::mixed_young_card_val()) {
      dirty_card(card);
      if (!young_card_in_frozen_cset(card)) {
        for (char* e = p; e < card_end; e += heapOopSize) {
          enqueue_young_target(e);
        }
      }
    } else {
      dirty_card(card);
    }
    p = card_end;
  }
}

void HGCBarrierSet::write_region(MemRegion mr) {
  if (mr.is_empty()) {
    return;
  }

  OrderAccess::storeload();
  volatile CardValue* first = _card_table->byte_for(mr.start());
  volatile CardValue* last = _card_table->byte_after(mr.last());
  for (volatile CardValue* card = first; card < last; ++card) {
    requeue_dirty_card(card);
  }
}

static void dirty_card_impl(volatile CardTable::CardValue* card);

static void enqueue_dirty_card(volatile CardTable::CardValue* card) {
  Thread* thread = Thread::current();
  HGCDirtyCardQueue& queue = HGCThreadLocalData::dirty_card_queue(thread);
  HGCBarrierSet::dirty_card_queue_set().enqueue(queue, card);
  hgc_diag_ring_push(hgc_cards_enqueued, &hgc_cards_enqueued_idx, (uintptr_t)card);
}

static HGCRegion* card_source_region(volatile CardTable::CardValue* card) {
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  HeapWord* start = heap->card_table()->addr_for(const_cast<CardTable::CardValue*>(card));
  return heap->region(start);
}

static bool mixed_young_card_needs_tracking(volatile CardTable::CardValue* card) {
  HGCRegion* source = card_source_region(card);
  return source != nullptr;
}

static bool young_card_in_frozen_cset(volatile CardTable::CardValue* card) {
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  if (!heap->is_concurrent_marking()) {
    return false;
  }
  HGCRegion* source = card_source_region(card);
  return source != nullptr &&
         source->state() == HGCRegion::Reclaiming;
}

static void enqueue_young_target(const void* addr) {
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  if (!heap->is_concurrent_marking()) {
    return;
  }
  oop obj = nullptr;
  if (UseCompressedOops) {
    narrowOop value = RawAccess<MO_RELAXED>::oop_load(
        reinterpret_cast<narrowOop*>(const_cast<void*>(addr)));
    if (!CompressedOops::is_null(value)) {
      obj = CompressedOops::decode_raw_not_null(value);
    }
  } else {
    obj = RawAccess<MO_RELAXED>::oop_load(
        reinterpret_cast<oop*>(const_cast<void*>(addr)));
  }
  if (obj == nullptr || !heap->is_in(cast_from_oop<HeapWord*>(obj))) {
    return;
  }
  HGCRegion* to = heap->region(cast_from_oop<HeapWord*>(obj));
  if (to == nullptr || to->state() != HGCRegion::Reclaiming) {
    return;
  }
  HeapWord* target = cast_from_oop<HeapWord*>(obj);
  if (to->is_large()) {
    if (to->large_owner()->is_large_marked()) {
      return;
    }
  } else {
    HGCArena* arena = heap->arena(target);
    if (arena != nullptr && (arena->is_black(target) || arena->is_gray(target))) {
      return;
    }
  }
  HGCBarrierSet::enqueue_preloaded(obj);
}

static void dirty_card_impl(volatile CardTable::CardValue* card) {
  for (;;) {
    CardTable::CardValue value = *card;
    if (HGCCardTable::is_dirty_card_value(value)) {
      return;
    }
    if (value == HGCCardTable::young_card_val()) {
      if (!young_card_in_frozen_cset(card)) {
        return;
      }
    }
    if (value == HGCCardTable::mixed_young_card_val() &&
        !mixed_young_card_needs_tracking(card)) {
      return;
    }
    CardTable::CardValue to;
    if (value == HGCCardTable::mixed_clean_card_val() ||
        value == HGCCardTable::mixed_young_card_val()) {
      to = HGCCardTable::mixed_dirty_card_val();
    } else {
      to = HGCCardTable::dirty_card_val();
    }
    CardTable::CardValue witnessed = Atomic::cmpxchg(card, value, to);
    if (witnessed == value) {
      break;
    }
  }
  enqueue_dirty_card(card);
}

void HGCBarrierSet::dirty_card(volatile CardValue* card) {
  dirty_card_impl(card);
}

static void dirty_mixed_young_card(volatile CardTable::CardValue* card) {
  for (;;) {
    CardTable::CardValue value = *card;
    if (value != HGCCardTable::mixed_young_card_val()) {

      dirty_card_impl(card);
      return;
    }
    if (Atomic::cmpxchg(card, value, HGCCardTable::mixed_dirty_card_val()) == value) {
      break;
    }
  }
  enqueue_dirty_card(card);
}

static bool hgc_mixed_young_store_needs_tracking(volatile CardTable::CardValue* card,
                                                 HGCRegion* source) {
  if (Atomic::load(card) != HGCCardTable::mixed_young_card_val()) {
    return false;
  }

  return source != nullptr && source->state() != HGCRegion::Reclaiming;
}

void HGCBarrierSet::post_barrier(const void* addr) {
  if (!HGCUsePostBarrier) return;
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  if (!heap->is_in(addr)) {
    return;
  }
  volatile CardValue* card = heap->card_table()->byte_for(addr);
  CardValue value = Atomic::load(card);

  if (HGCCardTable::is_dirty_card_value(value)) {
    return;
  }

  OrderAccess::release();
  if (value == HGCCardTable::young_card_val()) {
    if (!young_card_in_frozen_cset(card)) {
      enqueue_young_target(addr);
      return;
    }
  }
  if (value == HGCCardTable::mixed_young_card_val()) {
    HGCRegion* source =
        heap->region(reinterpret_cast<HeapWord*>(const_cast<void*>(addr)));
    if (hgc_mixed_young_store_needs_tracking(card, source)) {
      enqueue_young_target(addr);
      dirty_mixed_young_card(card);
      return;
    }
  }
  dirty_card(card);
}

void HGCBarrierSet::requeue_dirty_card(volatile CardTable::CardValue* card) {

  for (;;) {
    CardTable::CardValue value = *card;
    if (HGCCardTable::is_dirty_card_value(value)) {
      break;
    }
    CardTable::CardValue to;
    if (value == HGCCardTable::mixed_clean_card_val()) {
      to = HGCCardTable::mixed_dirty_card_val();
    } else if (value == HGCCardTable::clean_card_val()) {
      to = HGCCardTable::dirty_card_val();
    } else {
      return;
    }
    if (Atomic::cmpxchg(card, value, to) == value) {
      break;
    }
  }
  enqueue_dirty_card(card);
}

void HGCBarrierSet::mark_card_dirty_and_enqueue(volatile CardTable::CardValue* card,
                                                CardTable::CardValue to) {
  assert(HGCCardTable::is_dirty_card_value(to), "target must be a dirty flavor");
  for (;;) {
    CardTable::CardValue value = *card;
    if (value == to) {
      break;
    }
    if (Atomic::cmpxchg(card, value, to) == value) {
      break;
    }
  }
  enqueue_dirty_card(card);
}
