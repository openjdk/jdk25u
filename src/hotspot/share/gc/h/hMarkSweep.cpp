#include "gc/h/hMarkSweep.hpp"

#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeBehaviours.hpp"
#include "code/codeCache.hpp"
#include "code/dependencyContext.hpp"
#include "code/nmethod.hpp"
#include "gc/h/hArena.inline.hpp"
#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hConcurrentMarkThread.hpp"
#include "gc/h/hMarker.hpp"
#include "gc/h/hOopClosures.inline.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/h/hRegion.inline.hpp"
#include "gc/h/hRegionRemSet.inline.hpp"
#include "gc/h/hRemSet.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "gc/shared/classUnloadingContext.hpp"
#include "gc/shared/gcBehaviours.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/oopStorageSet.hpp"
#include "gc/shared/oopStorageSet.inline.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/referenceProcessorPhaseTimes.hpp"
#include "gc/shared/softRefPolicy.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/weakProcessor.hpp"
#include "jvmci/jvmci.hpp"
#include "logging/log.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/klass.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/handshake.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/growableArray.hpp"

volatile int hgc_cycle_id = 0;

#define HGC_TIME_CONCAT_(a, b) a##b
#define HGC_TIME_CONCAT(a, b) HGC_TIME_CONCAT_(a, b)
#define HGC_TIME(name) \
  GCTraceTime(Debug, gc, phases) HGC_TIME_CONCAT(hgc_trace_, __LINE__)(name)

static bool hgc_verify_all_enabled();
static bool hgc_verify_marks_enabled();
static void verify_marks(HGCCollectedHeap* heap);

class HGCIsAliveClosure : public BoolObjectClosure {
public:
  bool do_object_b(oop obj) override {
    if (obj == nullptr) {
      return false;
    }
    HeapWord* addr = cast_from_oop<HeapWord*>(obj);
    HGCCollectedHeap* heap = HGCCollectedHeap::heap();
    HGCRegion* region = heap->region(addr);
    if (region == nullptr) {

      return false;
    }
    if (region->state() != HGCRegion::Reclaiming) {
      return true;
    }
    if (region->is_large()) {

      return region->large_owner()->is_large_marked();
    }
    HGCArena* arena = heap->arena(addr);
    assert(arena != nullptr, "object is not in an arena");
    return arena->is_black(addr);
  }
};

static bool hgc_is_object_alive(oop obj) {
  HGCIsAliveClosure is_alive;
  return is_alive.do_object_b(obj);
}

class HGCIsUnloadingOopClosure : public OopClosure {
  bool _is_unloading;

public:
  HGCIsUnloadingOopClosure() : _is_unloading(false) {}

  void do_oop(oop* p) override {
    if (_is_unloading) {
      return;
    }
    oop obj = RawAccess<>::oop_load(p);
    if (obj != nullptr && !hgc_is_object_alive(obj)) {
      _is_unloading = true;
    }
  }

  void do_oop(narrowOop* p) override {
    ShouldNotReachHere();
  }

  bool is_unloading() const {
    return _is_unloading;
  }
};

class HGCIsUnloadingBehaviour : public IsUnloadingBehaviour {
public:
  bool has_dead_oop(nmethod* nm) const override {
    HGCIsUnloadingOopClosure cl;
    nm->oops_do(&cl);
    return cl.is_unloading();
  }
};

class HGCCompiledICProtectionBehaviour : public CompiledICProtectionBehaviour {
public:
  bool lock(nmethod* nm) override {
    HGCnmethodData* data = HGCnmethodData::for_nmethod(nm);
    assert(data != nullptr, "nmethod without HGC data");
    data->ic_lock()->lock();
    return true;
  }

  void unlock(nmethod* nm) override {
    HGCnmethodData* data = HGCnmethodData::for_nmethod(nm);
    assert(data != nullptr, "nmethod without HGC data");
    data->ic_lock()->unlock();
  }

  bool is_safe(nmethod* nm) override {
    if (SafepointSynchronize::is_at_safepoint() || nm->is_unloading()) {
      return true;
    }
    HGCnmethodData* data = HGCnmethodData::for_nmethod(nm);
    return data != nullptr && data->ic_lock()->owned_by_self();
  }
};

void hgc_initialize_class_unloading() {
  static HGCIsUnloadingBehaviour is_unloading_behaviour;
  static HGCCompiledICProtectionBehaviour ic_protection_behaviour;
  IsUnloadingBehaviour::set_current(&is_unloading_behaviour);
  CompiledICProtectionBehaviour::set_current(&ic_protection_behaviour);
}

class HGCNoOpVoidClosure : public VoidClosure {
public:
  void do_void() override {}
};

class HGCAliveCLDClosure : public CLDClosure {
  CLDToOopClosure* _cl;
  HGCIsAliveClosure* _is_alive;
public:
  HGCAliveCLDClosure(CLDToOopClosure* cl, HGCIsAliveClosure* is_alive) :
      _cl(cl), _is_alive(is_alive) {}
  void do_cld(ClassLoaderData* cld) override {
    oop holder = cld->holder_no_keepalive();
    if (holder == nullptr) {

      return;
    }
    if (_is_alive->do_object_b(holder)) {
      _cl->do_cld(cld);
    }
  }
};

static void hgc_scan_alive_clds(HGCCollectedHeap* heap, uint n_workers,
                                HGCIsAliveClosure& is_alive) {
  HGC_TIME("Alive CLD Scan");
  HGCMarkClosure cl(heap->mark_queue_set()->queue(0), false, 0, false,
                    OopIterateClosure::DO_DISCOVERY);
  cl.set_ref_discoverer(heap->ref_discoverer());
  CLDToOopClosure cld_cl(&cl, ClassLoaderData::_claim_none);
  HGCAliveCLDClosure alive(&cld_cl, &is_alive);
  {
    HGC_TIME("Alive CLDs: ClassLoaderDataGraph::cld_do");
    ClassLoaderDataGraph::cld_do(&alive);
  }
  HGCMarkTask drain(heap->mark_queue_set(), n_workers, false ,
                    heap->ref_discoverer(), OopIterateClosure::DO_DISCOVERY);
  {
    HGC_TIME("Alive CLDs: drain mark task");
    heap->workers()->run_task(&drain);
  }
}

class HGCRefProcProxyTask : public RefProcProxyTask {
  BoolObjectClosure& _is_alive;
  OopClosure& _keep_alive;
  VoidClosure& _complete_gc;

public:
  HGCRefProcProxyTask(BoolObjectClosure& is_alive, OopClosure& keep_alive,
                      VoidClosure& complete_gc, uint max_workers) :
      RefProcProxyTask("HGCRefProcProxyTask", max_workers),
      _is_alive(is_alive), _keep_alive(keep_alive), _complete_gc(complete_gc) {}

  void work(uint worker_id) override {
    assert(worker_id < _max_workers, "sanity");
    BarrierEnqueueDiscoveredFieldClosure enqueue;
    _rp_task->rp_work(worker_id, &_is_alive, &_keep_alive, &enqueue, &_complete_gc);
  }
};

static void hgc_process_references(HGCCollectedHeap* heap,
                                   HGCIsAliveClosure& is_alive,
                                   HGCMarkClosure& keep_alive) {
  ReferenceProcessor* rp = heap->ref_processor();
  if (rp == nullptr) {
    return;
  }
  HGC_TIME("Reference Processing");
  rp->set_active_mt_degree(ParallelGCThreads);
  GCTimer gc_timer;
  gc_timer.register_gc_start();
  gc_timer.register_gc_pause_start("HGC Mark", Ticks::now());
  gc_timer.register_gc_phase_start("Reference Processing", Ticks::now());
  {
    ReferenceProcessorPhaseTimes pt(&gc_timer, rp->max_num_queues());
    HGCNoOpVoidClosure complete;
    HGCRefProcProxyTask task(is_alive, keep_alive, complete, ParallelGCThreads);
    {
      HGC_TIME("Reference Processing: process_discovered_references");
      rp->process_discovered_references(task, pt);
    }
    {
      HGC_TIME("Reference Processing: print_reference_times");
      pt.print_all_references();
    }
  }
  gc_timer.register_gc_phase_end(Ticks::now());
  gc_timer.register_gc_pause_end(Ticks::now());
  gc_timer.register_gc_end();
}

static void hgc_unload_classes(HGCIsAliveClosure& is_alive) {
  if (!ClassUnloading) {
    return;
  }
  HGC_TIME("Class Unloading");
  ClassUnloadingContext ctx(1 ,
                            false ,
                            false );
  bool unloading_occurred;
  {
    CodeCache::UnlinkingScope scope(&is_alive);
    unloading_occurred = SystemDictionary::do_unloading(nullptr);
    CodeCache::do_unloading(unloading_occurred);
  }
  {
    HGC_TIME("Class Unloading: ctx.purge_nmethods");
    ctx.purge_nmethods();
  }
  {
    HGC_TIME("Class Unloading: ctx.free_nmethods");
    ctx.free_nmethods();
  }
  {
    HGC_TIME("Class Unloading: Klass::clean_weak_klass_links");
    Klass::clean_weak_klass_links(unloading_occurred);
  }
  JVMCI_ONLY({
    HGC_TIME("Class Unloading: JVMCI::do_unloading");
    JVMCI::do_unloading(unloading_occurred);
  });
  {
    HGC_TIME("Class Unloading: ClassLoaderDataGraph::purge");
    ClassLoaderDataGraph::purge(true );
  }
  DEBUG_ONLY({
    HGC_TIME("Class Unloading: MetaspaceUtils::verify");
    MetaspaceUtils::verify();
  });
}

class HGCRendezvousHandshakeClosure : public HandshakeClosure {
public:
  HGCRendezvousHandshakeClosure() : HandshakeClosure("HGCRendezvous") {}
  void do_thread(Thread*) override {}
};

void HGCMarkSweep::prepare_class_unloading() {
  if (!ClassUnloading) {
    return;
  }
  HGC_TIME("Prepare Class Unloading");
  CodeCache::increment_unloading_cycle();
  CodeCache::arm_all_nmethods();
  DependencyContext::cleaning_start();
}

class HGCUnlinkNMethodClosure : public NMethodClosure {
  bool _unloading_occurred;

public:
  explicit HGCUnlinkNMethodClosure(bool unloading_occurred) :
      _unloading_occurred(unloading_occurred) {}

  void do_nmethod(nmethod* nm) override {
    HGCnmethodData* data = HGCnmethodData::for_nmethod(nm);
    assert(data != nullptr, "nmethod without HGC data");

    if (nm->is_unloading()) {
      HGCReentrantLocker locker(data->lock());
      nm->unlink();
      return;
    }

    HGCReentrantLocker locker(data->ic_lock());
    nm->unload_nmethod_caches(_unloading_occurred);
  }
};

void HGCMarkSweep::unload_classes_concurrently() {
  if (!ClassUnloading) {
    return;
  }
  HGC_TIME("Concurrent Class Unloading");

  ClassUnloadingContext ctx(1 ,
                            true ,
                            true );
  bool unloading_occurred;
  {
    HGC_TIME("Concurrent Class Unloading: SystemDictionary");
    MutexLocker ml(ClassLoaderDataGraph_lock);
    unloading_occurred = SystemDictionary::do_unloading(nullptr);
  }
  {
    HGC_TIME("Concurrent Class Unloading: CodeCache");
    MutexLocker ml(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    HGCUnlinkNMethodClosure cl(unloading_occurred);
    CodeCache::nmethods_do(&cl);
  }
  {
    HGC_TIME("Concurrent Class Unloading: clean weak klass links");
    Klass::clean_weak_klass_links(unloading_occurred);
  }
  JVMCI_ONLY({
    HGC_TIME("Concurrent Class Unloading: JVMCI");
    JVMCI::do_unloading(unloading_occurred);
  });
  DependencyContext::cleaning_end();

  {
    HGC_TIME("Concurrent Class Unloading: rendezvous");
    HGCRendezvousHandshakeClosure cl;
    Handshake::execute(&cl);
  }

  {
    HGC_TIME("Concurrent Class Unloading: purge nmethods");
    ctx.purge_nmethods();
  }
  {
    HGC_TIME("Concurrent Class Unloading: free nmethods");
    ctx.free_nmethods();
  }
  {
    HGC_TIME("Concurrent Class Unloading: purge CLD");
    ClassLoaderDataGraph::purge(false );
  }
  CodeCache::purge_exception_caches();
  MetaspaceGC::compute_new_size();
  DEBUG_ONLY({
    HGC_TIME("Concurrent Class Unloading: MetaspaceUtils::verify");
    MetaspaceUtils::verify();
  });
}

class HGCTlabInRegionClosure : public ThreadClosure {
  HGCRegion* _region;
  bool _found;

public:
  explicit HGCTlabInRegionClosure(HGCRegion* region) : _region(region), _found(false) {}

  bool found() const { return _found; }

  virtual void do_thread(Thread* thread) override {
    if (!thread->is_Java_thread()) {
      return;
    }
    JavaThread* jt = JavaThread::cast(thread);
    HeapWord* start = jt->tlab().start();
    if (start == nullptr) {
      return;
    }
    HeapWord* region_start = _region->start();
    HeapWord* region_end = region_start + (HGCRegionSize >> LogHeapWordSize);
    if (start >= region_start && start < region_end) {
      _found = true;
      tty->print_cr("SWEEP TLAB OVERLAP: cycle=%d region=%u thread_id=%d start=" PTR_FORMAT
                    " top=" PTR_FORMAT " end=" PTR_FORMAT,
                    Atomic::load(&hgc_cycle_id),
                    HGCCollectedHeap::heap()->region_index(region_start),
                    jt->osthread()->thread_id(), p2i(start),
                    p2i(jt->tlab().top()), p2i(jt->tlab().end()));
    }
  }
};

class HGCSweepCsetTask : public WorkerTask {
  HGCCollectedHeap* _heap;
  bool _rebuild;

public:

  HGCSweepCsetTask(HGCCollectedHeap* heap, bool rebuild) :
      WorkerTask("HGC Sweep Cset"), _heap(heap), _rebuild(rebuild) {}

  virtual void work(uint worker_id) override {
    for (;;) {
      HGCRegion* region = _heap->pop_cset();
      if (region == nullptr) {
        return;
      }
      if (region->is_large()) {
        assert(region->is_large_owner(), "large continuation in the cset");
        assert(region->state() == HGCRegion::Reclaiming,
               "sweeping a large object that is not collecting: region=%u state=%d",
               _heap->region_index(region->start()), (int)region->state());
        if (region->is_large_marked()) {
          if (_rebuild) {
            region->translate_cards_after_rebuild(_heap->card_table(), false );
          }
          region->set_state(HGCRegion::Surv);
          _heap->add_large_owner(region);
        } else {
          _heap->free_large_run(region);
        }
        continue;
      }
      if (_rebuild) {
        assert(region->state() == HGCRegion::Reclaiming,
               "rebuilding a region that is not collecting: region=%u state=%d",
               _heap->region_index(region->start()), (int)region->state());
        HGCTlabInRegionClosure tlab_in_region(region);
        Threads::threads_do(&tlab_in_region);
        assert(!tlab_in_region.found(), "active TLAB inside a region being swept");
        region->rebuild_arenas();
        region->translate_cards_after_rebuild(_heap->card_table(), false );
      } else {
        assert(region->state() == HGCRegion::Surv,
               "releasing a region before the concurrent rebuild: region=%u state=%d",
               _heap->region_index(region->start()), (int)region->state());
      }

      region->clear_mark_bits();
      _heap->add_region(region);
    }
  }
};

class HGCMergeRemSetClosure {
  HGCCollectedHeap* _heap;
  HGCMarkClosure* _mark_cl;
  size_t _region_base_idx;
  bool _live_only;

public:
  HGCMergeRemSetClosure(HGCCollectedHeap* heap, HGCMarkClosure* mark_cl, bool live_only) :
    _heap(heap), _mark_cl(mark_cl), _region_base_idx(0), _live_only(live_only) {}

  bool start_iterate(uint tag, uint region_idx) {
    _region_base_idx = (size_t)region_idx << HGCRegion::LogCardsPerRegion;
    return true;
  }

  void do_card(uint card_idx) {
    size_t absolute_card_idx = _region_base_idx + card_idx;
    HGCCardTable* ct = _heap->card_table();
    HeapWord* start = ct->addr_for(ct->byte_for_index(absolute_card_idx));
    HGCRegion* region = _heap->region(start);
    if (region == nullptr) {
      return;
    }
    HeapWord* region_end = region->start() + (HGCRegionSize >> LogHeapWordSize);
    HeapWord* end = MIN2(start + HGCCardTable::card_size_in_words(), region_end);
    hgc_diag_ring_push(hgc_cards_merged, &hgc_cards_merged_idx,
                       (uintptr_t)ct->byte_for_index(absolute_card_idx));
    if (region->is_large()) {
      HGCRegion* owner = region->large_owner();
      if (_live_only && !owner->is_large_marked()) {
        return;
      }
      oop obj = cast_to_oop(owner->start());
      obj->oop_iterate(_mark_cl, MemRegion(start, end));
      return;
    }
    HGCArena* arena = _heap->arena(start);
    if (arena == nullptr) {
      return;
    }
    arena->oops_on_memregion(start, end, _mark_cl, _live_only);
  }

  void do_card_range(uint start_card_idx, uint length) {
    for (uint i = 0; i < length; ++i) {
      do_card(start_card_idx + i);
    }
  }
};

template <typename Closure>
class HGCScanArenasClosure {
  Closure* _cl;

public:
  explicit HGCScanArenasClosure(Closure* cl) : _cl(cl) {}

  void do_arena(HGCArena* arena) {
    HeapWord* start = arena->start();
    arena->oops_on_memregion(start, start + (arena->size() >> LogHeapWordSize), _cl);
  }

  void do_region(HGCRegion* region) {
    if (region->is_large()) {
      if (region->is_large_owner()) {
        oop obj = cast_to_oop(region->start());
        obj->oop_iterate(_cl);
      }
      return;
    }
    region->arenas_do(this);
  }
};

class HGCRebuildRemSetTask : public WorkerTask {
  HGCCollectedHeap* _heap;
  volatile size_t _next;

public:
  explicit HGCRebuildRemSetTask(HGCCollectedHeap* heap) :
      WorkerTask("HGC Rebuild RemSet"), _heap(heap), _next(0) {}

  virtual void work(uint worker_id) override {
    HGCRebuildRemSetClosure cl(_heap, worker_id);
    HGCScanArenasClosure<HGCRebuildRemSetClosure> scan(&cl);
    for (;;) {
      size_t i = Atomic::fetch_then_add(&_next, size_t(1));
      if (i >= _heap->count_of_regions()) {
        return;
      }
      HGCRegion* region = _heap->region_at(i);
      if (region != nullptr && !region->is_large_continuation()) {
        scan.do_region(region);
      }
    }
  }
};

class HGCFlushSATBQueuesClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) override {
    SATBMarkQueue& queue = HGCThreadLocalData::satb_mark_queue(thread);
    HGCBarrierSet::satb_mark_queue_set().flush_queue(queue);
  }
};

static void hgc_drain_satb_buffers(HGCMarkQueue* queue) {
  HGC_TIME("SATB Drain");
  HGCBatchedSATBBufferClosure drain(queue, HGCCollectedHeap::heap()->ref_discoverer());
  HGCSATBMarkQueueSet& queue_set = HGCBarrierSet::satb_mark_queue_set();
  while (queue_set.apply_closure_to_completed_buffer(&drain)) {
  }
}

void HGCMarkSweep::rebuild_rem_sets() {
  HGC_TIME("Rebuild Remsets");
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  size_t count = heap->count_of_regions();
  {
    HGC_TIME("Rebuild Remsets: clear/update regions");
    for (size_t i = 0; i < count; ++i) {
      HGCRegion* region = heap->region_at(i);
      if (region == nullptr || region->is_large_continuation()) {
        continue;
      }
      HGCHeapRegionRemSet* rem_set = region->rem_set();
      if (rem_set == nullptr) {
        continue;
      }
      rem_set->clear();
      rem_set->set_state_updating();
    }
  }

  heap->workers()->set_active_workers(ParallelGCThreads);
  HGCRebuildRemSetTask task(heap);
  {
    HGC_TIME("Rebuild Remsets: HGCRebuildRemSetTask");
    heap->workers()->run_task(&task);
  }

  {
    HGC_TIME("Rebuild Remsets: complete regions");
    for (size_t i = 0; i < count; ++i) {
      HGCRegion* region = heap->region_at(i);
      if (region == nullptr || region->is_large_continuation()) {
        continue;
      }
      HGCHeapRegionRemSet* rem_set = region->rem_set();
      if (rem_set != nullptr) {
        rem_set->set_state_complete();
      }
    }
  }
}

class HGCScanRemSetsTask : public WorkerTask {
  HGCCollectedHeap* _heap;
  HGCRegion** _regions;
  size_t _count;
  volatile size_t _next;
  bool _live_only;
  bool _at_safepoint;

public:
  HGCScanRemSetsTask(HGCCollectedHeap* heap, HGCRegion** regions, size_t count,
                     bool live_only, bool at_safepoint) :
      WorkerTask("HGC Scan RemSets"), _heap(heap), _regions(regions), _count(count),
      _next(0), _live_only(live_only), _at_safepoint(at_safepoint) {}

  virtual void work(uint worker_id) override {
    HGCMarkClosure cl(_heap->mark_queue_set()->queue(worker_id), false, worker_id,
                      true , OopIterateClosure::DO_FIELDS);
    HGCMergeRemSetClosure merge(_heap, &cl, _live_only);
    for (;;) {
      size_t i = Atomic::fetch_then_add(&_next, size_t(1));
      if (i >= _count) {
        return;
      }
      HGCHeapRegionRemSet* rem_set = _regions[i]->rem_set();
      if (rem_set != nullptr) {
        rem_set->iterate_for_merge(merge, _at_safepoint);
      }
    }
  }
};

void HGCMarkSweep::scan_rem_sets(bool live_only, bool at_safepoint) {
  HGC_TIME("Scan Remsets");
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();

  GrowableArray<HGCRegion*> regions;
  for (HGCRegion* region : heap->snapshot_of_cset()) {
    HGCHeapRegionRemSet* rem_set = region->rem_set();

    if ((rem_set != nullptr) && rem_set->has_card_set()) {
      rem_set->reset_table_scanner();
      regions.append(region);
    }
  }

  heap->workers()->set_active_workers(at_safepoint ? ParallelGCThreads : ConcGCThreads);
  HGCRegion** regions_data = regions.length() == 0 ? nullptr : regions.adr_at(0);
  HGCScanRemSetsTask task(heap, regions_data, regions.length(), live_only, at_safepoint);
  heap->workers()->run_task(&task);
}

class HGCFlushDirtyCardQueuesClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) override {
    HGCDirtyCardQueue& queue = HGCThreadLocalData::dirty_card_queue(thread);
    HGCBarrierSet::dirty_card_queue_set().flush_queue(queue);
  }
};

void HGCMarkSweep::refine_dirty_cards() {
  HGC_TIME("Refine Dirty Cards");

  HGCFlushDirtyCardQueuesClosure flush;
  {
    HGC_TIME("Refine Cards: flush thread queues");
    Threads::threads_do(&flush);
  }

  HGCDirtyCardQueueSet& queue_set = HGCBarrierSet::dirty_card_queue_set();

  {
    HGC_TIME("Refine Cards: enqueue paused buffers");
    queue_set.enqueue_all_paused_buffers();
  }
  {
    HGC_TIME("Refine Cards: drain completed buffers");
    const uint worker_id = HGCDirtyCardQueueSet::num_par_ids();
    HGCConcurrentRefineStats stats;
    while (queue_set.refine_completed_buffer_concurrently(worker_id, 0, &stats, false)) {
    }
  }
}

void HGCMarkSweep::doit() {
  assert_at_safepoint_on_vm_thread();
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  HGC_TIME("Full GC");

  if (_full) {
    if (heap->is_cycle_active()) {

      log_trace(gc, mark)("HGC: full collection skipped, a cycle is active");
      return;
    }
    if (VerifyBeforeGC) {
      heap->verify_structural();
    }
    {
      HGC_TIME("Full GC: fill_cset_all");
      heap->fill_cset_all();
    }
  }

  log_debug(gc, phases)("HGC full collect begin, cset=%zu, used=%zu bytes",
                        heap->snapshot_of_cset().size(), heap->used());
  log_trace(gc, mark)("HGC: collect begin, cset=%zu, used=%zu bytes", heap->snapshot_of_cset().size(), heap->used());

  {
    HGC_TIME("Full GC: prepare/release cset");
    for (HGCRegion* region : heap->snapshot_of_cset()) {
      region->prepare_for_mark();
      if (!region->is_large()) {
        heap->release_region(region);
      }
    }
  }
  {
    HGC_TIME("Full GC: clear_thread_local_arenas");
    heap->clear_thread_local_arenas();
  }

  {
    HGC_TIME("Full GC: ensure_parsability");
    heap->ensure_parsability(true);
  }
  heap->mark_queue_set()->clear();
  mark_live();

  for (HGCRegion* region : heap->snapshot_of_cset()) {
    region->invalidate_start_bitset();
  }
  sweep();
  rebuild_rem_sets();

  if (VerifyAfterGC) {
    Universe::verify("After GC");
  }

  if (HGCDumpHeapMap) {
    HGC_TIME("Full GC: dump_heap_map");
    heap->dump_heap_map();
  }

  log_debug(gc, phases)("HGC full collect end, used=%zu bytes", heap->used());
  log_trace(gc, mark)("HGC: collect end, used=%zu bytes", heap->used());
}

void HGCMarkSweep::mark_live() {
  HGC_TIME("Mark Live");
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  uint n_workers = heap->workers()->set_active_workers(ParallelGCThreads);
  ReferenceProcessor* rp = heap->ref_processor();
  bool clear_soft = heap->clear_all_soft_refs() ||
                    heap->soft_ref_policy()->should_clear_all_soft_refs();
  HGCMarkTask task(heap->mark_queue_set(), n_workers, false ,
                   heap->ref_discoverer());
  log_trace(gc, mark)("HGC: mark begin, %u workers", n_workers);
  if (rp != nullptr) {
    HGC_TIME("Mark Live: start_discovery");
    rp->start_discovery(clear_soft);
    heap->ref_discoverer()->start_discovery();
  }
  process_roots();
  {
    HGC_TIME("Mark Live: mark task");
    heap->workers()->run_task(&task);
  }

  HGCIsAliveClosure is_alive;
  hgc_scan_alive_clds(heap, n_workers, is_alive);
  HGCMarkClosure keep_alive(heap->mark_queue_set()->queue(0), false, 0, false,
                            OopIterateClosure::DO_FIELDS_EXCEPT_REFERENT);
  log_trace(gc, mark)("HGC: reference processing");
  hgc_process_references(heap, is_alive, keep_alive);
  {
    HGC_TIME("Mark Live: drain mark task");
    HGCMarkTask drain(heap->mark_queue_set(), n_workers, false ,
                      nullptr, OopIterateClosure::DO_FIELDS_EXCEPT_REFERENT);
    heap->workers()->run_task(&drain);
  }

  log_trace(gc, mark)("HGC: weak oops");
  {
    HGC_TIME("Mark Live: WeakProcessor::weak_oops_do");
    WeakProcessor::weak_oops_do(&is_alive, &keep_alive);
  }
  {
    HGC_TIME("Mark Live: drain after weak oops");
    HGCMarkTask drain(heap->mark_queue_set(), n_workers, false ,
                      nullptr, OopIterateClosure::DO_FIELDS_EXCEPT_REFERENT);
    heap->workers()->run_task(&drain);
  }
  log_trace(gc, mark)("HGC: class unloading");
  hgc_unload_classes(is_alive);
  log_trace(gc, mark)("HGC: mark end");
}

void HGCMarkSweep::process_roots(bool scan_remsets) {
  HGC_TIME("Process Roots");
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  HGCMarkClosure cl(heap->mark_queue_set()->queue(0));
  cl.set_ref_discoverer(heap->ref_discoverer());
  StrongRootsScope srs(0);
  CLDToOopClosure cld_cl(&cl, ClassLoaderData::_claim_stw_fullgc_mark);
  MarkingNMethodClosure code_cl(&cl, !NMethodToOopClosure::FixRelocations, true);

  log_trace(gc, mark)("HGC: roots: CLD graph");
  {
    HGC_TIME("Roots: CLD graph");
    if (ClassUnloading) {

      ClassLoaderDataGraph::roots_cld_do(&cld_cl, &cld_cl);
    } else {
      ClassLoaderDataGraph::cld_do(&cld_cl);
    }
  }
  log_trace(gc, mark)("HGC: roots: threads");
  {
    HGC_TIME("Roots: threads");
    Threads::oops_do(&cl, &code_cl);
  }
  log_trace(gc, mark)("HGC: roots: code cache");
  {
    HGC_TIME("Roots: code cache");
    CodeCache::nmethods_do(&code_cl);
  }
  log_trace(gc, mark)("HGC: roots: strong oop storages");
  {
    HGC_TIME("Roots: strong oop storages");
    OopStorageSet::strong_oops_do(&cl);
  }
  if (scan_remsets) {
    log_trace(gc, mark)("HGC: roots: remsets");
    scan_rem_sets();
  }
  log_trace(gc, mark)("HGC: roots done");
}

void HGCMarkSweep::sweep() {
  HGC_TIME("Sweep");
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  log_trace(gc, mark)("HGC: sweep begin");
  heap->workers()->set_active_workers(ParallelGCThreads);
  HGCSweepCsetTask task(heap, true );

  {
    HGC_TIME("Sweep: HGCSweepCsetTask");
    heap->workers()->run_task(&task);
  }
  heap->reclaim_uncommitted_regions();
  heap->detach_empty_regions();
  {
    HGC_TIME("Sweep: clear_claimed_marks");
    ClassLoaderDataGraph::clear_claimed_marks();
  }
  log_trace(gc, mark)("HGC: sweep end, used=%zu bytes", heap->used());
}

class HGCTlabLeftoverClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) override {
    if (!thread->is_Java_thread()) {
      return;
    }
    JavaThread* jt = JavaThread::cast(thread);
    HeapWord* start = jt->tlab().start();
    if (!hgc_arena_diag_enabled()) {
      if (start == nullptr) {
        return;
      }
      tty->print_cr("TLAB LEFTOVER after initial mark: thread=" PTR_FORMAT " start=" PTR_FORMAT,
                    p2i(thread), p2i(start));
      return;
    }
    HGCCollectedHeap* heap = HGCCollectedHeap::heap();
    HGCRegion* region = start != nullptr ? heap->region(start) : nullptr;
    tty->print_cr("IM TLAB: cycle=%d thread_id=%d start=" PTR_FORMAT " top=" PTR_FORMAT
                  " end=" PTR_FORMAT " region=%d state=%d",
                  Atomic::load(&hgc_cycle_id),
                  jt->osthread()->thread_id(),
                  p2i(start), p2i(jt->tlab().top()), p2i(jt->tlab().end()),
                  region != nullptr ? (int)heap->region_index(region->start()) : -1,
                  region != nullptr ? (int)region->state() : -1);
  }
};

void HGCInitialMark::doit() {
  assert_at_safepoint_on_vm_thread();
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();

  if (heap->is_cycle_active()) {
    return;
  }

  if (VerifyBeforeGC) {
    heap->verify_structural();
  }

  HGC_TIME("Initial Mark");
  int cycle_id = Atomic::add(&hgc_cycle_id, 1);

  {
    HGC_TIME("Initial Mark: start_current_cset");
    heap->start_current_cset();
    heap->mark_queue_set()->clear();
  }
  heap->reclaim_uncommitted_regions();

  log_debug(gc, phases)("HGC initial mark begin, cset=%zu, used=%zu bytes",
                        heap->snapshot_of_cset().size(), heap->used());
  log_trace(gc, mark)("HGC: initial mark begin, cset=%zu, used=%zu bytes",
                      heap->snapshot_of_cset().size(), heap->used());
#ifdef ASSERT
  if (hgc_arena_diag_enabled()) {
    tty->print_cr("CYCLE %d: initial mark, cset=%zu", cycle_id, heap->snapshot_of_cset().size());
  }
#endif

  {
    HGC_TIME("Initial Mark: prepare/mark/release cset");
    for (HGCRegion* region : heap->snapshot_of_cset()) {
      region->prepare_for_mark();
      if (!region->is_large()) {
        heap->release_region(region);
      }
    }
  }
  {
    HGC_TIME("Initial Mark: clear_thread_local_arenas");
    heap->clear_thread_local_arenas();
  }
  {
    HGC_TIME("Initial Mark: ensure_parsability");
    heap->ensure_parsability(true);
  }
  {
    HGC_TIME("Initial Mark: TLAB leftover check");
    HGCTlabLeftoverClosure leftover;
    Threads::threads_do(&leftover);
  }

  {
    HGC_TIME("Initial Mark: satb activate");
    HGCBarrierSet::satb_mark_queue_set().set_active_all_threads(true, false);
  }
  heap->set_concurrent_marking(true);

  ReferenceProcessor* rp = heap->ref_processor();
  if (rp != nullptr) {
    HGC_TIME("Initial Mark: start_discovery");
    bool clear_soft = heap->clear_all_soft_refs() ||
                      heap->soft_ref_policy()->should_clear_all_soft_refs();
    rp->start_discovery(clear_soft);
    heap->ref_discoverer()->start_discovery();
  }

  HGCMarkSweep::process_roots(false );

  {
    HGC_TIME("Initial Mark: start_cycle");
    heap->start_cycle();
  }
  {
    HGC_TIME("Initial Mark: cm_thread request_cycle");
    heap->cm_thread()->request_cycle();
  }

  log_trace(gc, mark)("HGC: initial mark end");
  log_debug(gc, phases)("HGC initial mark end");
}

class HGCVerifyProbeOopClosure : public BasicOopIterateClosure {
public:
  virtual ReferenceIterationMode reference_iteration_mode() override {
    return DO_FIELDS;
  }

  template <typename T>
  void do_oop_work(T* p) {
    T o = RawAccess<MO_RELAXED>::oop_load(p);
    if (CompressedOops::is_null(o)) {
      return;
    }
    oop obj = CompressedOops::decode_not_null(o);
    Atomic::inc(&hgc_refine_oops);
    if (p == hgc_probe_field) {
      Atomic::inc(&hgc_probe_hits);
    }
    if (HGCInSameRegion(p, obj)) {
      Atomic::inc(&hgc_refine_same);
    }
  }

  virtual void do_oop(oop* p) override { do_oop_work(p); }
  virtual void do_oop(narrowOop* p) override { do_oop_work(p); }
};

class HGCVerifyMarksClosure : public BasicOopIterateClosure {
  oop _holder;
  bool _authoritative;

public:
  static uint _reported;
  static uint _invalid;
  static uint _same;
  static uint _nonauthoritative;

  HGCVerifyMarksClosure() : _holder(nullptr), _authoritative(true) {}

  void set_authoritative(bool value) { _authoritative = value; }

  virtual ReferenceIterationMode reference_iteration_mode() override {
    return DO_FIELDS;
  }

  void do_holder(oop holder) {
    _holder = holder;
    holder->oop_iterate(this);
  }

  template <typename T>
  void do_oop_work(T* p) {
    oop obj = RawAccess<MO_RELAXED>::oop_load(p);
    if (obj == nullptr) {
      return;
    }
    HGCCollectedHeap* heap = HGCCollectedHeap::heap();
    HeapWord* addr = cast_from_oop<HeapWord*>(obj);
    HGCRegion* region = heap->region(addr);
    if (region == nullptr) {
      return;
    }
    const bool is_obj = oopDesc::is_oop(obj, true);
    if (!is_obj) {

      if (region->state() != HGCRegion::Reclaiming) {
        return;
      }
      ++_invalid;
      if (!_authoritative) {
        ++_nonauthoritative;
      }
      if (_reported++ < 10) {
        print_report(heap, p, obj, region, false);
      }
    }
    if (region->state() != HGCRegion::Reclaiming) {
      return;
    }
    HGCArena* arena = heap->arena(addr);
    if (region->is_large()) {
      if (region->large_owner()->is_large_marked()) {
        return;
      }
    } else if (arena == nullptr || arena->is_black(addr)) {
      return;
    }

    if (HGCInSameRegion(p, obj)) {
      ++_same;
    }
    if (!_authoritative) {
      ++_nonauthoritative;
    }
    if (_reported++ < 10) {
      print_report(heap, p, obj, region, true);
    }
  }

  void print_report(HGCCollectedHeap* heap, void* p, oop obj,
                    HGCRegion* region, bool is_obj) {
    ResourceMark rm;
    HGCRegion* hr = heap->region(cast_from_oop<HeapWord*>(_holder));
    CardValue* card = heap->card_table()->byte_for(p);
    CardValue card_value = *card;
    int card_kind = HGCCardTable::is_young_card_value(card_value) ? 0
                  : HGCCardTable::is_clean_card_value(card_value) ? 1 : 2;
    tty->print_cr("VERIFY: unmarked target: holder=" PTR_FORMAT " klass=%s field=" PTR_FORMAT
                  " target=" PTR_FORMAT " valid=%d region=%u state=%d"
                  " holder_region=%u holder_state=%d card=%s card_value=0x%02x"
                  " authority=%s",
                  p2i(_holder), _holder->klass()->external_name(), p2i(p),
                  p2i(obj), (int)is_obj,
                  heap->region_index(region->start()), (int)region->state(),
                  hr != nullptr ? heap->region_index(hr->start()) : 0,
                  hr != nullptr ? (int)hr->state() : -1,
                  card_kind == 0 ? "young" : card_kind == 1 ? "clean" : "dirty",
                  (unsigned)card_value,
                  _authoritative ? "authoritative" : "non-authoritative");
    if (!is_obj) {
      return;
    }
    HGCArena* ha = heap->arena(cast_from_oop<HeapWord*>(_holder));
    if (ha == nullptr) {

      return;
    }
    HGCHeapRegionRemSet* tr = region->rem_set();
    bool has_ref = (tr != nullptr) && tr->has_card_set() && tr->contains_reference(p);
    HeapWord* card_start = heap->card_table()->addr_for(card);
    HeapWord* card_end = MIN2(card_start + HGCCardTable::card_size_in_words(),
                              ha->start() + (ha->size() >> LogHeapWordSize));
    size_t oops_before = hgc_refine_oops;
    size_t same_before = hgc_refine_same;
    hgc_probe_field = p;
    hgc_probe_hits = 0;
    HGCVerifyProbeOopClosure probe;
    ha->oops_on_memregion(card_start, card_end, &probe);
    tty->print_cr("   probe: remset_has_ref=%d remset_occupied=%zu tracked=%d remset_state=%s has_card_set=%d"
                  " oops_on_card=%zu same_region=%zu"
                  " field_hits=%zu holder_black=%d holder_gray=%d",
                  (int)has_ref,
                  (tr != nullptr && tr->has_card_set()) ? tr->occupied() : 0,
                  tr != nullptr ? (int)tr->is_tracked() : -1,
                  tr != nullptr ? tr->get_short_state_str() : "-",
                  tr != nullptr ? (int)tr->has_card_set() : -1,
                  hgc_refine_oops - oops_before, hgc_refine_same - same_before,
                  hgc_probe_hits,
                  (int)ha->is_black(cast_from_oop<HeapWord*>(_holder)),
                  (int)ha->is_gray(cast_from_oop<HeapWord*>(_holder)));
    uintptr_t card_addr = (uintptr_t)card;
    tty->print_cr("   lifecycle: enqueued=%zu cleaned=%zu added=%zu merged=%zu writer=%d (totals %zu/%zu/%zu/%zu)",
                  hgc_diag_ring_count(hgc_cards_enqueued, &hgc_cards_enqueued_idx, card_addr),
                  hgc_diag_ring_count(hgc_cards_cleaned, &hgc_cards_cleaned_idx, card_addr),
                  hgc_diag_ring_count(hgc_cards_added, &hgc_cards_added_idx, card_addr),
                  hgc_diag_ring_count(hgc_cards_merged, &hgc_cards_merged_idx, card_addr),
                  hgc_card_writer_site(card),
                  (size_t)hgc_cards_enqueued_idx, (size_t)hgc_cards_cleaned_idx,
                  (size_t)hgc_cards_added_idx, (size_t)hgc_cards_merged_idx);
    hgc_probe_field = nullptr;
  }

  virtual void do_oop(oop* p) override { do_oop_work(p); }
  virtual void do_oop(narrowOop* p) override { do_oop_work(p); }
};

uint HGCVerifyMarksClosure::_reported = 0;
uint HGCVerifyMarksClosure::_invalid = 0;
uint HGCVerifyMarksClosure::_same = 0;
uint HGCVerifyMarksClosure::_nonauthoritative = 0;
class HGCVerifyArenasClosure {
  HGCVerifyMarksClosure* _cl;

public:
  explicit HGCVerifyArenasClosure(HGCVerifyMarksClosure* cl) : _cl(cl) {}

  void do_arena(HGCArena* arena) {
    _cl->set_authoritative(true);
    arena->oops_black_do(_cl);
  }
};

class HGCVerifyAllArenasClosure {
  HGCVerifyMarksClosure* _cl;

public:
  explicit HGCVerifyAllArenasClosure(HGCVerifyMarksClosure* cl) : _cl(cl) {}

  void do_arena(HGCArena* arena) {
    _cl->set_authoritative(false);
    arena->oops_starts_do(_cl);
  }
};

class HGCTlabCheckClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) override {
    if (!thread->is_Java_thread()) {
      return;
    }
    JavaThread* jt = JavaThread::cast(thread);
    HeapWord* start = jt->tlab().start();
    if (start == nullptr) {
      return;
    }
    HGCCollectedHeap* heap = HGCCollectedHeap::heap();
    HGCRegion* region = heap->region(start);
    if (region != nullptr && region->state() == HGCRegion::Reclaiming) {
      tty->print_cr("TLAB IN FROZEN REGION: cycle=%d thread=%s start=" PTR_FORMAT
                    " top=" PTR_FORMAT " end=" PTR_FORMAT " region=%u state=%d",
                    Atomic::load(&hgc_cycle_id),
                    thread->name(), p2i(start), p2i(jt->tlab().top()), p2i(jt->tlab().end()),
                    heap->region_index(region->start()), (int)region->state());
    }
  }
};

void HGCRemark::doit() {
  assert_at_safepoint_on_vm_thread();
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  assert(heap->is_cycle_active(), "remark without an active cycle");
  HGC_TIME("Remark");
  heap->cset_remark_started();
  log_debug(gc, phases)("HGC remark begin, cset=%zu, used=%zu bytes",
                        heap->snapshot_of_cset().size(), heap->used());

#ifdef ASSERT
  if (hgc_arena_diag_enabled()) {
    tty->print_cr("CYCLE %d: remark, cset=%zu", Atomic::load(&hgc_cycle_id),
                  heap->snapshot_of_cset().size());
  }
#endif

  log_trace(gc, mark)("HGC: remark begin");

  {
    HGC_TIME("Remark: flush SATB queues");
    HGCFlushSATBQueuesClosure flush;
    Threads::threads_do(&flush);
  }

  hgc_drain_satb_buffers(heap->mark_queue_set()->queue(0));
  log_trace(gc, mark)("HGC: remark flushed and drained");

  HGCMarkSweep::refine_dirty_cards();

  uint n_workers = heap->workers()->set_active_workers(ParallelGCThreads);
  HGCMarkTask task(heap->mark_queue_set(), n_workers, false ,
                   heap->ref_discoverer());
  {
    HGC_TIME("Remark: finish mark task");
    heap->workers()->run_task(&task);
  }

  {
    HGC_TIME("Remark: drain mark task");
    HGCMarkTask drain(heap->mark_queue_set(), n_workers, false ,
                      heap->ref_discoverer(), OopIterateClosure::DO_DISCOVERY);
    heap->workers()->run_task(&drain);
  }

  HGCIsAliveClosure is_alive;
  hgc_scan_alive_clds(heap, n_workers, is_alive);
  HGCMarkClosure keep_alive(heap->mark_queue_set()->queue(0), false, 0, false,
                            OopIterateClosure::DO_FIELDS_EXCEPT_REFERENT);
  log_trace(gc, mark)("HGC: reference processing");
  hgc_process_references(heap, is_alive, keep_alive);
  {
    HGC_TIME("Remark: drain after references");
    HGCMarkTask drain(heap->mark_queue_set(), n_workers, false ,
                      nullptr, OopIterateClosure::DO_FIELDS_EXCEPT_REFERENT);
    heap->workers()->run_task(&drain);
  }

  log_trace(gc, mark)("HGC: weak oops");
  {
    HGC_TIME("Remark: WeakProcessor::weak_oops_do");
    WeakProcessor::weak_oops_do(&is_alive, &keep_alive);
  }
  {
    HGC_TIME("Remark: drain after weak oops");
    HGCMarkTask drain(heap->mark_queue_set(), n_workers, false ,
                      nullptr, OopIterateClosure::DO_FIELDS_EXCEPT_REFERENT);
    heap->workers()->run_task(&drain);
  }
  log_trace(gc, mark)("HGC: prepare class unloading");
  HGCMarkSweep::prepare_class_unloading();
  {
    HGC_TIME("Remark: update start bitsets");
    for (HGCRegion* region : heap->snapshot_of_cset()) {
      region->invalidate_start_bitset();
    }
  }

  if (hgc_verify_marks_enabled()) {
    HGC_TIME("Remark: verify_marks");
    verify_marks(heap);
  }

  heap->set_concurrent_marking(false);
  {
    HGC_TIME("Remark: satb deactivate");
    HGCBarrierSet::satb_mark_queue_set().set_active_all_threads(false, true);
  }

  {
    HGC_TIME("Remark: TLAB check");
    HGCTlabCheckClosure tlab_check;
    Threads::threads_do(&tlab_check);
  }

  log_trace(gc, mark)("HGC: remark end, used=%zu bytes", heap->used());
  log_debug(gc, phases)("HGC remark end, used=%zu bytes", heap->used());
  heap->cset_remark_finished();
}

HGCConcurrentRebuildTask::HGCConcurrentRebuildTask(HGCCollectedHeap* heap) :
    WorkerTask("HGC Concurrent Rebuild"), _heap(heap), _next(0) {}

void HGCConcurrentRebuildTask::work(uint worker_id) {
  for (;;) {
    size_t i = Atomic::fetch_then_add(&_next, size_t(1));
    if (i >= _heap->count_of_regions()) {
      break;
    }
    HGCRegion* region = _heap->region_at(i);
    if (region == nullptr || region->state() != HGCRegion::Reclaiming) {
      continue;
    }
    if (region->is_large()) {

      if (region->is_large_owner() && region->is_large_marked()) {
        region->translate_cards_after_rebuild(_heap->card_table());
      }
      continue;
    }
    region->rebuild_arenas();
    region->translate_cards_after_rebuild(_heap->card_table());
  }

  HGCBarrierSet::dirty_card_queue_set().flush_queue(
      HGCThreadLocalData::dirty_card_queue(Thread::current()));
}

void HGCRelease::doit() {
  assert_at_safepoint_on_vm_thread();
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  HGC_TIME("Release");
  heap->workers()->set_active_workers(ParallelGCThreads);
  log_debug(gc, phases)("HGC release begin, used=%zu bytes", heap->used());

  HGCSweepCsetTask sweep(heap, false );

  {
    HGC_TIME("Release: HGCSweepCsetTask");
    heap->workers()->run_task(&sweep);
  }
  heap->reclaim_uncommitted_regions();
  heap->detach_empty_regions();
  {
    HGC_TIME("Release: clear_claimed_marks");
    ClassLoaderDataGraph::clear_claimed_marks();
  }
  if (VerifyAfterGC) {
    heap->verify_structural();
  }
  if (HGCDumpHeapMap) {
    HGC_TIME("Release: dump_heap_map");
    heap->dump_heap_map();
  }
  log_trace(gc, mark)("HGC: release end, used=%zu bytes", heap->used());
  log_debug(gc, phases)("HGC release end, used=%zu bytes", heap->used());
}

static bool hgc_verify_all_enabled() {
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_VERIFY_ALL") == nullptr) ? 0 : 1;
  }
  return value == 1;
}

static bool hgc_verify_marks_enabled() {
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_VERIFY_MARKS") != nullptr ||
             ::getenv("HGC_VERIFY_ALL") != nullptr) ? 1 : 0;
  }
  return value == 1;
}

static void verify_marks(HGCCollectedHeap* heap) {
  HGCVerifyMarksClosure::_reported = 0;
  HGCVerifyMarksClosure::_invalid = 0;
  HGCVerifyMarksClosure::_same = 0;
  HGCVerifyMarksClosure::_nonauthoritative = 0;
  HGCVerifyMarksClosure cl;
  HGCVerifyArenasClosure scan(&cl);
  for (HGCRegion* region : heap->snapshot_of_cset()) {
    region->arenas_do(&scan);
  }
  if (hgc_verify_all_enabled()) {
    HGCVerifyAllArenasClosure scan_all(&cl);
    for (size_t i = 0; i < heap->count_of_regions(); ++i) {
      HGCRegion* region = heap->region_at(i);
      if (region == nullptr || region->state() == HGCRegion::Reclaiming) {
        continue;
      }
      region->arenas_do(&scan_all);
    }
  }
  if (HGCVerifyMarksClosure::_reported > 0 || HGCVerifyMarksClosure::_invalid > 0 ||
      HGCVerifyMarksClosure::_same > 0) {
    tty->print_cr("VERIFY: %u unmarked targets, %u invalid values, %u same-region,"
                  " non-authoritative=%u",
                  HGCVerifyMarksClosure::_reported,
                  HGCVerifyMarksClosure::_invalid,
                  HGCVerifyMarksClosure::_same,
                  HGCVerifyMarksClosure::_nonauthoritative);
  }
}

