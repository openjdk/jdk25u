#include "gc/h/hMarker.hpp"

#include "gc/h/hArena.inline.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hOopClosures.inline.hpp"
#include "gc/h/hRegionRemSet.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/taskqueue.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/ostream.hpp"

static void diag_bad(const char* where, const void* p, oop obj);

HGCMarkQueueSet::HGCMarkQueueSet(uint n) :
    GenericTaskQueueSet<HGCMarkQueue, mtGC>(n),
    _shared_queue(),
    _push_lock(Mutex::nosafepoint, "HGC mark queue push") {
  for (uint i = 0; i < n; ++i) {
    register_queue(i, new HGCMarkQueue());
  }
}

HGCMarkQueueSet::~HGCMarkQueueSet() {
  for (uint i = 0; i < size(); ++i) {
    delete queue(i);
  }
}

void HGCMarkQueueSet::push_shared(HGCArena* arena) {
  MutexLocker ml(&_push_lock, Mutex::_no_safepoint_check_flag);
  _shared_queue.push(arena);
}

bool HGCMarkQueueSet::pop_shared(HGCArena*& arena) {
  MutexLocker ml(&_push_lock, Mutex::_no_safepoint_check_flag);
  if (_shared_queue.pop_local(arena)) {
    return true;
  }
  return _shared_queue.pop_overflow(arena);
}

void HGCMarkQueueSet::clear() {
  HGCArena* arena = nullptr;
  for (uint i = 0; i < size(); ++i) {
    while (queue(i)->pop_local(arena) || queue(i)->pop_overflow(arena)) {
    }
  }
  MutexLocker ml(&_push_lock, Mutex::_no_safepoint_check_flag);
  while (_shared_queue.pop_local(arena) || _shared_queue.pop_overflow(arena)) {
  }
}

bool HGCMarkQueueSet::has_pending_work() {
  {
    MutexLocker ml(&_push_lock, Mutex::_no_safepoint_check_flag);
    if (!_shared_queue.is_empty()) {
      return true;
    }
  }
  for (uint i = 0; i < size(); i++) {
    if (!queue(i)->is_empty()) {
      return true;
    }
  }
  return false;
}

THREAD_LOCAL oopDesc* hgc_diag_holder = nullptr;


void HGCMarkClosure::mark_object(oop obj) {
  HeapWord* addr = cast_from_oop<HeapWord*>(obj);
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  HGCRegion* region = heap->region(addr);
  if (region == nullptr) {

    if (_lenient) {
      return;
    }
    assert(region != nullptr, "object is not in the HGC heap");
    return;
  }

  if (region->state() != HGCRegion::Reclaiming) {
    return;
  }

  HGCArena* arena = nullptr;
  if (!region->is_large()) {
    arena = heap->arena(addr);
    assert(arena != nullptr, "object is not in the HGC heap");
    if (arena->is_black(addr)) {
      return;
    }
  }
  if (!oopDesc::is_oop(obj, true)) {
    if (_lenient) {

      return;
    }
    diag_bad("mark_object", _field, obj);
    ShouldNotReachHere();
  }
  if (region->is_large()) {
    HGCRegion* owner = region->large_owner();
    assert(owner != nullptr && owner->is_large_owner(), "large object without owner");
    if (addr != owner->start()) {

      if (_lenient) {
        return;
      }
      diag_bad("mark_object (large interior)", _field, obj);
      ShouldNotReachHere();
    }
    if (owner->mark_large()) {

      obj->oop_iterate(this);
    }
    return;
  }
  if (arena->mark_gray(addr) && arena->try_enqueue()) {
    if (_shared_push) {
      heap->mark_queue_set()->push_shared(arena);
    } else {
      _queue->push(arena);
    }
  }
}

template <typename T>
static void hgc_record_reference(HGCCollectedHeap* heap, uint worker_id, T* p, oop obj) {
  if (HGCInSameRegion(p, obj)) {
    return;
  }
  HGCRegion* to = heap->region(cast_from_oop<HeapWord*>(obj));
  if (to == nullptr) {
    return;
  }
  HGCHeapRegionRemSet* rem_set = to->rem_set();
  if ((rem_set != nullptr) && rem_set->is_tracked()) {
    rem_set->add_reference(p, worker_id);
  }
}

void HGCMarkClosure::do_oop(oop* p) {
  oop obj = *p;
  if (obj != nullptr) {
    _field = p;
    mark_object(obj);
    if (_record_refs) {
      hgc_record_reference(HGCCollectedHeap::heap(), _worker_id, p, obj);
    }
  }
}

void HGCMarkClosure::do_oop(narrowOop* p) {
  narrowOop value = *p;
  if (!CompressedOops::is_null(value)) {

    oop obj = _lenient ? CompressedOops::decode_raw(value)
                       : CompressedOops::decode_not_null(value);
    _field = p;
    mark_object(obj);
    if (_record_refs) {
      hgc_record_reference(HGCCollectedHeap::heap(), _worker_id, p, obj);
    }
  }
}

void HGCBatchedSATBBufferClosure::do_buffer(void** buffer, size_t size) {
  HGCMarkClosure cl(_queue, false, 0, true , _reference_mode, true );
  cl.set_ref_discoverer(_ref_discoverer);
  for (size_t i = 0; i < size; ++i) {
    cl.mark_object(cast_to_oop(buffer[i]));
  }
}

namespace {

class HGCYieldTerminator : public TerminatorTerminator {
  const bool _concurrent;

public:
  explicit HGCYieldTerminator(bool concurrent) : _concurrent(concurrent) {}

  bool should_exit_termination() override {
    return _concurrent && SuspendibleThreadSet::should_yield();
  }
};

}

void HGCMarkTask::work(uint worker_id) {
  HGCMarkQueue* queue = _queues->queue(worker_id);

  HGCMarkClosure cl(queue, true, worker_id, true , _reference_mode);
  cl.set_ref_discoverer(_ref_discoverer);
  size_t claimed = 0;
  size_t stale = 0;
  SuspendibleThreadSetJoiner sts_join(_concurrent);
  HGCYieldTerminator terminator(_concurrent);

  for (;;) {
    if (sts_join.should_yield()) {
      sts_join.yield();
    }
    HGCArena* arena = nullptr;
    bool have_arena = false;
    if (worker_id == 0) {

      have_arena = _queues->pop_shared(arena) ||
                   queue->pop_local(arena) || queue->pop_overflow(arena) ||
                   _queues->steal(worker_id, arena);
    } else {
      have_arena = queue->pop_local(arena) || queue->pop_overflow(arena) ||
                   _queues->steal(worker_id, arena);
    }
    if (have_arena) {
      if (arena->try_claim()) {
        ++claimed;
        log_trace(gc, mark)("HGC worker %u: process arena " PTR_FORMAT, worker_id, p2i(arena->start()));
        arena->process_gray(&cl);
        arena->release();

        if (arena->has_pending_gray() && arena->try_enqueue()) {
          queue->push(arena);
        }
      } else {
        ++stale;
      }
      continue;
    }
    if (_terminator.offer_termination(&terminator)) {
      log_trace(gc, mark)("HGC worker %u: claimed=%zu stale=%zu", worker_id, claimed, stale);
      return;
    }
  }
}

static void diag_bad(const char* where, const void* p, oop obj) {
  ResourceMark rm;
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  oop holder = hgc_diag_holder;
  tty->print_cr("DIAG %s: value=" PTR_FORMAT " field=" PTR_FORMAT, where, p2i(obj), p2i(p));
  if (holder != nullptr) {
    HeapWord* h = cast_from_oop<HeapWord*>(holder);
    HGCRegion* hr = heap->region(h);
    tty->print_cr("  holder=" PTR_FORMAT " klass=%s size=%zu offset_of_field=%zd region=%u state=%d",
                  p2i(holder), holder->klass()->external_name(), holder->size(),
                  (const char*)p - (const char*)h,
                  hr != nullptr ? heap->region_index(hr->start()) : 0,
                  hr != nullptr ? (int)hr->state() : -1);
    HGCArena* ha = heap->arena(h);
    if (ha != nullptr) {
      tty->print_cr("  holder arena=" PTR_FORMAT " black=%d gray=%d offset=%zu",
                    p2i(ha->start()), (int)ha->is_black(h), (int)ha->is_gray(h),
                    (size_t)ha->offset(h));
    }
    if (p != nullptr) {
      const HeapWord* f = reinterpret_cast<const HeapWord*>(p);
      tty->print("  holder words around the field:");
      for (int i = -2; i <= 3; ++i) {
        tty->print(" [%+d]=" PTR_FORMAT, i, p2i(*reinterpret_cast<HeapWord* const*>(f + i)));
      }
      tty->cr();
    }
  } else {
    tty->print_cr("  holder=<none: roots or SATB>");
  }
  HeapWord* v = cast_from_oop<HeapWord*>(obj);
  HGCRegion* vr = heap->region(v);
  tty->print_cr("  value region=%u state=%d",
                vr != nullptr ? heap->region_index(vr->start()) : 0,
                vr != nullptr ? (int)vr->state() : -1);
  if ((p != nullptr) && (vr != nullptr)) {
    CardTable::CardValue* card = heap->card_table()->byte_for(const_cast<void*>(p));
    CardTable::CardValue card_value = *card;
    const char* kind = HGCCardTable::is_young_card_value(card_value) ? "young"
                     : HGCCardTable::is_clean_card_value(card_value) ? "clean"
                     : HGCCardTable::is_dirty_card_value(card_value) ? "dirty" : "?";
    HGCHeapRegionRemSet* tr = vr->rem_set();
    tty->print_cr("  field card=%s target_remset=%d tracked=%d has_ref=%d",
                  kind, tr != nullptr, (tr != nullptr && tr->is_tracked()) ? 1 : 0,
                  (tr != nullptr && tr->has_card_set() &&
                   tr->contains_reference(const_cast<void*>(p))) ? 1 : 0);
  }
  HGCArena* va = heap->arena(v);
  if (va != nullptr) {
    tty->print_cr("  arena=" PTR_FORMAT " size=%zu bytes, black(v)=%d gray(v)=%d offset=%zu",
                  p2i(va->start()), va->size(),
                  (int)va->is_black(v), (int)va->is_gray(v),
                  (size_t)va->offset(v));
  }
  for (int i = -4; i < 6; ++i) {
    HeapWord* w = v + i;
    oop cand = cast_to_oop(w);
    tty->print_cr("    [%+d] = " PTR_FORMAT "%s",
                  i, p2i(*reinterpret_cast<HeapWord* const*>(w)),
                  oopDesc::is_oop(cand, true) ? "  <- valid oop starts here" : "");
  }
}
