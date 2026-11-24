#ifndef SHARE_GC_H_HCARDSETMEMORY_INLINE_HPP
#define SHARE_GC_H_HCARDSETMEMORY_INLINE_HPP

#include "gc/h/hCardSetMemory.hpp"

inline void* HGCCardSetAllocator::allocate() {
  void* slot = _free_slots_list.allocate();
  assert(slot != nullptr, "must be");
  return slot;
}

inline uint8_t* HGCCardSetMemoryManager::allocate(uint type) {
  assert(type < num_mem_object_types(), "must be");
  return (uint8_t*)_allocators[type].allocate();
}

inline uint8_t* HGCCardSetMemoryManager::allocate_node() {
  return allocate(0);
}

inline void HGCCardSetMemoryManager::free_node(void* value) {
  free(0, value);
}

#endif
