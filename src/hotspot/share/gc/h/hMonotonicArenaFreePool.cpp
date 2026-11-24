#include "gc/h/hMonotonicArenaFreePool.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/globalCounter.inline.hpp"
#include "utilities/ostream.hpp"

HGCMonotonicArenaMemoryStats::HGCMonotonicArenaMemoryStats() {
  clear();
}

void HGCMonotonicArenaMemoryStats::clear() {
  for (uint i = 0; i < num_pools(); i++) {
    _num_mem_sizes[i] = 0;
    _num_segments[i] = 0;
  }
}

void HGCMonotonicArenaFreePool::update_unlink_processors(HGCReturnMemoryProcessorSet* unlink_processor) {

  for (uint i = 0; i < num_free_lists(); i++) {
    unlink_processor->at(i)->visit_free_list(free_list(i));
  }
}

void HGCMonotonicArenaFreePool::HGCReturnMemoryProcessor::visit_free_list(HGCMonotonicArena::SegmentFreeList* source) {
  assert(_source == nullptr, "already visited");
  if (_return_to_vm_size > 0) {
    _source = source;
  } else {
    assert(_source == nullptr, "must be");
  }
  if (source->mem_size() > _return_to_vm_size) {
    _first = source->get_all(_num_unlinked, _unlinked_bytes);
  } else {
    assert(_first == nullptr, "must be");
  }
  if (_first == nullptr) {
    _source = nullptr;
    _return_to_vm_size = 0;
  }
}

bool HGCMonotonicArenaFreePool::HGCReturnMemoryProcessor::return_to_vm(jlong deadline) {
  assert(!finished_return_to_vm(), "already returned everything to the VM");
  assert(_first != nullptr, "must have segment to return");

  size_t keep_size = 0;
  size_t keep_num = 0;

  Segment* cur = _first;
  Segment* last = nullptr;

  while (cur != nullptr && _return_to_vm_size > 0) {
    size_t cur_size = cur->mem_size();
    _return_to_vm_size -= MIN2(_return_to_vm_size, cur_size);

    keep_size += cur_size;
    keep_num++;

    last = cur;
    cur = cur->next();
    if (os::elapsed_counter() > deadline) {
      break;
    }
  }

  assert(_first != nullptr, "must be");
  assert(last != nullptr, "must be");

  last->set_next(nullptr);

  GlobalCounter::write_synchronize();
  _source->bulk_add(*_first, *last, keep_num, keep_size);
  _first = cur;

  log_trace(gc, task)("Monotonic Arena Free Memory: Returned to VM %zu segments size %zu", keep_num, keep_size);

  if (_return_to_vm_size == 0 || _first == nullptr) {
    _source = nullptr;
    _return_to_vm_size = 0;
  }
  return _source != nullptr;
}

bool HGCMonotonicArenaFreePool::HGCReturnMemoryProcessor::return_to_os(jlong deadline) {
  assert(finished_return_to_vm(), "not finished returning to VM");
  assert(!finished_return_to_os(), "already returned everything to the OS");

  size_t num_delete = 0;
  size_t mem_size_deleted = 0;

  while (_first != nullptr) {
    Segment* next = _first->next();
    num_delete++;
    mem_size_deleted += _first->mem_size();
    Segment::delete_segment(_first);
    _first = next;

    if (os::elapsed_counter() > deadline) {
      break;
    }
  }

  log_trace(gc, task)("Monotonic Arena Free Memory: Return to OS %zu segments size %zu", num_delete, mem_size_deleted);

  return _first != nullptr;
}

HGCMonotonicArenaFreePool::HGCMonotonicArenaFreePool(uint num_free_lists) :
  _num_free_lists(num_free_lists) {

  _free_lists = NEW_C_HEAP_ARRAY(SegmentFreeList, _num_free_lists, mtGC);
  for (uint i = 0; i < _num_free_lists; i++) {
    new (&_free_lists[i]) SegmentFreeList();
  }
}

HGCMonotonicArenaFreePool::~HGCMonotonicArenaFreePool() {
  for (uint i = 0; i < _num_free_lists; i++) {
    _free_lists[i].~SegmentFreeList();
  }
  FREE_C_HEAP_ARRAY(mtGC, _free_lists);
}

HGCMonotonicArenaMemoryStats HGCMonotonicArenaFreePool::memory_sizes() const {
  HGCMonotonicArenaMemoryStats free_list_stats;
  assert(free_list_stats.num_pools() == num_free_lists(), "must be");
  for (uint i = 0; i < num_free_lists(); i++) {
    free_list_stats._num_mem_sizes[i] = _free_lists[i].mem_size();
    free_list_stats._num_segments[i] = _free_lists[i].num_segments();
  }
  return free_list_stats;
}

size_t HGCMonotonicArenaFreePool::mem_size() const {
  size_t result = 0;
  for (uint i = 0; i < _num_free_lists; i++) {
    result += _free_lists[i].mem_size();
  }
  return result;
}

void HGCMonotonicArenaFreePool::print_on(outputStream* out) const {
  out->print_cr("  Free Pool: size %zu", mem_size());
  for (uint i = 0; i < _num_free_lists; i++) {
    FormatBuffer<> fmt("    %s", HGCCardSetConfiguration::mem_object_type_name_str(i));
    _free_lists[i].print_on(out, fmt);
  }
}
