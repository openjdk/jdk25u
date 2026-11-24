#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hConcurrentRefine.hpp"
#include "gc/h/hConcurrentRefineStats.hpp"
#include "gc/h/hConcurrentRefineThread.hpp"
#include "gc/h/hDirtyCardQueue.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "logging/log.hpp"
#include "runtime/cpuTimeCounters.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ticks.hpp"

HGCConcurrentRefineThread::HGCConcurrentRefineThread(HGCConcurrentRefine* cr, uint worker_id) :
  ConcurrentGCThread(),
  _vtime_start(0.0),
  _vtime_accum(0.0),
  _notifier(Mutex::nosafepoint, FormatBuffer<>("HGC Refine#%d", worker_id), true),
  _requested_active(false),
  _refinement_stats(),
  _worker_id(worker_id),
  _cr(cr)
{
  set_name("HGC Refine#%d", worker_id);
}

void HGCConcurrentRefineThread::run_service() {
  _vtime_start = os::elapsedVTime();

  while (wait_for_completed_buffers()) {
    SuspendibleThreadSetJoiner sts_join;
    HGCConcurrentRefineStats active_stats_start = _refinement_stats;
    report_active("Activated");
    while (!should_terminate()) {
      if (sts_join.should_yield()) {
        report_inactive("Paused", _refinement_stats - active_stats_start);
        sts_join.yield();
        active_stats_start = _refinement_stats;
        report_active("Resumed");
      } else if (maybe_deactivate()) {
        break;
      } else {
        do_refinement_step();
      }
    }
    report_inactive("Deactivated", _refinement_stats - active_stats_start);
    track_usage();
  }

  log_debug(gc, refine)("Stopping %d", _worker_id);
}

void HGCConcurrentRefineThread::report_active(const char* reason) const {
  log_trace(gc, refine)("%s worker %u, current: %zu",
                        reason,
                        _worker_id,
                        HGCBarrierSet::dirty_card_queue_set().num_cards());
}

void HGCConcurrentRefineThread::report_inactive(const char* reason,
                                                const HGCConcurrentRefineStats& stats) const {
  log_trace(gc, refine)
           ("%s worker %u, cards: %zu, refined %zu, rate %1.2fc/ms",
            reason,
            _worker_id,
            HGCBarrierSet::dirty_card_queue_set().num_cards(),
            stats.refined_cards(),
            stats.refinement_rate_ms());
}

void HGCConcurrentRefineThread::activate() {
  if (this == Thread::current()) {

    return;
  }
  MonitorLocker ml(&_notifier, Mutex::_no_safepoint_check_flag);
  if (!_requested_active || should_terminate()) {
    _requested_active = true;
    ml.notify();
  }
}

bool HGCConcurrentRefineThread::maybe_deactivate() {
  assert(this == Thread::current(), "precondition");
  if (cr()->is_thread_wanted(_worker_id)) {
    return false;
  } else {
    MutexLocker ml(&_notifier, Mutex::_no_safepoint_check_flag);
    bool requested = _requested_active;
    _requested_active = false;
    return !requested;
  }
}

bool HGCConcurrentRefineThread::try_refinement_step(size_t stop_at) {
  assert(this == Thread::current(), "precondition");
  return _cr->try_refinement_step(_worker_id, stop_at, &_refinement_stats);
}

void HGCConcurrentRefineThread::stop_service() {
  activate();
}

class HGCPrimaryConcurrentRefineThread final : public HGCConcurrentRefineThread {
  bool wait_for_completed_buffers() override;
  bool maybe_deactivate() override;
  void do_refinement_step() override;
  void track_usage() override;

public:
  HGCPrimaryConcurrentRefineThread(HGCConcurrentRefine* cr) :
    HGCConcurrentRefineThread(cr, 0)
  {}
};

bool HGCPrimaryConcurrentRefineThread::wait_for_completed_buffers() {
  assert(this == Thread::current(), "precondition");
  MonitorLocker ml(notifier(), Mutex::_no_safepoint_check_flag);
  if (!requested_active() && !should_terminate()) {
    ml.wait(cr()->adjust_threads_wait_ms());
  }
  cr()->record_thread_adjustment_needed();
  return !should_terminate();
}

bool HGCPrimaryConcurrentRefineThread::maybe_deactivate() {
  return !cr()->is_thread_adjustment_needed() &&
         HGCConcurrentRefineThread::maybe_deactivate();
}

void HGCPrimaryConcurrentRefineThread::do_refinement_step() {
  if (!cr()->adjust_threads_periodically()) {
    if (!try_refinement_step(cr()->pending_cards_target())) {
      cr()->reduce_threads_wanted();
    }
  }
}

void HGCPrimaryConcurrentRefineThread::track_usage() {

  HGCConcurrentRefineThread::track_usage();
}

class HGCSecondaryConcurrentRefineThread final : public HGCConcurrentRefineThread {
  bool wait_for_completed_buffers() override;
  void do_refinement_step() override;

public:
  HGCSecondaryConcurrentRefineThread(HGCConcurrentRefine* cr, uint worker_id) :
    HGCConcurrentRefineThread(cr, worker_id)
  {
    assert(worker_id > 0, "precondition");
  }
};

bool HGCSecondaryConcurrentRefineThread::wait_for_completed_buffers() {
  assert(this == Thread::current(), "precondition");
  MonitorLocker ml(notifier(), Mutex::_no_safepoint_check_flag);
  while (!requested_active() && !should_terminate()) {
    ml.wait();
  }
  return !should_terminate();
}

void HGCSecondaryConcurrentRefineThread::do_refinement_step() {
  assert(this == Thread::current(), "precondition");
  try_refinement_step(0);
}

HGCConcurrentRefineThread*
HGCConcurrentRefineThread::create(HGCConcurrentRefine* cr, uint worker_id) {
  HGCConcurrentRefineThread* crt;
  if (worker_id == 0) {
    crt = new (std::nothrow) HGCPrimaryConcurrentRefineThread(cr);
  } else {
    crt = new (std::nothrow) HGCSecondaryConcurrentRefineThread(cr, worker_id);
  }
  if (crt != nullptr) {
    crt->create_and_start();
  }
  return crt;
}
