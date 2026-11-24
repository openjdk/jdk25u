#include "gc/h/hCardSetContainers.hpp"
#include "gc/h/hCardSetMemory.hpp"

HGCCardSetAllocator::HGCCardSetAllocator(const char* name,
                                         const HGCCardSetAllocOptions* alloc_options,
                                         SegmentFreeList* segment_free_list) :
  _arena(alloc_options, segment_free_list),
  _free_slots_list(name, &_arena)
{
  uint slot_size = _arena.slot_size();
  assert(slot_size >= sizeof(HGCCardSetContainer), "Slot instance size %u for allocator %s too small", slot_size, name);
}

HGCCardSetAllocator::~HGCCardSetAllocator() {
  drop_all();
}

void HGCCardSetAllocator::free(void* slot) {
  assert(slot != nullptr, "precondition");
  _free_slots_list.release(slot);
}

void HGCCardSetAllocator::drop_all() {
  _free_slots_list.reset();
  _arena.drop_all();
}

size_t HGCCardSetAllocator::mem_size() const {
  return sizeof(*this) +
         num_segments() * sizeof(Segment) +
         _arena.num_total_slots() * _arena.slot_size();
}

size_t HGCCardSetAllocator::unused_mem_size() const {
  uint num_unused_slots = (_arena.num_total_slots() - _arena.num_allocated_slots()) +
                          (uint)_free_slots_list.free_count();
  return num_unused_slots * _arena.slot_size();
}

uint HGCCardSetAllocator::num_segments() const {
  return _arena.num_segments();
}

HGCCardSetMemoryManager::HGCCardSetMemoryManager(HGCCardSetConfiguration* config,
                                                 HGCCardSetFreePool* free_list_pool) : _config(config) {

  _allocators = NEW_C_HEAP_ARRAY(HGCCardSetAllocator,
                                 _config->num_mem_object_types(),
                                 mtGC);
  for (uint i = 0; i < num_mem_object_types(); i++) {
    new (&_allocators[i]) HGCCardSetAllocator(_config->mem_object_type_name_str(i),
                                              _config->mem_object_alloc_options(i),
                                              free_list_pool->free_list(i));
  }
}

uint HGCCardSetMemoryManager::num_mem_object_types() const {
  return _config->num_mem_object_types();
}

HGCCardSetMemoryManager::~HGCCardSetMemoryManager() {
  for (uint i = 0; i < num_mem_object_types(); i++) {
    _allocators[i].~HGCCardSetAllocator();
  }
  FREE_C_HEAP_ARRAY(HGCCardSetAllocator, _allocators);
}

void HGCCardSetMemoryManager::free(uint type, void* value) {
  assert(type < num_mem_object_types(), "must be");
  _allocators[type].free(value);
}

void HGCCardSetMemoryManager::flush() {
  for (uint i = 0; i < num_mem_object_types(); i++) {
    _allocators[i].drop_all();
  }
}

size_t HGCCardSetMemoryManager::mem_size() const {
  size_t result = 0;
  for (uint i = 0; i < num_mem_object_types(); i++) {
    result += _allocators[i].mem_size();
  }
  return sizeof(*this) + result -
    (sizeof(HGCCardSetAllocator) * num_mem_object_types());
}

size_t HGCCardSetMemoryManager::unused_mem_size() const {
  size_t result = 0;
  for (uint i = 0; i < num_mem_object_types(); i++) {
    result += _allocators[i].unused_mem_size();
  }
  return result;
}

HGCMonotonicArenaMemoryStats HGCCardSetMemoryManager::memory_stats() const {
  HGCMonotonicArenaMemoryStats result;
  for (uint i = 0; i < num_mem_object_types(); i++) {
    result._num_mem_sizes[i] += _allocators[i].mem_size();
    result._num_segments[i] += _allocators[i].num_segments();
  }
  return result;
}
