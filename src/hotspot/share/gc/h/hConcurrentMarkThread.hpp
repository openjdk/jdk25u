#ifndef SHARE_GC_H_HCONCURRENTMARKTHREAD_HPP
#define SHARE_GC_H_HCONCURRENTMARKTHREAD_HPP

#include "gc/shared/concurrentGCThread.hpp"

class HGCCollectedHeap;

class HGCConcurrentMarkThread : public ConcurrentGCThread {
  HGCCollectedHeap* _heap;
  bool _cycle_requested;

  bool wait_for_next_cycle();
  void concurrent_mark();
  void converge_local_queues();
  void request_remark();
  void concurrent_rebuild();
  void request_release();

protected:
  virtual void run_service() override;
  virtual void stop_service() override;

public:
  HGCConcurrentMarkThread(HGCCollectedHeap* heap);

  void request_cycle();
};

#endif
