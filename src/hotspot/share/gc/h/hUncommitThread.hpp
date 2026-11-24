#ifndef SHARE_GC_H_HUNCOMMITTHREAD_HPP
#define SHARE_GC_H_HUNCOMMITTHREAD_HPP

#include "gc/shared/concurrentGCThread.hpp"

class HGCCollectedHeap;

class HGCUncommitThread : public ConcurrentGCThread {
  HGCCollectedHeap* _heap;
  volatile bool _work_requested;

  bool wait_for_work();

protected:
  virtual void run_service() override;
  virtual void stop_service() override;

public:
  HGCUncommitThread(HGCCollectedHeap* heap);

  void request_work();
};

#endif
