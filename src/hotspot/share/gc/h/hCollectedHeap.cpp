#include "hCollectedHeap.hpp"
#include "gc/h/hArena.hpp"
#include "gc/h/hArena.inline.hpp"
#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hCardSet.hpp"
#include "gc/h/hCardSetMemory.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hCollectionSetPolicy.hpp"
#include "gc/h/hConcurrentMarkThread.hpp"
#include "gc/h/hUncommitThread.hpp"
#include "gc/h/hConcurrentRefine.hpp"
#include "gc/h/hMarkSweep.hpp"
#include "gc/h/hMarker.hpp"
#include "gc/h/hOopClosures.inline.hpp"
#include "gc/h/hPolicy.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/h/hRegion.inline.hpp"
#include "gc/h/hRegionRemSet.hpp"
#include "gc/h/hRegionRemSet.inline.hpp"
#include "gc/h/hRemSet.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "logging/log.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/reservedSpace.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/oop.inline.hpp"
#include "nmt/memTag.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/osThread.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/powerOfTwo.hpp"
#include <cstddef>

Mutex* HGCCollectedHeap::_detached_refinement_stats_lock = nullptr;


static size_t hgc_reserved_multi() {

  const size_t desired = MAX2((size_t)2, HGCRegionSize / HGCMaxArenaSize);
  if (!UseCompressedOops) {
    return desired;
  }

  const size_t heap_regions = MaxHeapSize / HGCRegionSize;
  if (heap_regions == 0) {
    return desired;
  }

  const size_t max_reserved = Arguments::max_heap_for_compressed_oops();
  const size_t max_multi = (max_reserved / HGCRegionSize) / heap_regions;
  if (max_multi >= desired) {
    return desired;
  }

  return MAX2((size_t)1, max_multi);
}

static size_t hgc_real_memory_limit() {

  return MaxHeapSize;
}

void* HGCCollectedHeap::operator new(size_t size) {
  return CHeapObj<mtGC>::operator new(size + (MaxHeapSize / HGCRegionSize) * hgc_reserved_multi() * sizeof(HGCRegion*));
}

HGCCollectedHeap::HGCCollectedHeap() :
  _start(nullptr),
  _num_of_k(log2i(HGCRegionSize) - log2i(HGCMinArenaSize) + 1),
  _current_count_of_regions(0),
  _committed_regions(0),
  _real_memory_bytes(0),
  _count_of_regions((MaxHeapSize / HGCRegionSize) * hgc_reserved_multi()),
  _free_region_indices(new (mtGC) GrowableArray<size_t>(16, mtGC)),
  _workers(nullptr),
  _mark_queue_set(nullptr),
  _card_table(nullptr),
  _rem_set(nullptr),
  _card_set_config(nullptr),
  _card_set_free_pool(nullptr),
  _cm_thread(nullptr),
  _uncommit_thread(nullptr),
  _refinement(nullptr),
  _policy(nullptr),
  _cset_policy(new HGCCollectionSetPolicy()),
  _ref_processor(nullptr),
  _clear_all_soft_refs(false),
  _uncommit_queue(new (mtGC) GrowableArray<HGCRegion*>(16, mtGC)),
  _uncommitted_regions(new (mtGC) GrowableArray<HGCRegion*>(16, mtGC)),
  _cset_lock(nullptr),
  _region_space_lock(nullptr),
  _kregions_lock(nullptr),
  _uncommit_lock(nullptr),
  _cycle_lock(nullptr),
  _cycle_end_lock(nullptr),
  _cycle_active(false),
  _concurrent_marking(false),
  _stopped(false) {
  assert(_num_of_k <= max_num_of_k - 1, "wtfk");

  for (size_t i = 0; i < _count_of_regions; ++i) {
    _regions[i] = nullptr;
  }
}

HGCCollectedHeap::~HGCCollectedHeap() {
  stop();
  delete _refinement;
  _refinement = nullptr;
  delete _ref_processor;
  _ref_processor = nullptr;
  delete _policy;
  delete _cset_policy;
  delete _free_region_indices;
  _free_region_indices = nullptr;
  delete _uncommit_queue;
  _uncommit_queue = nullptr;
  delete _uncommitted_regions;
  _uncommitted_regions = nullptr;
  delete _kregions_lock;
  delete _rem_set;
  delete _card_set_free_pool;
  delete _card_set_config;
}

jint HGCCollectedHeap::initialize() {
  size_t max_byte_size = _count_of_regions * HGCRegionSize;
  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, HGCRegionSize);
  _start = (HeapWord*)heap_rs.base();
  initialize_reserved_region(heap_rs);
  log_info(gc, heap)("HGC: reserved heap " PTR_FORMAT ", %zu regions x %zu bytes",
                     p2i(_start), _count_of_regions, HGCRegionSize);

  HGCRegion::setup_region_size();

  _card_table = new HGCCardTable(_reserved);

  HGCHeapRegionRemSet::initialize(_reserved);

  _card_set_config = new HGCCardSetConfiguration();
  _card_set_free_pool = new HGCMonotonicArenaFreePool(_card_set_config->num_mem_object_types());

  _rem_set = new HGCRemSet(this, _card_table);
  _rem_set->initialize(_count_of_regions);

  HGCBarrierSet* bs = new HGCBarrierSet(_card_table);
  bs->initialize();
  BarrierSet::set_barrier_set(bs);

  {
    HGCSATBMarkQueueSet& satbqs = bs->satb_mark_queue_set();
    satbqs.set_process_completed_buffers_threshold(20);
    satbqs.set_buffer_enqueue_threshold_percentage(60);
  }

  const uint max_gc_workers = MAX2(ParallelGCThreads, ConcGCThreads);
  _workers = new WorkerThreads("HGC Mark", max_gc_workers);
  if (_workers == nullptr) {
    return JNI_ENOMEM;
  }
  _workers->initialize_workers();

  _mark_queue_set = new HGCMarkQueueSet(max_gc_workers);

  _cset_lock = new Mutex(Mutex::nosafepoint, "HGC cset");
  _region_space_lock = new Mutex(Mutex::nosafepoint, "HGC region space");
  _kregions_lock = new Mutex(Mutex::nosafepoint, "HGC kregions");
  _uncommit_lock = new Monitor(Mutex::nosafepoint, "HGC uncommit");
  _detached_refinement_stats_lock = new Mutex(Mutex::nosafepoint, "HGC detached refinement stats");
  _cycle_lock = new Monitor(Mutex::nosafepoint, "HGC cycle");
  _cycle_end_lock = new Monitor(Mutex::safepoint, "HGC cycle end");

  _cm_thread = new HGCConcurrentMarkThread(this);
  _uncommit_thread = new HGCUncommitThread(this);

  _policy = new HGCPolicy();
  jint refine_ecode = 0;
  _refinement = HGCConcurrentRefine::create(_policy, &refine_ecode);
  if (_refinement == nullptr) {
    return refine_ecode;
  }
  HGCBarrierSet::dirty_card_queue_set().set_refinement(_refinement);

  hgc_initialize_class_unloading();

  return JNI_OK;
}

HeapWord* HGCCollectedHeap::InAligns::mem_allocate(HGCCollectedHeap* self,
    HGCArena::Alignments size, bool* gc_overhead_limit_was_exceeded) {
  assert_heap_not_locked_and_not_at_safepoint();
  log_trace(gc, alloc)("HGC: mem_allocate %zu alignments", size.value());
  HGCArena::Alignments actual_size;
  return allocate(self, size, size, &actual_size, true );
}

HeapWord* HGCCollectedHeap::InAligns::allocate(HGCCollectedHeap* self,
    HGCArena::Alignments min_size,
    HGCArena::Alignments requested_size,
    HGCArena::Alignments* actual_size,
    bool allow_large) {

  assert_heap_not_locked_and_not_at_safepoint();

  if (allow_large && self->is_large_request(requested_size)) {
    return self->allocate_large(requested_size, actual_size);
  }

  auto cur = Thread::current();
  HGCArena* a = HGCThreadLocalData::get_arena(cur);

  log_trace(gc, tlab)("HGC tlab: thread: " PTR_FORMAT " [id: %2d], min=%zu bytes, requested=%zu bytes, current arena=" PTR_FORMAT,
                      p2i(cur), cur->osthread()->thread_id(),
                      min_size.to_bytes(), requested_size.to_bytes(),
                      p2i(a != nullptr ? a->start() : nullptr));

  auto use_arena = [&](HGCArena* arena, HGCArena::Alignments size) -> HeapWord* {
    HGCThreadLocalData::set_arena(cur, arena);

    log_trace(gc, tlab)("HGC tlab: use arena " PTR_FORMAT ", take %zu bytes",
                        p2i(arena->start()), size.to_bytes());

    self->add_region_to_cset(arena->region());
    auto ar2 = arena->alloc(size);
    assert(ar2.mem != nullptr, "acquired arena " PTR_FORMAT " cannot fit %zu bytes",
           p2i(arena->start()), size.to_bytes());
    *actual_size = ar2.size;
    return ar2.mem;
  };

  if (a != nullptr) {
    assert(a->region()->state() != HGCRegion::Reclaiming,
           "cached arena of a frozen region: arena=" PTR_FORMAT, p2i(a->start()));
    auto ar = a->alloc(requested_size);

    if (ar.mem != nullptr) {
      *actual_size = ar.size;
      return ar.mem;
    }

    log_trace(gc, tlab)("HGC tlab: arena " PTR_FORMAT " can not fit, candidate size=%zu alignments, span offset=%zu",
                        p2i(a->start()), ar.size.value(), (size_t)ar.offset_of_span);

    if (ar.size >= min_size) {
      *actual_size = ar.size;
      return a->alloc_at(ar).mem;
    }

    auto&& r = a->region();
    assert(r->state() != HGCRegion::Reclaiming, "allocation into a Reclaiming region");

    size_t tlab_size = HGCThreadLocalData::get_tlab_size(cur);
    if (tlab_size > 0) {

      {
        cur->retire_tlab();
      }
      r->sub_tlab_size(tlab_size);
      HGCThreadLocalData::set_tlab_size(cur, 0);
    }
    {
      r->return_arena(a);
    }
    HGCThreadLocalData::set_arena(cur, nullptr);
    HGCArena* arena;
    {
      arena = r->acquire_arena_for(requested_size);
    }
    if (arena != nullptr) {
      return use_arena(arena, requested_size);
    }
    if (min_size != requested_size) {
      {
        arena = r->acquire_arena_for(min_size);
      }
      if (arena != nullptr) {
        return use_arena(arena, min_size);
      }
    }
  }

  {
    size_t requested_category = HGCArena::category_of_request_by_size(requested_size);
    HGCArena::Alignments requested_span =
        HGCArena::require_range_of_span_by_category_of_span(requested_category).first;
    assert(requested_span.to_bytes() <= HGCMaxArenaSize,
           "arena path cannot serve the request: requested=%zu bytes (span %zu bytes), "
           "min=%zu bytes, HGCMaxArenaSize=%zu bytes",
           requested_size.to_bytes(), requested_span.to_bytes(),
           min_size.to_bytes(), HGCMaxArenaSize);
    assert(requested_size.to_bytes() <= HGCMaxArenaSize,
           "large request reached the arena path: requested=%zu bytes, HGCMaxArenaSize=%zu bytes",
           requested_size.to_bytes(), HGCMaxArenaSize);
  }
  assert(k_by_requested_size(requested_size) >= min_k(),
         "requested_size=%zu alignments (%zu bytes), min_size=%zu alignments (%zu bytes), "
         "k=%zu, min_k=%zu, HGCRegionSize=%zu, HGCMinArenaSize=%zu, HGCMaxArenaSize=%zu",
         requested_size.value(), requested_size.to_bytes(),
         min_size.value(), min_size.to_bytes(),
         k_by_requested_size(requested_size), min_k(),
         HGCRegionSize, HGCMinArenaSize, HGCMaxArenaSize);
  size_t k = MIN2(k_by_requested_size(requested_size), max_k());
  {
    MutexLocker ml(Heap_lock);
    for (int i = k; i >= (int)min_k(); --i) {
      HGCRegionDoubleList& rlist = self->_kregions[i];
      HGCRegion* first = rlist.first();

      if (!first) continue;

      for (HGCRegion* r = first; r != nullptr; ) {
        assert(r->state() != HGCRegion::Reclaiming, "Reclaiming region in allocation lists");
        HGCArena* arena;
        {
          arena = r->acquire_arena_for(requested_size);
        }
        if (arena != nullptr) {
          return use_arena(arena, requested_size);
        }
        HGCRegion* next = r->next;
        const size_t target = MIN2(k + 1, max_k());
        if (target != (size_t)i) {
          rlist.remove(r);
          r->set_evristik_k(target);
          self->_kregions[target].push_front(r);
        }
        r = next;
      }
    }
  }
  log_trace(gc, tlab)("HGC tlab: allocate new region k=%zu for requested=%zu bytes",
                      k, requested_size.to_bytes());
  for (size_t kk = k; ; --kk) {
    HGCRegion* r;
    {
      r = self->allocate_new_region(kk);
    }
    if (r == nullptr) {
      return nullptr;
    }
    HGCArena* arena;
    {
      arena = r->acquire_arena_for(requested_size);
    }

    {
      self->publish_region(r);
    }
    if (arena != nullptr) {
      return use_arena(arena, requested_size);
    }
    if (kk <= min_k()) {
      return nullptr;
    }
  }
}

void HGCCollectedHeap::publish_region(HGCRegion* region) {
  const size_t i = region_index(region->start());
  MutexLocker ml(Heap_lock);
  assert(_regions[i] == nullptr, "region index %zu is already published", i);
  _regions[i] = region;
  _kregions[region->k()].push_back(region);
  _cset.push(region);
  log_debug(gc, cset)("HGC cset new region=%zu", i);
}

HGCRegion* HGCCollectedHeap::allocate_new_region(size_t k) {
  assert(k <= max_k(), "k<=max_k()");
  assert(k >= min_k(), "k>=min_k()");

  size_t i;
  bool appended = false;
  {
    MutexLocker ml(_region_space_lock, Mutex::_no_safepoint_check_flag);
    if (_free_region_indices->length() > 0) {
      i = _free_region_indices->at(0);
      _free_region_indices->remove_at(0);
    } else {
      i = Atomic::load(&_current_count_of_regions);
      if (i >= _count_of_regions) {
        return nullptr;
      }
      appended = true;
    }
    size_t real = Atomic::add(&_real_memory_bytes, HGCRegionSize);
    if (real > hgc_real_memory_limit()) {
      Atomic::sub(&_real_memory_bytes, HGCRegionSize);
      if (!appended) {
        _free_region_indices->append(i);
      }
      return nullptr;
    }
    if (appended) {
      Atomic::store(&_current_count_of_regions, i + 1);
    }
    Atomic::add(&_committed_regions, (size_t)1);
  }
  assert(_regions[i] == nullptr, "region index %zu is already published", i);

  HeapWord* region_start = _start + ((i * HGCRegionSize) >> LogHeapWordSize);
  HGCRegion* region;
  {
    region = new (k) HGCRegion(region_start, k);
  }
  {
    HGCHeapRegionRemSet* rs = new HGCHeapRegionRemSet(region);
    rs->set_state_complete();
    rs->install_card_set(new HGCCardSet(_card_set_config, _card_set_free_pool));
    region->set_rem_set(rs);
    _card_table->mark_as_young(MemRegion(region_start, HGCRegionSize >> LogHeapWordSize));
  }
  log_trace(gc, heap)("HGC: region #%zu k=%zu at " PTR_FORMAT, i, k, p2i(region_start));

  return region;
}

bool HGCCollectedHeap::is_large_request(HGCArena::Alignments size) const {
  if (size.to_bytes() > HGCMaxArenaSize) {
    return true;
  }

  size_t category = HGCArena::category_of_request_by_size(size);
  if (category >= 64) {
    return true;
  }
  HGCArena::Alignments required =
      HGCArena::require_range_of_span_by_category_of_span(category).first;
  return required.to_bytes() > HGCMaxArenaSize;
}

HeapWord* HGCCollectedHeap::allocate_large(HGCArena::Alignments requested,
                                           HGCArena::Alignments* actual_size) {
  const size_t bytes = requested.to_bytes();
  const size_t region_bytes = HGCRegionSize;
  size_t regions = (bytes + region_bytes - 1) / region_bytes;
  if (regions == 0) {
    regions = 1;
  }
  if (regions > _count_of_regions) {
    return nullptr;
  }
  const size_t page = (size_t)os::vm_page_size();
  const size_t commit_bytes = align_up(bytes, page);
  size_t real = Atomic::add(&_real_memory_bytes, commit_bytes);
  if (real > hgc_real_memory_limit()) {
    Atomic::sub(&_real_memory_bytes, commit_bytes);
    return nullptr;
  }

  size_t start = SIZE_MAX;
  {
    MutexLocker ml(_region_space_lock, Mutex::_no_safepoint_check_flag);

    size_t found_start;
    size_t found_regions;
    if (_large_runs.alloc_fit(regions, &found_start, &found_regions)) {
      start = found_start;
      if (found_regions > regions) {
        _large_runs.add(start + regions, found_regions - regions);
      }
    } else {
      size_t cur = Atomic::load(&_current_count_of_regions);
      if (cur + regions <= _count_of_regions) {
        start = cur;
        Atomic::store(&_current_count_of_regions, cur + regions);
      }
    }
  }
  if (start == SIZE_MAX) {
    Atomic::sub(&_real_memory_bytes, commit_bytes);
    return nullptr;
  }

  HeapWord* obj = _start + ((start * region_bytes) >> LogHeapWordSize);
  if (!os::commit_memory((char*)obj, commit_bytes, false)) {
    Atomic::sub(&_real_memory_bytes, commit_bytes);
    {
      MutexLocker ml(_region_space_lock, Mutex::_no_safepoint_check_flag);
      add_large_run(start, regions);
    }
    return nullptr;
  }

  HGCRegion* owner = new (0) HGCRegion(obj, 0, true, regions, nullptr);
  owner->set_large_object_bytes(bytes);
  owner->set_large_committed_bytes(commit_bytes);
  HGCHeapRegionRemSet* rs = new HGCHeapRegionRemSet(owner);
  rs->set_state_complete();
  rs->install_card_set(new HGCCardSet(_card_set_config, _card_set_free_pool));
  owner->set_rem_set(rs);

  Atomic::release_store(&_regions[start], owner);
  for (size_t j = 1; j < regions; ++j) {
    HeapWord* rstart = _start + (((start + j) * region_bytes) >> LogHeapWordSize);
    HGCRegion* follower = new (0) HGCRegion(rstart, 0, true, 0, owner);
    Atomic::release_store(&_regions[start + j], follower);
  }

  _card_table->mark_as_young(MemRegion(obj, (regions * region_bytes) >> LogHeapWordSize));
  owner->set_state(HGCRegion::New);
  _large_owners.push(owner);

  log_trace(gc, alloc)("HGC: large object " PTR_FORMAT ", %zu bytes, %zu regions [%zu,%zu), committed %zu",
                       p2i(obj), bytes, regions, start, start + regions, commit_bytes);
  if (actual_size != nullptr) {
    *actual_size = requested;
  }
  return obj;
}

void HGCCollectedHeap::add_large_run(size_t start, size_t regions) {

  _large_runs.add(start, regions);
}

void HGCCollectedHeap::free_large_run(HGCRegion* owner) {
  assert(owner != nullptr && owner->is_large_owner(), "not a large owner");
  const size_t start = region_index(owner->start());
  const size_t regions = owner->large_regions();
  const size_t committed = owner->large_committed_bytes();
  HeapWord* obj = owner->start();

  for (size_t j = 1; j < regions; ++j) {
    HGCRegion* follower = _regions[start + j];
    Atomic::release_store(&_regions[start + j], (HGCRegion*)nullptr);
    delete follower;
  }

  log_debug(gc, heap)("HGC: free large run #%zu regions=%zu bytes=%zu committed=%zu marked=%d",
                      start, regions, owner->large_object_bytes(),
                      committed, (int)owner->is_large_marked());
  HGCHeapRegionRemSet* rs = owner->rem_set();
  if (rs != nullptr) {
    rs->uninstall_card_set();
    owner->set_rem_set(nullptr);
    delete rs;
  }
  Atomic::release_store(&_regions[start], (HGCRegion*)nullptr);

  _card_table->mark_as_young(MemRegion(obj, (regions * HGCRegionSize) >> LogHeapWordSize));
  os::uncommit_memory((char*)obj, committed);
  Atomic::sub(&_real_memory_bytes, committed);
  delete owner;

  MutexLocker ml(_region_space_lock, Mutex::_no_safepoint_check_flag);
  add_large_run(start, regions);
}

void HGCCollectedHeap::add_region_to_cset(HGCRegion* region) {
  assert(region->state() != HGCRegion::Reclaiming,
         "allocation into a frozen region " PTR_FORMAT, p2i(region->start()));
  if (region->state() != HGCRegion::Surv) {
    return;
  }

  region->prepare_cards_for_allocation(_card_table);

  if (!region->claim_for_cset()) {
    return;
  }
  _cset.push(region);
  log_debug(gc, cset)("HGC cset region=%u", region_index(region->start()));
}

bool HGCCollectedHeap::is_young_card(HeapWord* p) const {
  return _card_table->is_in_young(p);
}

void HGCCollectedHeap::set_concurrent_marking(bool value) {
  _concurrent_marking = value;
}

void HGCCollectedHeap::mark_object_concurrently(oop obj) {
  if (!is_concurrent_marking()) {
    return;
  }

  HGCMarkClosure cl(_mark_queue_set->queue(0), false, 0, true ,
                    OopIterateClosure::DO_DISCOVERY, true );
  cl.mark_object(obj);
}

void HGCCollectedHeap::maybe_start_cycle() {

  if (!Universe::is_fully_initialized()) {
    return;
  }
  if (is_cycle_active() || _cset.size() <= _cset_policy->limit()) {
    return;
  }
  if (HGCForceSTW) {

    collect(GCCause::_allocation_failure);
    return;
  }
  collect(GCCause::_hgc_cset_complete);
}

size_t HGCCollectedHeap::k_by_requested_size(HGCArena::Alignments size_in_alignments) {

  size_t requested_bytes = size_in_alignments.to_bytes();
  if (requested_bytes >= HGCRegionSize) {
    return 0;
  }
  size_t arena_bytes = round_up_power_of_2(requested_bytes);
  return log2i(HGCRegionSize) - log2i(arena_bytes);
}

size_t HGCCollectedHeap::min_k() {
  return log2i(HGCRegionSize) - log2i(HGCMaxArenaSize);
}

size_t HGCCollectedHeap::max_k() {
  return log2i(HGCRegionSize) - log2i(HGCMinArenaSize);
}

HeapWord* HGCCollectedHeap::mem_allocate(size_t size,
    bool* gc_overhead_limit_was_exceeded) {
  assert(is_object_aligned(size), "not align");
  log_trace(gc, alloc)("HGC: mem_allocate %zu words", size);

  maybe_start_cycle();
  HGCArena::Alignments size_in_aligns = HGCArena::Alignments::from_words(size);
  HeapWord* res = InAligns::mem_allocate(this, size_in_aligns, gc_overhead_limit_was_exceeded);
  if (res == nullptr) {
    log_trace(gc, alloc)("HGC: allocation failed for %zu alignments (%zu bytes), triggering STW collection",
                         size_in_aligns.value(), size_in_aligns.to_bytes());
    collect(GCCause::_allocation_failure);
    res = InAligns::mem_allocate(this, size_in_aligns, gc_overhead_limit_was_exceeded);
    if (res == nullptr) {
      log_trace(gc, alloc)("HGC: out of memory: real=%zu limit=%zu",
                           real_memory_used(), hgc_real_memory_limit());
    }
  }
  return res;
}

HeapWord* HGCCollectedHeap::allocate_new_tlab(size_t min_size,
    size_t requested_size, size_t* actual_size) {
  assert(is_object_aligned(min_size), "not align");
  assert(is_object_aligned(requested_size), "not align");
  assert(min_size <= max_tlab_size(), "min_size > max_tlab_size()");

  maybe_start_cycle();

  auto cur = Thread::current();
  HGCArena* old_arena = HGCThreadLocalData::get_arena(cur);
  size_t old_tlab_size = HGCThreadLocalData::get_tlab_size(cur);
  if (old_tlab_size > 0) {
    HGCRegion* old_region = (old_arena != nullptr) ? old_arena->region() : nullptr;
    if (old_region != nullptr) {
      old_region->sub_tlab_size(old_tlab_size);
    }
    HGCThreadLocalData::set_tlab_size(cur, 0);
  }

  HGCArena::Alignments actual_size_in_aligns;

  HeapWord* res = InAligns::allocate(this,
      HGCArena::Alignments::from_words(min_size),
      HGCArena::Alignments::from_words(requested_size),
      &actual_size_in_aligns,
      false );

  *actual_size = actual_size_in_aligns.to_words();

  if (res != nullptr) {
    HGCRegion* new_region = region(res);
    assert(new_region != nullptr, "region must be not null");
    assert(new_region->state() != HGCRegion::Reclaiming,
           "TLAB chunk from a frozen region: region=%u", region_index(new_region->start()));
#ifdef ASSERT
    if (hgc_arena_diag_enabled()) {
      tty->print_cr("TLAB NEW: thread_id=%d start=" PTR_FORMAT " words=%zu region=%u state=%d",
                    cur->osthread()->thread_id(), p2i(res), actual_size_in_aligns.to_words(),
                    region_index(new_region->start()), (int)new_region->state());
    }
#endif
    new_region->add_tlab_size(actual_size_in_aligns.value());
    HGCThreadLocalData::set_tlab_size(cur, actual_size_in_aligns.value());
  }

  log_trace(gc, tlab)("HGC: allocate_new_tlab min=%zu requested=%zu actual=%zu words at " PTR_FORMAT,
                      min_size, requested_size, *actual_size, p2i(res));
  return res;
}

uint HGCCollectedHeap::region_index(HeapWord* ptr) const {
  return (uint)((ptr - _start) >> (log2i(HGCRegionSize) - LogHeapWordSize));
}

HGCRegion* HGCCollectedHeap::region(HeapWord* ptr) const {
  if (_start > ptr) return nullptr;
  size_t index = region_index(ptr);
  return index < _count_of_regions ? Atomic::load(&_regions[index]) : nullptr;
}

HGCArena* HGCCollectedHeap::arena(HeapWord* ptr) const {
  HGCRegion* r = region(ptr);
  return r != nullptr ? r->arena(ptr) : nullptr;
}

size_t HGCCollectedHeap::count_of_regions() const {
  return Atomic::load(&_current_count_of_regions);
}

void HGCCollectedHeap::release_region(HGCRegion* region) {
  assert_at_safepoint();
  const size_t ek = region->evristik_k();
  HGCRegionDoubleList& list = _kregions[ek];
  if (hgc_verify_enabled()) {
    if (!list.contains(region)) {
      tty->print_cr("RELEASE MISS: region=" PTR_FORMAT " start=" PTR_FORMAT " k=%zu ek=%zu state=%d",
                    p2i(region), p2i(region->start()), region->k(), ek, (int)region->state());
      for (size_t j = 0; j < max_num_of_k; ++j) {
        if (j != ek && _kregions[j].contains(region)) {
          tty->print_cr("  region is in list %zu", j);
        }
      }
    }
    assert(list.contains(region), "region " PTR_FORMAT " not in list %zu", p2i(region), ek);
  }
  list.remove(region);
}

void HGCCollectedHeap::add_region(HGCRegion* region) {
  assert_at_safepoint();
  assert(region->state() == HGCRegion::Surv,
         "region returned to the pool before it was rebuilt: state=%d", (int)region->state());
  assert(!region->is_large(), "large regions are not returned to the pool");

  if (region->is_empty()) {
    _empty_regions.push(region);
    return;
  }

  MutexLocker ml(_kregions_lock, Mutex::_no_safepoint_check_flag);
  _kregions[region->evristik_k()].push_back(region);
}

void HGCCollectedHeap::detach_empty_regions() {
  assert_at_safepoint();
  GrowableArray<HGCRegion*> batch(16, mtGC);
  while (HGCRegion* region = _empty_regions.pop()) {
    assert(region != nullptr && !region->is_large(), "only normal regions");
    assert(region->state() == HGCRegion::Surv, "region must be rebuilt");
    assert(region->tlab_size() == 0, "region still has TLAB accounting");
    batch.append(region);
  }
  if (batch.length() == 0) {
    return;
  }

  for (int i = 1; i < batch.length(); ++i) {
    HGCRegion* region = batch.at(i);
    const size_t index = region_index(region->start());
    int j = i - 1;
    while (j >= 0 && region_index(batch.at(j)->start()) > index) {
      batch.at_put(j + 1, batch.at(j));
      --j;
    }
    batch.at_put(j + 1, region);
  }

  {
    MonitorLocker ml(_uncommit_lock, Mutex::_no_safepoint_check_flag);
    for (int i = 0; i < batch.length(); ++i) {
      HGCRegion* region = batch.at(i);
      const size_t index = region_index(region->start());
      assert(_regions[index] == region, "region index %zu is not published", index);

      Atomic::release_store(&_regions[index], (HGCRegion*)nullptr);
      _uncommit_queue->append(region);
      log_debug(gc, heap)("HGC: detach empty region #%zu", index);
    }
  }
  if (_uncommit_thread != nullptr) {
    _uncommit_thread->request_work();
  }
}

void HGCCollectedHeap::uncommit_pending_regions() {
  for (;;) {
    HGCRegion* region = nullptr;
    {
      MonitorLocker ml(_uncommit_lock, Mutex::_no_safepoint_check_flag);
      if (_uncommit_queue->length() == 0) {
        return;
      }
      region = _uncommit_queue->at(0);
      _uncommit_queue->remove_at(0);
    }
    assert(!region->is_large(), "only normal regions");
    assert(region->state() == HGCRegion::Surv, "region must be rebuilt");
    assert(region->tlab_size() == 0, "region still has TLAB accounting");
    assert(!region->pages_uncommitted(), "region already uncommitted");

    const size_t index = region_index(region->start());
    log_debug(gc, heap)("HGC: uncommit region #%zu start=" PTR_FORMAT
                        " used=%zu tlab=%zu state=%d",
                        index, p2i(region->start()), region->used(),
                        region->tlab_size(), (int)region->state());
    HGCHeapRegionRemSet* rs = region->rem_set();
    if (rs != nullptr) {
      rs->clear(false, true);
      rs->uninstall_card_set();
      region->set_rem_set(nullptr);
      delete rs;
    }

    os::uncommit_memory((char*)(_start + ((index * HGCRegionSize) >> LogHeapWordSize)),
                        HGCRegionSize);
    Atomic::sub(&_real_memory_bytes, HGCRegionSize);
    Atomic::sub(&_committed_regions, (size_t)1);
    region->set_pages_uncommitted();
    {
      MonitorLocker ml(_uncommit_lock, Mutex::_no_safepoint_check_flag);
      _uncommitted_regions->append(region);
    }
    log_debug(gc, heap)("HGC: uncommitted region #%zu", index);
  }
}

void HGCCollectedHeap::reclaim_uncommitted_regions() {
  assert_at_safepoint();
  GrowableArray<HGCRegion*>* batch;
  {
    MonitorLocker ml(_uncommit_lock, Mutex::_no_safepoint_check_flag);
    batch = _uncommitted_regions;
    _uncommitted_regions = new (mtGC) GrowableArray<HGCRegion*>(16, mtGC);
  }
  for (int i = 0; i < batch->length(); ++i) {
    HGCRegion* region = batch->at(i);
    assert(region->pages_uncommitted(), "region pages are still committed");
    const size_t index = region_index(region->start());
    delete region;
    {
      MutexLocker ml(_region_space_lock, Mutex::_no_safepoint_check_flag);
      _free_region_indices->append(index);
    }
  }
  delete batch;
}

class HGCClearThreadLocalArenasClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) override {
    HGCArena* arena = HGCThreadLocalData::get_arena(thread);
    size_t tlab_size = HGCThreadLocalData::get_tlab_size(thread);
    if (tlab_size > 0) {
      HGCRegion* region = (arena != nullptr) ? arena->region() : nullptr;
      if (region != nullptr) {
        region->sub_tlab_size(tlab_size);
      }
      HGCThreadLocalData::set_tlab_size(thread, 0);
    }

    if (arena != nullptr) {
      HGCThreadLocalData::set_arena(thread, nullptr);
    }
  }
};

void HGCCollectedHeap::clear_thread_local_arenas() {
  HGCClearThreadLocalArenasClosure cl;
  Threads::threads_do(&cl);
}

CollectedHeap::Name HGCCollectedHeap::kind() const {
  return CollectedHeap::HGC;
}

const char* HGCCollectedHeap::name() const {
  return "HGC";
}

size_t HGCCollectedHeap::capacity() const {
  size_t committed = Atomic::load(&_real_memory_bytes);
  return MIN2(committed, MaxHeapSize);
}

size_t HGCCollectedHeap::max_capacity() const {
  return MaxHeapSize;
}

size_t HGCCollectedHeap::used() const {
  size_t result = 0;
  size_t count = Atomic::load(&_current_count_of_regions);
  for (size_t i = 0; i < count; ++i) {
    HGCRegion* region = _regions[i];
    if (region != nullptr) {
      result += region->used();
    }
  }
  return result;
}

size_t HGCCollectedHeap::real_memory_used() const {
  return Atomic::load(&_real_memory_bytes);
}

bool HGCCollectedHeap::is_in(const void* p) const {
  auto r = region(const_cast<HeapWord*>(reinterpret_cast<const HeapWord*>(p)));
  return r != nullptr;
}

size_t HGCCollectedHeap::tlab_capacity(Thread* ignored) const {
  size_t live = used();
  return live < MaxHeapSize ? MaxHeapSize - live : 0;
}

size_t HGCCollectedHeap::tlab_used(Thread* ignored) const {
  return used();
}

size_t HGCCollectedHeap::unsafe_max_tlab_alloc(Thread* ignored) const {
  return (HGCMaxArenaSize >> 1) - MinObjAlignmentInBytes;
}

size_t HGCCollectedHeap::max_tlab_size() const {
  return unsafe_max_tlab_alloc(nullptr) / HeapWordSize;
}

HGCCollectedHeap* HGCCollectedHeap::heap() {
  return named_heap<HGCCollectedHeap>(CollectedHeap::HGC);
}

ReferenceProcessor* HGCCollectedHeap::ref_processor() const {
  if (_ref_processor == nullptr) {

    HGCCollectedHeap* self = const_cast<HGCCollectedHeap*>(this);
    self->_ref_processor = new ReferenceProcessor(&self->_always_true, ParallelGCThreads,
                                                   ParallelGCThreads, true );
    self->_ref_discoverer.set_ref_processor(self->_ref_processor);
  }
  return _ref_processor;
}

HGCRefDiscoverer* HGCCollectedHeap::ref_discoverer() {
  ref_processor();
  return &_ref_discoverer;
}

HGCRefDiscoverer::HGCRefDiscoverer() :
    _rp(nullptr),
    _discovered(8, mtGC),
    _lock(new Mutex(Mutex::nosafepoint, "HGC ref discoverer")) {
}

HGCRefDiscoverer::~HGCRefDiscoverer() {
  delete _lock;
}

void HGCRefDiscoverer::start_discovery() {
  MutexLocker ml(_lock, Mutex::_no_safepoint_check_flag);
  _discovered.clear();
}

bool HGCRefDiscoverer::discover_reference(oop obj, ReferenceType rt) {
  assert(_rp != nullptr, "reference processor not initialized");

  oop referent = java_lang_ref_Reference::unknown_referent_no_keepalive(obj);
  if (referent == nullptr) {
    return false;
  }

  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  HeapWord* referent_addr = cast_from_oop<HeapWord*>(referent);
  if (heap->region(referent_addr) == nullptr || !oopDesc::is_oop(referent, true)) {
    java_lang_ref_Reference::clear_referent_raw(obj);
    return false;
  }
  ResourceMark rm;
  {
    MutexLocker ml(_lock, Mutex::_no_safepoint_check_flag);

    if (java_lang_ref_Reference::discovered(obj) != nullptr ||
        _discovered.contains(obj)) {
      return true;
    }
    _discovered.append(obj);
  }

  bool discovered = _rp->discover_reference(obj, rt);
  return discovered;
}

size_t HGCCollectedHeap::cset_limit() const {
  return _cset_policy->limit();
}

void HGCCollectedHeap::cset_remark_started() {
  _cset_policy->remark_started();
}

void HGCCollectedHeap::cset_remark_finished() {
  _cset_policy->remark_finished();
}

void HGCCollectedHeap::start_current_cset() {
  assert_at_safepoint();
  assert(_snapshot_of_cset.size() == 0, "the previous cycle's collection set was not consumed");
  _snapshot_of_cset = _cset;
  _cset = HGCVolatileRegionList();

  while (HGCRegion* owner = _large_owners.pop()) {
    _snapshot_of_cset.push(owner);
  }
  for (HGCRegion* region : _snapshot_of_cset) {
    region->set_state(HGCRegion::Reclaiming);
  }
}

void HGCCollectedHeap::fill_cset_all() {
  assert_at_safepoint();
  assert(!_cycle_active, "full collection must wait for the concurrent cycle");
  assert(_snapshot_of_cset.size() == 0, "the previous cycle's collection set was not consumed");
  _cset = HGCVolatileRegionList();

  while (_large_owners.pop() != nullptr) {
  }
  size_t count = count_of_regions();
  for (size_t i = 0; i < count; ++i) {
    HGCRegion* region = _regions[i];
    if (region == nullptr || region->is_large_continuation()) {
      continue;
    }
    region->set_state(HGCRegion::Reclaiming);
    _snapshot_of_cset.push(region);
  }
}

HGCRegion* HGCCollectedHeap::pop_cset() {
  return _snapshot_of_cset.pop();
}

void HGCCollectedHeap::start_cycle() {
  assert_at_safepoint();
  assert(!_cycle_active, "cycle already active");
  _cycle_active = true;
  _cset_policy->cycle_started(_snapshot_of_cset.size());
}

void HGCCollectedHeap::end_cycle() {
  MonitorLocker ml(_cycle_end_lock, Mutex::_no_safepoint_check_flag);
  _concurrent_marking = false;
  _cycle_active = false;
  _cset_policy->cycle_finished(_count_of_regions);
  ml.notify_all();
}

void HGCCollectedHeap::wait_for_cycle_end() {
  MonitorLocker ml(_cycle_end_lock, Mutex::_safepoint_check_flag);
  while (_cycle_active) {
    ml.wait();
  }
}

void HGCCollectedHeap::collect(GCCause::Cause cause) {
  switch (cause) {
    case GCCause::_metadata_GC_threshold:
    case GCCause::_metadata_GC_clear_soft_refs:
      assert_at_safepoint();
      MetaspaceGC::compute_new_size();
      break;
    case GCCause::_hgc_cset_complete: {
      if (count_of_regions() == 0 || is_cycle_active()) {
        break;
      }
      HGCInitialMark op;
      VMThread::execute(&op);
      break;
    }
    default: {
      if (count_of_regions() == 0) {
        break;
      }
      if (is_cycle_active()) {
        if (SafepointSynchronize::is_at_safepoint()) {
          break;
        }
        GCTraceTime(Debug, gc, phases) t("Wait for cycle end");
        wait_for_cycle_end();
      }
      HGCMarkSweep op(true);
      VMThread::execute(&op);
    }
  }
}

void HGCCollectedHeap::do_full_collection(bool clear_all_soft_refs) {
  _clear_all_soft_refs = clear_all_soft_refs ||
                         soft_ref_policy()->should_clear_all_soft_refs();
  soft_ref_policy()->set_should_clear_all_soft_refs(false);
  collect(gc_cause());
  if (_clear_all_soft_refs) {
    soft_ref_policy()->cleared_all_soft_refs();
  }
  _clear_all_soft_refs = false;
}

void HGCCollectedHeap::stop() {
  if (_stopped) {
    return;
  }
  _stopped = true;

  if (_refinement != nullptr) {
    _refinement->stop();
  }
  if (_uncommit_thread != nullptr) {
    _uncommit_thread->stop();
  }
  if (_cm_thread != nullptr) {
    _cm_thread->stop();
  }
}

void HGCCollectedHeap::safepoint_synchronize_begin() {

  SuspendibleThreadSet::synchronize();
}

void HGCCollectedHeap::safepoint_synchronize_end() {
  SuspendibleThreadSet::desynchronize();
}

void HGCCollectedHeap::initialize_serviceability() {
}

GrowableArray<GCMemoryManager*> HGCCollectedHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> managers(1);
  return managers;
}

GrowableArray<MemoryPool*> HGCCollectedHeap::memory_pools() {
  GrowableArray<MemoryPool*> pools(1);
  return pools;
}

bool HGCCollectedHeap::requires_barriers(stackChunkOop obj) const {
  return true;
}

namespace {
class HGCStartsToObjects {
  ObjectClosure* _cl;
public:
  explicit HGCStartsToObjects(ObjectClosure* cl) : _cl(cl) {}
  void do_holder(oop obj) { _cl->do_object(obj); }
};

class HGCArenaObjectClosure : public Closure {
  ObjectClosure* _cl;
public:
  explicit HGCArenaObjectClosure(ObjectClosure* cl) : _cl(cl) {}
  void do_arena(HGCArena* arena) {
    HGCStartsToObjects starts(_cl);
    arena->oops_starts_do(&starts);
  }
};

class HGCVerifyOopClosure : public BasicOopIterateClosure {
  oop _holder;

  template <class T>
  void do_oop_work(T* p) {
    oop value = RawAccess<>::oop_load(p);
    if (value == nullptr || oopDesc::is_oop(value, true)) {
      return;
    }
    HGCCollectedHeap* heap = HGCCollectedHeap::heap();
    HGCRegion* region = heap->region(cast_from_oop<HeapWord*>(_holder));
    tty->print_cr("HGC verify: bad reference " PTR_FORMAT " in holder " PTR_FORMAT
                  " region %d state %d field " PTR_FORMAT,
                  p2i(value), p2i(_holder),
                  region != nullptr ? (int)heap->region_index(region->start()) : -1,
                  region != nullptr ? (int)region->state() : -1, p2i(p));
    guarantee(false, "HGC verify: invalid oop reference");
  }

public:
  explicit HGCVerifyOopClosure(oop holder) : _holder(holder) {}
  void do_oop(oop* p) override { do_oop_work(p); }
  void do_oop(narrowOop* p) override { do_oop_work(p); }
};
}

void HGCCollectedHeap::object_iterate(ObjectClosure* cl) {
  const size_t count = count_of_regions();
  for (size_t i = 0; i < count; ++i) {
    HGCRegion* region = region_at(i);
    if (region == nullptr) {
      continue;
    }
    if (region->is_large()) {
      if (region->is_large_owner()) {
        cl->do_object(cast_to_oop(region->start()));
      }
    } else {
      HGCArenaObjectClosure arenas(cl);
      region->arenas_do(&arenas);
    }
  }
}

void HGCCollectedHeap::prepare_for_verify() {
}

void HGCCollectedHeap::verify_structural() {

  const size_t count = count_of_regions();
  for (size_t i = 0; i < count; ++i) {
    HGCRegion* region = region_at(i);
    if (region == nullptr || !region->is_large_owner()) {
      continue;
    }
    oop obj = cast_to_oop(region->start());
    guarantee(obj->size() * HeapWordSize <= region->large_object_bytes(),
              "HGC: large object exceeds its run at " PTR_FORMAT, p2i(obj));
  }

  for (size_t i = 0; i < count; ++i) {
    HGCRegion* region = region_at(i);
    if (region == nullptr || region->is_large_continuation()) {
      continue;
    }
    HeapWord* start = region->start();
    HeapWord* end = start + (region->is_large()
                               ? (region->large_regions() * (HGCRegionSize >> LogHeapWordSize))
                               : (HGCRegionSize >> LogHeapWordSize));
    for (CardTable::CardValue* card = _card_table->byte_for(start);
         card < _card_table->byte_after(end - 1); ++card) {
      CardTable::CardValue value = *card;
      guarantee(value == HGCCardTable::young_card_val() ||
                value == HGCCardTable::mixed_young_card_val() ||
                value == HGCCardTable::clean_card_val() ||
                value == HGCCardTable::dirty_card_val() ||
                value == HGCCardTable::mixed_clean_card_val() ||
                value == HGCCardTable::mixed_dirty_card_val(),
                "HGC: invalid card value %d for region %zu", (int)value, i);
    }
  }
  log_info(gc, verify)("HGC structural verification passed");
}

void HGCCollectedHeap::verify(VerifyOption option) {
  class VerifyObjectClosure : public ObjectClosure {
  public:
    void do_object(oop obj) override {
      guarantee(oopDesc::is_oop(obj, true), "HGC: invalid object at " PTR_FORMAT, p2i(obj));
      guarantee(Universe::heap()->is_in(cast_from_oop<HeapWord*>(obj)),
                "HGC: object outside the heap at " PTR_FORMAT, p2i(obj));
      HGCVerifyOopClosure refs(obj);
      obj->oop_iterate(&refs);
    }
  };

  VerifyObjectClosure cl;
  object_iterate(&cl);
  verify_structural();
}

void HGCCollectedHeap::gc_threads_do(ThreadClosure* tc) const {
  if (_cm_thread != nullptr) {
    tc->do_thread(_cm_thread);
  }
  if (_refinement != nullptr) {
    _refinement->threads_do(tc);
  }
}

void HGCCollectedHeap::print_gc_on(outputStream* st) const {
}

void HGCCollectedHeap::register_nmethod(nmethod* nm) {
  assert_locked_or_safepoint(CodeCache_lock);

  if (nm->gc_data<HGCnmethodData>() == nullptr) {
    nm->set_gc_data(new HGCnmethodData());
  }
}

void HGCCollectedHeap::unregister_nmethod(nmethod* nm) {
  assert_locked_or_safepoint(CodeCache_lock);
  HGCnmethodData* data = HGCnmethodData::for_nmethod(nm);
  if (data != nullptr) {
    nm->set_gc_data<HGCnmethodData>(nullptr);
    delete data;
  }
}

void HGCCollectedHeap::verify_nmethod(nmethod* nm) {
}

void HGCCollectedHeap::pin_object(JavaThread* thread, oop obj) {

  guarantee(is_in(cast_from_oop<HeapWord*>(obj)),
            "HGC: pin_object on an object outside the heap: " PTR_FORMAT, p2i(obj));
}

void HGCCollectedHeap::unpin_object(JavaThread* thread, oop obj) {
  guarantee(is_in(cast_from_oop<HeapWord*>(obj)),
            "HGC: unpin_object on an object outside the heap: " PTR_FORMAT, p2i(obj));
}

void HGCCollectedHeap::print_heap_on(outputStream* st) const {
  st->print_cr("HGC heap: reserved %zuK, committed %zuK, used %zuK",
               max_capacity() / K, capacity() / K, used() / K);
  st->print_cr("  region %zuK, regions %zu/%zu",
               HGCRegionSize / K, Atomic::load(&_committed_regions),
               _count_of_regions);
}

void HGCCollectedHeap::dump_heap_map() {
  static size_t dump_count = 0;
  ++dump_count;
  size_t count = Atomic::load(&_current_count_of_regions);
  tty->print_cr("{ \"gc\": %zu, \"region_size\": %zu, \"region_count\": %zu, \"alignment\": %d, \"regions\": [",
                dump_count, HGCRegionSize, count, MinObjAlignmentInBytes);
  bool printed = false;
  for (size_t i = 0; i < count; ++i) {
    HGCRegion* region = _regions[i];
    if (region == nullptr) continue;
    if (printed) {
      tty->print_cr(",");
    }
    region->print_map_on(tty, i);
    printed = true;
  }
  tty->print_cr("]}");
}

bool HGCCollectedHeap::print_location(outputStream* st, void* addr) const {
  if (!is_in(addr)) {
    return false;
  }
  HGCRegion* r = region(reinterpret_cast<HeapWord*>(addr));
  st->print_cr(PTR_FORMAT " is in HGC region " PTR_FORMAT " (k=%zu)",
               p2i(addr), p2i(r->start()), r->k());
  return true;
}

void HGCCollectedHeap::print_tracing_info() const {
  print_heap_on(tty);
}

