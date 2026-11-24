#ifndef SHARE_GC_H_HBARRIERSET_INLINE_HPP
#define SHARE_GC_H_HBARRIERSET_INLINE_HPP

#include "gc/h/hBarrierSet.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"

#include "gc/h/hCardTable.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "gc/shared/accessBarrierSupport.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.hpp"
#include "runtime/thread.hpp"

inline void HGCBarrierSet::enqueue_preloaded(oop pre_val) {
  if (!HGCUsePreBarrier) return;
  HGCSATBMarkQueueSet& queue_set = HGCBarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  SATBMarkQueue& queue = HGCThreadLocalData::satb_mark_queue(Thread::current());
  queue_set.enqueue_known_active(queue, pre_val);
}

template <class T>
inline void HGCBarrierSet::enqueue(T* dst) {
  if (!HGCUsePreBarrier) return;
  HGCSATBMarkQueueSet& queue_set = HGCBarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  T heap_oop = RawAccess<MO_RELAXED>::oop_load(dst);
  if (!CompressedOops::is_null(heap_oop)) {
    SATBMarkQueue& queue = HGCThreadLocalData::satb_mark_queue(Thread::current());
    queue_set.enqueue_known_active(queue, CompressedOops::decode_not_null(heap_oop));
  }
}

template <DecoratorSet decorators, typename T>
inline void HGCBarrierSet::write_ref_field_pre(T* field) {
  if (HasDecorator<decorators, IS_DEST_UNINITIALIZED>::value ||
      HasDecorator<decorators, AS_NO_KEEPALIVE>::value) {
    return;
  }

  enqueue(field);
}

template <DecoratorSet decorators, typename T>
inline void HGCBarrierSet::write_ref_field_post(T* field) {

  post_barrier(field);
}

inline void HGCBarrierSet::enqueue_preloaded_if_weak(DecoratorSet decorators, oop value) {
  assert((decorators & ON_UNKNOWN_OOP_REF) == 0, "Reference strength must be known");
  const bool on_strong_oop_ref = (decorators & ON_STRONG_OOP_REF) != 0;
  const bool peek              = (decorators & AS_NO_KEEPALIVE) != 0;
  const bool needs_enqueue     = (!peek && !on_strong_oop_ref);

  if (needs_enqueue && value != nullptr) {
    enqueue_preloaded(value);
  }
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop HGCBarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_load_not_in_heap(T* addr) {
  oop value = ModRef::oop_load_not_in_heap(addr);
  enqueue_preloaded_if_weak(decorators, value);
  return value;
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop HGCBarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_load_in_heap(T* addr) {
  oop value = ModRef::oop_load_in_heap(addr);
  enqueue_preloaded_if_weak(decorators, value);
  return value;
}

template <DecoratorSet decorators, typename BarrierSetT>
inline oop HGCBarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_load_in_heap_at(oop base, ptrdiff_t offset) {
  oop value = ModRef::oop_load_in_heap_at(base, offset);
  enqueue_preloaded_if_weak(AccessBarrierSupport::resolve_possibly_unknown_oop_ref_strength<decorators>(base, offset), value);
  return value;
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline void HGCBarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_store_not_in_heap(T* addr, oop new_value) {

  HGCBarrierSet* bs = barrier_set_cast<HGCBarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_field_pre<decorators>(addr);
  Raw::oop_store(addr, new_value);
  bs->post_barrier(addr);
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop HGCBarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_atomic_cmpxchg_not_in_heap(T* addr, oop compare_value, oop new_value) {

  HGCBarrierSet* bs = barrier_set_cast<HGCBarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_field_pre<decorators>(addr);
  oop result = Raw::oop_atomic_cmpxchg(addr, compare_value, new_value);
  bs->post_barrier(addr);
  return result;
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop HGCBarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_atomic_xchg_not_in_heap(T* addr, oop new_value) {

  HGCBarrierSet* bs = barrier_set_cast<HGCBarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_field_pre<decorators>(addr);
  oop result = Raw::oop_atomic_xchg(addr, new_value);
  bs->post_barrier(addr);
  return result;
}

#endif
