#ifndef SHARE_GC_H_HCONCURRENTREFINE_HPP
#define SHARE_GC_H_HCONCURRENTREFINE_HPP

#include "gc/h/hConcurrentRefineStats.hpp"
#include "gc/h/hConcurrentRefineThreadsNeeded.hpp"
#include "memory/allocation.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

class HGCConcurrentRefine;
class HGCConcurrentRefineThread;
class HGCDirtyCardQueueSet;
class HGCPolicy;
class ThreadClosure;

class HGCConcurrentRefineThreadControl {
  HGCConcurrentRefine* _cr;
  GrowableArrayCHeap<HGCConcurrentRefineThread*, mtGC> _threads;

  HGCConcurrentRefineThread* create_refinement_thread(uint worker_id, bool initializing);

  bool ensure_threads_created(uint worker_id, bool initializing);

  NONCOPYABLE(HGCConcurrentRefineThreadControl);

public:
  HGCConcurrentRefineThreadControl(uint max_num_threads);
  ~HGCConcurrentRefineThreadControl();

  jint initialize(HGCConcurrentRefine* cr);

  void assert_current_thread_is_primary_refinement_thread() const NOT_DEBUG_RETURN;

  uint max_num_threads() const { return _threads.capacity(); }

  bool activate(uint worker_id);

  void worker_threads_do(ThreadClosure* tc);
  void stop();
};

class HGCConcurrentRefine : public CHeapObj<mtGC> {
  HGCPolicy* _policy;
  volatile uint _threads_wanted;
  size_t _pending_cards_target;
  Ticks _last_adjust;
  Ticks _last_deactivate;
  bool _needs_adjust;
  HGCConcurrentRefineThreadsNeeded _threads_needed;
  HGCConcurrentRefineThreadControl _thread_control;
  HGCDirtyCardQueueSet& _dcqs;
  Monitor* _pause_lock;
  bool _paused;
  uint _active_refiners;

  HGCConcurrentRefine(HGCPolicy* policy);

  static uint worker_id_offset();

  jint initialize();

  void assert_current_thread_is_primary_refinement_thread() const {
    _thread_control.assert_current_thread_is_primary_refinement_thread();
  }

  static const size_t PendingCardsTargetUninitialized = SIZE_MAX;
  bool is_pending_cards_target_initialized() const {
    return _pending_cards_target != PendingCardsTargetUninitialized;
  }

  void update_pending_cards_target(double logged_cards_scan_time_ms,
                                   size_t processed_logged_cards,
                                   size_t predicted_thread_buffer_cards,
                                   double goal_ms);

  uint64_t adjust_threads_period_ms() const;
  bool is_in_last_adjustment_period() const;

  class RemSetSamplingClosure;
  void adjust_young_list_target_length();

  void adjust_threads_wanted(size_t available_bytes);

  NONCOPYABLE(HGCConcurrentRefine);

public:
  ~HGCConcurrentRefine();

  static HGCConcurrentRefine* create(HGCPolicy* policy, jint* ecode);

  void stop();

  void activate_primary();

  void adjust_after_gc(double logged_cards_scan_time_ms,
                       size_t processed_logged_cards,
                       size_t predicted_thread_buffer_cards,
                       double goal_ms);

  size_t pending_cards_target() const { return _pending_cards_target; }

  bool adjust_threads_periodically();

  uint64_t adjust_threads_wait_ms() const;

  void record_thread_adjustment_needed();

  bool is_thread_adjustment_needed() const;

  void reduce_threads_wanted();

  bool is_thread_wanted(uint worker_id) const;

  HGCConcurrentRefineStats get_and_reset_refinement_stats();

  bool try_refinement_step(uint worker_id,
                           size_t stop_at,
                           HGCConcurrentRefineStats* stats);

  void pause_refinement();
  void resume_refinement();

  void threads_do(ThreadClosure *tc);
};

#endif
