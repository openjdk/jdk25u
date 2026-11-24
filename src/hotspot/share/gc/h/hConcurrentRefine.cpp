#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hConcurrentRefine.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/h/hConcurrentRefineThread.hpp"
#include "gc/h/hDirtyCardQueue.hpp"
#include "gc/h/hPolicy.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/h/hRegionRemSet.hpp"
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.hpp"
#include "runtime/java.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <math.h>

HGCConcurrentRefineThread* HGCConcurrentRefineThreadControl::create_refinement_thread(uint worker_id, bool initializing) {
  HGCConcurrentRefineThread* result = nullptr;
  if (initializing || !InjectGCWorkerCreationFailure) {
    result = HGCConcurrentRefineThread::create(_cr, worker_id);
  }
  if (result == nullptr || result->osthread() == nullptr) {
    log_warning(gc)("Failed to create refinement thread %u, no more %s",
                    worker_id,
                    result == nullptr ? "memory" : "OS threads");
    if (result != nullptr) {
      delete result;
      result = nullptr;
    }
  }
  return result;
}

HGCConcurrentRefineThreadControl::HGCConcurrentRefineThreadControl(uint max_num_threads) :
  _cr(nullptr),
  _threads(max_num_threads)
{}

HGCConcurrentRefineThreadControl::~HGCConcurrentRefineThreadControl() {
  while (_threads.is_nonempty()) {
    delete _threads.pop();
  }
}

bool HGCConcurrentRefineThreadControl::ensure_threads_created(uint worker_id, bool initializing) {
  assert(worker_id < max_num_threads(), "precondition");

  while ((uint)_threads.length() <= worker_id) {
    HGCConcurrentRefineThread* rt = create_refinement_thread(_threads.length(), initializing);
    if (rt == nullptr) {
      return false;
    }
    _threads.push(rt);
  }

  return true;
}

jint HGCConcurrentRefineThreadControl::initialize(HGCConcurrentRefine* cr) {
  assert(cr != nullptr, "HGCConcurrentRefine must not be null");
  _cr = cr;

  if (max_num_threads() > 0) {
    _threads.push(create_refinement_thread(0, true));
    if (_threads.at(0) == nullptr) {
      vm_shutdown_during_initialization("Could not allocate primary refinement thread");
      return JNI_ENOMEM;
    }

    if (!UseDynamicNumberOfGCThreads) {
      if (!ensure_threads_created(max_num_threads() - 1, true)) {
        vm_shutdown_during_initialization("Could not allocate refinement threads");
        return JNI_ENOMEM;
      }
    }
  }

  return JNI_OK;
}

#ifdef ASSERT
void HGCConcurrentRefineThreadControl::assert_current_thread_is_primary_refinement_thread() const {
  assert(Thread::current() == _threads.at(0), "Not primary thread");
}
#endif

bool HGCConcurrentRefineThreadControl::activate(uint worker_id) {
  if (ensure_threads_created(worker_id, false)) {
    _threads.at(worker_id)->activate();
    return true;
  }

  return false;
}

void HGCConcurrentRefineThreadControl::worker_threads_do(ThreadClosure* tc) {
  for (HGCConcurrentRefineThread* t : _threads) {
    tc->do_thread(t);
  }
}

void HGCConcurrentRefineThreadControl::stop() {
  for (HGCConcurrentRefineThread* t : _threads) {
    t->stop();
  }
}

uint64_t HGCConcurrentRefine::adjust_threads_period_ms() const {
  return 50;
}

static size_t minimum_pending_cards_target() {
  return ParallelGCThreads * 1 * K;
}

HGCConcurrentRefine::HGCConcurrentRefine(HGCPolicy* policy) :
  _policy(policy),
  _threads_wanted(0),
  _pending_cards_target(PendingCardsTargetUninitialized),
  _last_adjust(),
  _needs_adjust(false),
  _threads_needed(policy, adjust_threads_period_ms()),
  _thread_control(HGCUseConcurrentRefinement ? HGCConcRefinementThreads : 0),
  _dcqs(HGCBarrierSet::dirty_card_queue_set()),
  _pause_lock(new Monitor(Mutex::nosafepoint, "HGC refine pause")),
  _paused(false),
  _active_refiners(0)
{}

jint HGCConcurrentRefine::initialize() {
  return _thread_control.initialize(this);
}

HGCConcurrentRefine* HGCConcurrentRefine::create(HGCPolicy* policy, jint* ecode) {
  HGCConcurrentRefine* cr = new HGCConcurrentRefine(policy);
  *ecode = cr->initialize();
  if (*ecode != 0) {
    delete cr;
    cr = nullptr;
  }
  return cr;
}

void HGCConcurrentRefine::stop() {
  _thread_control.stop();
}

void HGCConcurrentRefine::activate_primary() {
  if (!HGCUseConcurrentRefinement) {
    return;
  }

  if (!is_pending_cards_target_initialized()) {
    _pending_cards_target = 1024;
  }
  if (Atomic::load(&_threads_wanted) == 0) {
    Atomic::store(&_threads_wanted, static_cast<uint>(1));
  }
  _thread_control.activate(0);
}

HGCConcurrentRefine::~HGCConcurrentRefine() {
  delete _pause_lock;
}

void HGCConcurrentRefine::threads_do(ThreadClosure *tc) {
  _thread_control.worker_threads_do(tc);
}

void HGCConcurrentRefine::update_pending_cards_target(double logged_cards_time_ms,
                                                      size_t processed_logged_cards,
                                                      size_t predicted_thread_buffer_cards,
                                                      double goal_ms) {
  size_t minimum = minimum_pending_cards_target();
  if ((processed_logged_cards < minimum) || (logged_cards_time_ms == 0.0)) {
    log_debug(gc, ergo, refine)("Unchanged pending cards target: %zu",
                                _pending_cards_target);
    return;
  }

  double rate = processed_logged_cards / logged_cards_time_ms;
  size_t budget = static_cast<size_t>(goal_ms * rate);
  size_t new_target = budget - MIN2(budget, predicted_thread_buffer_cards);
  if (is_pending_cards_target_initialized()) {
    new_target = (new_target + _pending_cards_target) / 2;
  }
  new_target = MAX2(new_target, minimum_pending_cards_target());
  _pending_cards_target = new_target;
  log_debug(gc, ergo, refine)("New pending cards target: %zu", new_target);
}

void HGCConcurrentRefine::adjust_after_gc(double logged_cards_time_ms,
                                          size_t processed_logged_cards,
                                          size_t predicted_thread_buffer_cards,
                                          double goal_ms) {
  update_pending_cards_target(logged_cards_time_ms,
                              processed_logged_cards,
                              predicted_thread_buffer_cards,
                              goal_ms);
  if (_thread_control.max_num_threads() == 0) {
    _dcqs.set_mutator_refinement_threshold(_pending_cards_target);
  } else {
    _dcqs.set_mutator_refinement_threshold(SIZE_MAX);
    _needs_adjust = true;
    if (is_pending_cards_target_initialized()) {
      _thread_control.activate(0);
    }
  }
}

static uint64_t compute_adjust_wait_time_ms(double available_ms) {
  return static_cast<uint64_t>(sqrt(available_ms) * 4.0);
}

uint64_t HGCConcurrentRefine::adjust_threads_wait_ms() const {
  assert_current_thread_is_primary_refinement_thread();
  if (is_pending_cards_target_initialized()) {
    double available_ms = _threads_needed.predicted_time_until_next_gc_ms();
    uint64_t wait_time_ms = compute_adjust_wait_time_ms(available_ms);
    return MAX2(wait_time_ms, adjust_threads_period_ms());
  } else {
    return 0;
  }
}

class HGCConcurrentRefine::RemSetSamplingClosure {
  size_t _sampled_cards;

public:
  RemSetSamplingClosure() :
    _sampled_cards(0) {}

  void do_region(HGCRegion* r) {
    HGCHeapRegionRemSet* rem_set = r->rem_set();
    if ((rem_set != nullptr) && rem_set->has_card_set()) {
      _sampled_cards += rem_set->occupied();
    }
  }

  size_t sampled_cards() const { return _sampled_cards; }
};

void HGCConcurrentRefine::adjust_young_list_target_length() {
  if (_policy->use_adaptive_young_list_length()) {
    HGCCollectedHeap* hgc = HGCCollectedHeap::heap();
    RemSetSamplingClosure cl;
    for (HGCRegion* r : hgc->snapshot_of_cset()) {
      cl.do_region(r);
    }
    _policy->revise_young_list_target_length(cl.sampled_cards(), 0);
  }
}

bool HGCConcurrentRefine::adjust_threads_periodically() {
  assert_current_thread_is_primary_refinement_thread();

  if (!_needs_adjust) {
    Tickspan since_adjust = Ticks::now() - _last_adjust;
    if (since_adjust.milliseconds() >= adjust_threads_period_ms()) {
      _needs_adjust = true;
    }
  }

  if (_needs_adjust) {
    if (Heap_lock->try_lock()) {
      size_t used_bytes = _policy->estimate_used_young_bytes_locked();
      Heap_lock->unlock();
      adjust_young_list_target_length();
      size_t young_bytes = _policy->young_list_target_length() * HGCRegionSize;
      size_t available_bytes = young_bytes - MIN2(young_bytes, used_bytes);
      adjust_threads_wanted(available_bytes);
      _needs_adjust = false;
      _last_adjust = Ticks::now();
      return true;
    }
  }

  return false;
}

bool HGCConcurrentRefine::is_in_last_adjustment_period() const {
  return _threads_needed.predicted_time_until_next_gc_ms() <= adjust_threads_period_ms();
}

void HGCConcurrentRefine::adjust_threads_wanted(size_t available_bytes) {
  assert_current_thread_is_primary_refinement_thread();
  size_t num_cards = _dcqs.num_cards();
  size_t mutator_threshold = SIZE_MAX;
  uint old_wanted = Atomic::load(&_threads_wanted);

  _threads_needed.update(old_wanted,
                         available_bytes,
                         num_cards,
                         _pending_cards_target);
  uint new_wanted = _threads_needed.threads_needed();
  if (new_wanted > _thread_control.max_num_threads()) {
    mutator_threshold = _pending_cards_target;
    new_wanted = _thread_control.max_num_threads();
  } else if (is_in_last_adjustment_period()) {
    mutator_threshold = _pending_cards_target;
  }
  Atomic::store(&_threads_wanted, new_wanted);
  _dcqs.set_mutator_refinement_threshold(mutator_threshold);
  log_debug(gc, refine)("Concurrent refinement: wanted %u, cards: %zu, "
                        "predicted: %zu, time: %1.2fms",
                        new_wanted,
                        num_cards,
                        _threads_needed.predicted_cards_at_next_gc(),
                        _threads_needed.predicted_time_until_next_gc_ms());
  for (uint i = MAX2(old_wanted, 1u); i < new_wanted; ++i) {
    if (!_thread_control.activate(i)) {
      Atomic::store(&_threads_wanted, i);
      _dcqs.set_mutator_refinement_threshold(_pending_cards_target);
      break;
    }
  }
}

void HGCConcurrentRefine::reduce_threads_wanted() {
  assert_current_thread_is_primary_refinement_thread();
  if (!_needs_adjust) {
    uint wanted = Atomic::load(&_threads_wanted);
    if (wanted > 0) {
      Atomic::store(&_threads_wanted, --wanted);
    }
    if (is_in_last_adjustment_period()) {
      _dcqs.set_mutator_refinement_threshold(_pending_cards_target);
    }
  }
}

bool HGCConcurrentRefine::is_thread_wanted(uint worker_id) const {
  return worker_id < Atomic::load(&_threads_wanted);
}

bool HGCConcurrentRefine::is_thread_adjustment_needed() const {
  assert_current_thread_is_primary_refinement_thread();
  return _needs_adjust;
}

void HGCConcurrentRefine::record_thread_adjustment_needed() {
  assert_current_thread_is_primary_refinement_thread();
  _needs_adjust = true;
}

HGCConcurrentRefineStats HGCConcurrentRefine::get_and_reset_refinement_stats() {
  struct CollectStats : public ThreadClosure {
    HGCConcurrentRefineStats _total_stats;
    virtual void do_thread(Thread* t) {
      HGCConcurrentRefineThread* crt = static_cast<HGCConcurrentRefineThread*>(t);
      HGCConcurrentRefineStats& stats = *crt->refinement_stats();
      _total_stats += stats;
      stats.reset();
    }
  } collector;
  threads_do(&collector);
  return collector._total_stats;
}

uint HGCConcurrentRefine::worker_id_offset() {
  return HGCDirtyCardQueueSet::num_par_ids();
}

bool HGCConcurrentRefine::try_refinement_step(uint worker_id,
                                              size_t stop_at,
                                              HGCConcurrentRefineStats* stats) {
  {
    MonitorLocker ml(_pause_lock, Mutex::_no_safepoint_check_flag);
    while (_paused) {
      ml.wait();
    }
    _active_refiners++;
  }

  uint adjusted_id = worker_id + worker_id_offset();
  bool result = _dcqs.refine_completed_buffer_concurrently(adjusted_id, stop_at, stats);

  {
    MonitorLocker ml(_pause_lock, Mutex::_no_safepoint_check_flag);
    assert(_active_refiners > 0, "unbalanced refine pause");
    _active_refiners--;
    if (_active_refiners == 0) {
      ml.notify_all();
    }
  }
  return result;
}

void HGCConcurrentRefine::pause_refinement() {
  MonitorLocker ml(_pause_lock, Mutex::_no_safepoint_check_flag);
  _paused = true;
  while (_active_refiners > 0) {
    ml.wait();
  }
}

void HGCConcurrentRefine::resume_refinement() {
  MonitorLocker ml(_pause_lock, Mutex::_no_safepoint_check_flag);
  _paused = false;
  ml.notify_all();
}
