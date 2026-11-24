#ifndef SHARE_GC_H_HBARRIERSET_HPP
#define SHARE_GC_H_HBARRIERSET_HPP

#include "gc/h/hDirtyCardQueue.hpp"
#include "gc/h/hSATBMarkQueueSet.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "memory/allocation.hpp"
#include "runtime/mutex.hpp"

class HGCCardTable;
class nmethod;

class HGCReentrantLock : public CHeapObj<mtGC> {
  PlatformMonitor _monitor;
  Thread* volatile _owner;
  uint64_t _count;

public:
  HGCReentrantLock();
  ~HGCReentrantLock();

  void lock();
  void unlock();
  bool owned_by_self() const;
};

class HGCReentrantLocker : public StackObj {
  HGCReentrantLock* const _lock;

public:
  explicit HGCReentrantLocker(HGCReentrantLock* lock) : _lock(lock) {
    if (_lock != nullptr) {
      _lock->lock();
    }
  }
  ~HGCReentrantLocker() {
    if (_lock != nullptr) {
      _lock->unlock();
    }
  }
};

class HGCnmethodData : public CHeapObj<mtGC> {
  HGCReentrantLock _lock;
  HGCReentrantLock _ic_lock;

public:
  static HGCnmethodData* for_nmethod(nmethod* nm);

  HGCReentrantLock* lock() { return &_lock; }
  HGCReentrantLock* ic_lock() { return &_ic_lock; }
};

class HGCBarrierSetNMethod : public BarrierSetNMethod {
public:
  bool nmethod_entry_barrier(nmethod* nm) override;
};

class HGCBarrierSet: public CardTableBarrierSet {
  friend class VMStructs;

private:
  BufferNode::Allocator _satb_mark_queue_buffer_allocator;
  HGCSATBMarkQueueSet _satb_mark_queue_set;

  BufferNode::Allocator _dirty_card_queue_buffer_allocator;
  HGCDirtyCardQueueSet _dirty_card_queue_set;

  static HGCBarrierSet* hgc_barrier_set() {
    return barrier_set_cast<HGCBarrierSet>(BarrierSet::barrier_set());
  }

public:
  HGCBarrierSet(HGCCardTable* card_table);

  static HGCSATBMarkQueueSet& satb_mark_queue_set() {
    return hgc_barrier_set()->_satb_mark_queue_set;
  }

  static HGCDirtyCardQueueSet& dirty_card_queue_set() {
    return hgc_barrier_set()->_dirty_card_queue_set;
  }

  template <class T> static void enqueue(T* dst);
  static void enqueue_preloaded(oop pre_val);
  static void enqueue_preloaded_if_weak(DecoratorSet decorators, oop value);

  template <DecoratorSet decorators, typename T>
  void write_ref_field_pre(T* field);

  template <DecoratorSet decorators, typename T>
  void write_ref_field_post(T* field);

  static void dirty_card(volatile CardValue* card);

  static void post_barrier(const void* addr);

  static void requeue_dirty_card(volatile CardValue* card);

  static void mark_card_dirty_and_enqueue(volatile CardValue* card, CardValue to);

  template <class T> void write_ref_array_pre_work(T* dst, size_t count);
  virtual void write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized);
  virtual void write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized);

  void write_ref_array(HeapWord* start, size_t count);

  using CardTableBarrierSet::write_region;
  virtual void write_region(MemRegion mr) override;

  virtual void on_thread_create(Thread* thread);
  virtual void on_thread_destroy(Thread* thread);
  virtual void on_thread_attach(Thread* thread);
  virtual void on_thread_detach(Thread* thread);

  template <DecoratorSet decorators, typename BarrierSetT = HGCBarrierSet>
  class AccessBarrier: public ModRefBarrierSet::AccessBarrier<decorators, BarrierSetT> {
    typedef ModRefBarrierSet::AccessBarrier<decorators, BarrierSetT> ModRef;
    typedef BarrierSet::AccessBarrier<decorators, BarrierSetT> Raw;

  public:

    template <typename T>
    static oop oop_load_not_in_heap(T* addr);

    template <typename T>
    static void oop_store_not_in_heap(T* addr, oop new_value);

    static oop oop_load_in_heap_at(oop base, ptrdiff_t offset);

    template <typename T>
    static oop oop_load_in_heap(T* addr);

    template <typename T>
    static oop oop_atomic_cmpxchg_not_in_heap(T* addr, oop compare_value, oop new_value);
    template <typename T>
    static oop oop_atomic_xchg_not_in_heap(T* addr, oop new_value);
  };
};

template<>
struct BarrierSet::GetName<HGCBarrierSet> {
  static const BarrierSet::Name value = BarrierSet::HGCBarrierSet;
};

template<>
struct BarrierSet::GetType<BarrierSet::HGCBarrierSet> {
  typedef ::HGCBarrierSet type;
};

#endif
