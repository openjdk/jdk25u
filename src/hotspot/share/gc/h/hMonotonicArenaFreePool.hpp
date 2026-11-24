#ifndef SHARE_GC_H_HMONOTONICARENAFREEPOOL_HPP
#define SHARE_GC_H_HMONOTONICARENAFREEPOOL_HPP

#include "gc/h/hCardSet.hpp"
#include "gc/h/hMonotonicArena.hpp"
#include "utilities/growableArray.hpp"

class outputStream;

class HGCMonotonicArenaMemoryStats {
public:
  size_t _num_mem_sizes[HGCCardSetConfiguration::num_mem_object_types()];
  size_t _num_segments[HGCCardSetConfiguration::num_mem_object_types()];

  HGCMonotonicArenaMemoryStats();

  void add(HGCMonotonicArenaMemoryStats const other) {
    STATIC_ASSERT(ARRAY_SIZE(_num_segments) == ARRAY_SIZE(_num_mem_sizes));
    for (uint i = 0; i < ARRAY_SIZE(_num_mem_sizes); i++) {
      _num_mem_sizes[i] += other._num_mem_sizes[i];
      _num_segments[i] += other._num_segments[i];
    }
  }

  void clear();

  uint num_pools() const { return HGCCardSetConfiguration::num_mem_object_types(); }
};

class HGCMonotonicArenaFreePool : public CHeapObj<mtGC> {
  using SegmentFreeList = HGCMonotonicArena::SegmentFreeList;

  const uint _num_free_lists;
  SegmentFreeList* _free_lists;

public:
  class HGCReturnMemoryProcessor;
  typedef GrowableArrayCHeap<HGCReturnMemoryProcessor*, mtGC> HGCReturnMemoryProcessorSet;

  void update_unlink_processors(HGCReturnMemoryProcessorSet* unlink_processors);

  explicit HGCMonotonicArenaFreePool(uint num_free_lists);
  ~HGCMonotonicArenaFreePool();

  SegmentFreeList* free_list(uint i) {
    assert(i < _num_free_lists, "must be");
    return &_free_lists[i];
  }

  uint num_free_lists() const { return _num_free_lists; }

  HGCMonotonicArenaMemoryStats memory_sizes() const;
  size_t mem_size() const;

  void print_on(outputStream* out) const;
};

class HGCMonotonicArenaFreePool::HGCReturnMemoryProcessor : public CHeapObj<mtGC> {
  using SegmentFreeList = HGCMonotonicArena::SegmentFreeList;
  using Segment = HGCMonotonicArena::Segment;
  SegmentFreeList* _source;
  size_t _return_to_vm_size;

  Segment* _first;
  size_t _unlinked_bytes;
  size_t _num_unlinked;

public:
  explicit HGCReturnMemoryProcessor(size_t return_to_vm) :
    _source(nullptr), _return_to_vm_size(return_to_vm), _first(nullptr), _unlinked_bytes(0), _num_unlinked(0) {
  }

  void visit_free_list(SegmentFreeList* source);

  bool finished_return_to_vm() const { return _return_to_vm_size == 0; }
  bool finished_return_to_os() const { return _first == nullptr; }

  bool return_to_vm(jlong deadline);
  bool return_to_os(jlong deadline);
};

#endif
