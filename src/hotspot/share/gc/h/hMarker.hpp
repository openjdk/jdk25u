#ifndef SHARE_GC_H_HMARKER_HPP
#define SHARE_GC_H_HMARKER_HPP

#include "gc/h/hArena.hpp"
#include "gc/shared/satbMarkQueue.hpp"
#include "gc/shared/taskqueue.hpp"
#include "gc/shared/taskTerminator.hpp"
#include "gc/shared/workerThread.hpp"
#include "memory/iterator.hpp"
#include "runtime/mutex.hpp"

typedef OverflowTaskQueue<HGCArena*, mtGC> HGCMarkQueue;

class HGCMarkQueueSet : public GenericTaskQueueSet<HGCMarkQueue, mtGC> {
private:

  HGCMarkQueue _shared_queue;
  Mutex _push_lock;

public:
  explicit HGCMarkQueueSet(uint n);
  ~HGCMarkQueueSet();

  void push_shared(HGCArena* arena);
  bool pop_shared(HGCArena*& arena);

  void clear();

  bool has_pending_work();
};

class HGCMarkClosure : public BasicOopIterateClosure {
private:
  HGCMarkQueue* _queue;
  bool _record_refs;
  uint _worker_id;
  bool _lenient;
  bool _shared_push;
  OopIterateClosure::ReferenceIterationMode _reference_mode;
  const void* _field;

public:
  explicit HGCMarkClosure(HGCMarkQueue* queue,
                          bool record_refs = false,
                          uint worker_id = 0,
                          bool lenient = false,
                          OopIterateClosure::ReferenceIterationMode reference_mode = OopIterateClosure::DO_DISCOVERY,
                          bool shared_push = false) :
      _queue(queue), _record_refs(record_refs), _worker_id(worker_id), _lenient(lenient),
      _shared_push(shared_push),
      _reference_mode(reference_mode), _field(nullptr) {}

  OopIterateClosure::ReferenceIterationMode reference_iteration_mode() override {
    return _reference_mode;
  }

  void mark_object(oop obj);

  void set_ref_discoverer(ReferenceDiscoverer* rd) {
    set_ref_discoverer_internal(rd);
  }

  virtual void do_oop(oop* p) override;
  virtual void do_oop(narrowOop* p) override;
};

class HGCBatchedSATBBufferClosure : public SATBBufferClosure {
  HGCMarkQueue* _queue;
  ReferenceDiscoverer* _ref_discoverer;
  OopIterateClosure::ReferenceIterationMode _reference_mode;

public:
  explicit HGCBatchedSATBBufferClosure(HGCMarkQueue* queue,
                                       ReferenceDiscoverer* ref_discoverer = nullptr,
                                       OopIterateClosure::ReferenceIterationMode reference_mode = OopIterateClosure::DO_DISCOVERY) :
      _queue(queue), _ref_discoverer(ref_discoverer), _reference_mode(reference_mode) {}

  virtual void do_buffer(void** buffer, size_t size) override;
};

class HGCMarkTask : public WorkerTask {
private:
  HGCMarkQueueSet* _queues;
  TaskTerminator _terminator;
  bool _concurrent;
  ReferenceDiscoverer* _ref_discoverer;
  OopIterateClosure::ReferenceIterationMode _reference_mode;

public:
  HGCMarkTask(HGCMarkQueueSet* queues, uint n_workers, bool concurrent,
              ReferenceDiscoverer* ref_discoverer = nullptr,
              OopIterateClosure::ReferenceIterationMode reference_mode = OopIterateClosure::DO_DISCOVERY) :
      WorkerTask("HGC Mark"),
      _queues(queues),
      _terminator(n_workers, queues),
      _concurrent(concurrent),
      _ref_discoverer(ref_discoverer),
      _reference_mode(reference_mode) {}

  virtual void work(uint worker_id) override;
};

#endif
