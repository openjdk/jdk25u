#ifndef SHARE_GC_H_VMSTRUCTS_H_HPP
#define SHARE_GC_H_VMSTRUCTS_H_HPP

#include "gc/h/hCollectedHeap.hpp"

#define VM_STRUCTS_HGC(nonstatic_field,                                       \
                        volatile_nonstatic_field,                             \
                        static_field)                                         \
  nonstatic_field(HGCCollectedHeap, _start, HeapWord*)                        \
  nonstatic_field(HGCCollectedHeap, _count_of_regions, size_t)                \
  volatile_nonstatic_field(HGCCollectedHeap, _current_count_of_regions,       \
                           size_t)                                            \
  nonstatic_field(HGCRegion, _start, HeapWord*)                               \
  nonstatic_field(HGCRegion, _k, size_t)                                      \
  nonstatic_field(HGCRegion, next, HGCRegion*)                                \
  nonstatic_field(HGCRegion, prev, HGCRegion*)                                \
  nonstatic_field(HGCRegion, _state, HGCRegion::State)

#define VM_TYPES_HGC(declare_type,                                            \
                      declare_toplevel_type,                                  \
                      declare_integer_type)                                   \
  declare_type(HGCCollectedHeap, CollectedHeap)                               \
  declare_toplevel_type(HGCRegion)                                            \
  declare_toplevel_type(HGCRegion::State)                                     \
  declare_toplevel_type(HGCArena)                                             \
  declare_toplevel_type(HGCBarrierSet)

#define VM_INT_CONSTANTS_HGC(declare_constant,                                \
                              declare_constant_with_value)

#endif
