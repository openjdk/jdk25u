#ifndef SHARE_GC_H_HCARDSETMEMORY_HPP
#define SHARE_GC_H_HCARDSETMEMORY_HPP

#include "gc/h/hCardSet.hpp"
#include "gc/h/hMonotonicArena.hpp"
#include "gc/h/hMonotonicArenaFreePool.hpp"
#include "gc/shared/freeListAllocator.hpp"
#include "memory/allocation.hpp"

class HGCCardSetConfiguration;
class outputStream;

class HGCCardSetAllocOptions : public HGCMonotonicArena::AllocOptions {
  static const uint MinimumNumSlots = 8;
  static const uint MaximumNumSlots = UINT_MAX / 2;

  uint exponential_expand(uint prev_num_slots) const {
    return clamp(prev_num_slots * 2, _initial_num_slots, _max_num_slots);
  }

public:
  static const uint SlotAlignment = 8;

  HGCCardSetAllocOptions(uint slot_size, uint initial_num_slots = MinimumNumSlots, uint max_num_slots = MaximumNumSlots) :
    HGCMonotonicArena::AllocOptions(mtGC, slot_size, initial_num_slots, max_num_slots, SlotAlignment) {
  }

  virtual uint next_num_slots(uint prev_num_slots) const override {
    return exponential_expand(prev_num_slots);
  }
};

class HGCCardSetAllocator {
  using Segment = HGCMonotonicArena::Segment;
  using SegmentFreeList = HGCMonotonicArena::SegmentFreeList;
  HGCMonotonicArena _arena;
  FreeListAllocator _free_slots_list;

public:
  HGCCardSetAllocator(const char* name,
                      const HGCCardSetAllocOptions* alloc_options,
                      SegmentFreeList* segment_free_list);
  ~HGCCardSetAllocator();

  void* allocate();
  void free(void* slot);

  void drop_all();

  size_t mem_size() const;

  size_t unused_mem_size() const;

  uint num_segments() const;
};

using HGCCardSetFreePool = HGCMonotonicArenaFreePool;

class HGCCardSetMemoryManager : public CHeapObj<mtGC> {
  HGCCardSetConfiguration* _config;

  HGCCardSetAllocator* _allocators;

  uint num_mem_object_types() const;
public:
  HGCCardSetMemoryManager(HGCCardSetConfiguration* config,
                          HGCCardSetFreePool* free_list_pool);

  virtual ~HGCCardSetMemoryManager();

  inline uint8_t* allocate(uint type);
  void free(uint type, void* value);

  inline uint8_t* allocate_node();
  inline void free_node(void* value);

  void flush();

  size_t mem_size() const;
  size_t unused_mem_size() const;

  HGCMonotonicArenaMemoryStats memory_stats() const;
};

#endif
