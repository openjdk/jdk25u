#ifndef SHARE_GC_H_HCONCURRENTREFINETHREAD_HPP
#define SHARE_GC_H_HCONCURRENTREFINETHREAD_HPP

#include "gc/h/hConcurrentRefineStats.hpp"
#include "gc/shared/concurrentGCThread.hpp"
#include "runtime/mutex.hpp"
#include "utilities/globalDefinitions.hpp"

class HGCConcurrentRefine;

class HGCConcurrentRefineThread: public ConcurrentGCThread {
  friend class VMStructs;
  friend class HGCCollectedHeap;

  double _vtime_start;
  double _vtime_accum;

  Monitor _notifier;
  bool _requested_active;

  HGCConcurrentRefineStats _refinement_stats;

  uint _worker_id;

  HGCConcurrentRefine* _cr;

  NONCOPYABLE(HGCConcurrentRefineThread);

protected:
  HGCConcurrentRefineThread(HGCConcurrentRefine* cr, uint worker_id);

  Monitor* notifier() { return &_notifier; }
  bool requested_active() const { return _requested_active; }

  virtual bool wait_for_completed_buffers() = 0;

  virtual bool maybe_deactivate();

  virtual void do_refinement_step() = 0;

  virtual void track_usage() {
    if (os::supports_vtime()) {
      _vtime_accum = (os::elapsedVTime() - _vtime_start);
    } else {
      _vtime_accum = 0.0;
    }
  };

  bool try_refinement_step(size_t stop_at);

  void report_active(const char* reason) const;
  void report_inactive(const char* reason, const HGCConcurrentRefineStats& stats) const;

  HGCConcurrentRefine* cr() const { return _cr; }

  void run_service() override;
  void stop_service() override;

public:
  static HGCConcurrentRefineThread* create(HGCConcurrentRefine* cr, uint worker_id);
  virtual ~HGCConcurrentRefineThread() = default;

  uint worker_id() const { return _worker_id; }

  void activate();

  HGCConcurrentRefineStats* refinement_stats() {
    return &_refinement_stats;
  }

  const HGCConcurrentRefineStats* refinement_stats() const {
    return &_refinement_stats;
  }

  double vtime_accum() { return _vtime_accum; }
};

#endif
