#ifndef SHARE_GC_H_HOOPCLOSURES_HPP
#define SHARE_GC_H_HOOPCLOSURES_HPP

#include "memory/iterator.hpp"
#include "oops/oopsHierarchy.hpp"

class HGCCollectedHeap;

class HGCConcurrentRefineOopClosure : public BasicOopIterateClosure {
  HGCCollectedHeap* _heap;
  uint _worker_id;

public:
  HGCConcurrentRefineOopClosure(HGCCollectedHeap* heap, uint worker_id) :
    _heap(heap),
    _worker_id(worker_id) {
  }

  virtual ReferenceIterationMode reference_iteration_mode() { return DO_FIELDS; }

  template <class T> void do_oop_work(T* p);
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p)       { do_oop_work(p); }
};

class HGCRebuildRemSetClosure : public BasicOopIterateClosure {
  HGCCollectedHeap* _heap;
  uint _worker_id;

public:
  HGCRebuildRemSetClosure(HGCCollectedHeap* heap, uint worker_id) :
    _heap(heap),
    _worker_id(worker_id) {
  }

  template <class T> void do_oop_work(T* p);
  virtual void do_oop(oop* p)       { do_oop_work(p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }

  virtual ReferenceIterationMode reference_iteration_mode() { return DO_FIELDS; }
};

#endif
