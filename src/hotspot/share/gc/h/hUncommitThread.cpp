#include "gc/h/hUncommitThread.hpp"

#include "gc/h/hCollectedHeap.hpp"
#include "runtime/mutexLocker.hpp"

HGCUncommitThread::HGCUncommitThread(HGCCollectedHeap* heap) :
    ConcurrentGCThread(),
    _heap(heap),
    _work_requested(false) {
  set_name("HGC Uncommit");
  create_and_start();
}

bool HGCUncommitThread::wait_for_work() {
  MonitorLocker ml(_heap->uncommit_lock(), Mutex::_no_safepoint_check_flag);
  while (!_work_requested && !should_terminate()) {
    ml.wait();
  }
  if (should_terminate()) {
    return false;
  }
  _work_requested = false;
  return true;
}

void HGCUncommitThread::run_service() {
  while (wait_for_work()) {
    _heap->uncommit_pending_regions();
  }
}

void HGCUncommitThread::stop_service() {
  MonitorLocker ml(_heap->uncommit_lock(), Mutex::_no_safepoint_check_flag);
  ml.notify_all();
}

void HGCUncommitThread::request_work() {
  MonitorLocker ml(_heap->uncommit_lock(), Mutex::_no_safepoint_check_flag);
  _work_requested = true;
  ml.notify();
}
