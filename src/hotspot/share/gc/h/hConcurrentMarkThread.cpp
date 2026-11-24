#include "gc/h/hConcurrentMarkThread.hpp"

#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hConcurrentRefine.hpp"
#include "gc/h/hDirtyCardQueue.hpp"
#include "gc/h/hMarkSweep.hpp"
#include "gc/h/hMarker.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/handshake.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/vmThread.hpp"
#include "memory/resourceArea.hpp"

HGCConcurrentMarkThread::HGCConcurrentMarkThread(HGCCollectedHeap* heap) :
    ConcurrentGCThread(),
    _heap(heap),
    _cycle_requested(false) {
  set_name("HGC Main Marker");
  create_and_start();
}

bool HGCConcurrentMarkThread::wait_for_next_cycle() {
  MonitorLocker ml(_heap->cycle_lock(), Mutex::_no_safepoint_check_flag);
  while (!_cycle_requested && !should_terminate()) {
    ml.wait();
  }
  if (should_terminate()) {
    return false;
  }
  _cycle_requested = false;
  return true;
}

class HGCConvergeLocalQueuesClosure : public HandshakeClosure {
  size_t _dirty_cards;
  size_t _satb_cards;

public:
  HGCConvergeLocalQueuesClosure() :
      HandshakeClosure("HGC Converge Local Queues"),
      _dirty_cards(0), _satb_cards(0) {}

  void do_thread(Thread* thread) override {
    HGCDirtyCardQueue& dirty = HGCThreadLocalData::dirty_card_queue(thread);
    Atomic::add(&_dirty_cards, dirty.size());
    HGCBarrierSet::dirty_card_queue_set().flush_queue(dirty);

    SATBMarkQueue& satb = HGCThreadLocalData::satb_mark_queue(thread);
    Atomic::add(&_satb_cards, satb.size());
    HGCBarrierSet::satb_mark_queue_set().flush_queue(satb);
  }

  size_t dirty_cards() const { return Atomic::load(&_dirty_cards); }
  size_t satb_cards() const { return Atomic::load(&_satb_cards); }
};

void HGCConcurrentMarkThread::converge_local_queues() {

  static constexpr uint MaxRounds = 8;
  static constexpr size_t ResidualTarget = 1024;

  HGCMarkQueue* queue = _heap->mark_queue_set()->queue(0);
  HGCBatchedSATBBufferClosure satb_drain(queue, _heap->ref_discoverer());
  HGCSATBMarkQueueSet& satb_set = HGCBarrierSet::satb_mark_queue_set();
  HGCDirtyCardQueueSet& dirty_set = HGCBarrierSet::dirty_card_queue_set();
  const uint dirty_worker_id = HGCDirtyCardQueueSet::num_par_ids();
  const uint n_workers = _heap->workers()->set_active_workers(ConcGCThreads);

  for (uint round = 0; round < MaxRounds; ++round) {
    HGCConvergeLocalQueuesClosure flush;
    Handshake::execute(&flush);

    bool satb_drained = false;
    while (satb_set.apply_closure_to_completed_buffer(&satb_drain)) {
      satb_drained = true;
    }

    HGCConcurrentRefineStats dirty_stats;
    bool dirty_drained = false;
    while (dirty_set.refine_completed_buffer_no_paused(dirty_worker_id, &dirty_stats)) {
      dirty_drained = true;
    }

    if (satb_drained || dirty_drained || _heap->mark_queue_set()->has_pending_work()) {
      HGCMarkTask mark(_heap->mark_queue_set(), n_workers, true ,
                       _heap->ref_discoverer(), OopIterateClosure::DO_DISCOVERY);
      _heap->workers()->run_task(&mark);
    }

    const size_t flushed = flush.dirty_cards() + flush.satb_cards();
    log_debug(gc, phases)("HGC converge local queues: round=%u dirty=%zu satb=%zu flushed=%zu",
                          round, flush.dirty_cards(), flush.satb_cards(), flushed);
    if (flushed <= ResidualTarget) {
      break;
    }
  }
}

void HGCConcurrentMarkThread::concurrent_mark() {
  HGCMarkQueue* queue = _heap->mark_queue_set()->queue(0);
  HGCBatchedSATBBufferClosure drain(queue, _heap->ref_discoverer());
  HGCSATBMarkQueueSet& queue_set = HGCBarrierSet::satb_mark_queue_set();
  uint n_workers = _heap->workers()->set_active_workers(ConcGCThreads);
  HGCConcurrentRefine* refinement = _heap->refinement();

  {
    ResourceMark rm;
    refinement->pause_refinement();
    HGCMarkSweep::scan_rem_sets(false , false );
    refinement->resume_refinement();
  }

  for (;;) {
    log_trace(gc, mark)("HGC: concurrent mark task begin");
    HGCMarkTask task(_heap->mark_queue_set(), n_workers, true ,
                     _heap->ref_discoverer());
    _heap->workers()->run_task(&task);
    log_trace(gc, mark)("HGC: concurrent mark task done, draining");

    bool drained = false;
    while (queue_set.apply_closure_to_completed_buffer(&drain)) {
      drained = true;
    }
    const bool pending = _heap->mark_queue_set()->has_pending_work();
    log_trace(gc, mark)("HGC: concurrent mark drain done, drained=%d pending=%d", drained, pending);
    if (!drained && !pending) {
      break;
    }
  }

  converge_local_queues();
  log_trace(gc, mark)("HGC: concurrent mark done");
}

void HGCConcurrentMarkThread::request_remark() {
  log_trace(gc, mark)("HGC: request remark");
  HGCRemark op;
  VMThread::execute(&op);
  log_trace(gc, mark)("HGC: remark returned");
}

void HGCConcurrentMarkThread::concurrent_rebuild() {
  log_trace(gc, mark)("HGC: concurrent rebuild begin");
  _heap->workers()->set_active_workers(ConcGCThreads);
  HGCConcurrentRebuildTask task(_heap);
  _heap->workers()->run_task(&task);
  log_trace(gc, mark)("HGC: concurrent rebuild done");
}

void HGCConcurrentMarkThread::request_release() {
  log_trace(gc, mark)("HGC: request release");
  HGCRelease op;
  VMThread::execute(&op);
  log_trace(gc, mark)("HGC: release returned");
}

void HGCConcurrentMarkThread::run_service() {
  while (wait_for_next_cycle()) {
    concurrent_mark();
    request_remark();
    HGCMarkSweep::unload_classes_concurrently();
    concurrent_rebuild();
    request_release();
    _heap->end_cycle();
  }
}

void HGCConcurrentMarkThread::stop_service() {
  MonitorLocker ml(_heap->cycle_lock(), Mutex::_no_safepoint_check_flag);
  ml.notify_all();
}

void HGCConcurrentMarkThread::request_cycle() {
  MonitorLocker ml(_heap->cycle_lock(), Mutex::_no_safepoint_check_flag);
  _cycle_requested = true;
  ml.notify_all();
}
