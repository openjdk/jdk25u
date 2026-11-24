#ifndef SHARE_GC_H_HMONOTONICARENA_INLINE_HPP
#define SHARE_GC_H_HMONOTONICARENA_INLINE_HPP

#include "gc/h/hMonotonicArena.hpp"

#include "runtime/atomic.hpp"
#include "utilities/globalCounter.inline.hpp"

inline void* HGCMonotonicArena::Segment::allocate_slot() {
  if (_next_allocate >= _num_slots) {
    return nullptr;
  }
  uint result = Atomic::fetch_then_add(&_next_allocate, 1u, memory_order_relaxed);
  if (result >= _num_slots) {
    return nullptr;
  }
  void* r = _bottom + (size_t)result * _slot_size;
  return r;
}

inline HGCMonotonicArena::Segment* HGCMonotonicArena::SegmentFreeList::get() {
  GlobalCounter::CriticalSection cs(Thread::current());

  Segment* result = _list.pop();
  if (result != nullptr) {
    Atomic::dec(&_num_segments, memory_order_relaxed);
    Atomic::sub(&_mem_size, result->mem_size(), memory_order_relaxed);
  }
  return result;
}

#endif
