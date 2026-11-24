#include "gc/h/hMonotonicArena.inline.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "runtime/vmOperations.hpp"
#include "utilities/globalCounter.inline.hpp"

HGCMonotonicArena::Segment::Segment(uint slot_size, uint num_slots, Segment* next, MemTag mem_tag) :
  _slot_size(slot_size),
  _num_slots(num_slots),
  _next(next),
  _next_allocate(0),
  _mem_tag(mem_tag) {
  _bottom = ((char*) this) + header_size();
}

HGCMonotonicArena::Segment* HGCMonotonicArena::Segment::create_segment(uint slot_size,
                                                                       uint num_slots,
                                                                       Segment* next,
                                                                       MemTag mem_tag) {
  size_t block_size = size_in_bytes(slot_size, num_slots);
  char* alloc_block = NEW_C_HEAP_ARRAY(char, block_size, mem_tag);
  return new (alloc_block) Segment(slot_size, num_slots, next, mem_tag);
}

void HGCMonotonicArena::Segment::delete_segment(Segment* segment) {
  if (!VM_Exit::vm_exited()) {
    GlobalCounter::write_synchronize();
  }
  segment->~Segment();
  FREE_C_HEAP_ARRAY(_mem_tag, segment);
}

void HGCMonotonicArena::SegmentFreeList::bulk_add(Segment& first,
                                                  Segment& last,
                                                  size_t num,
                                                  size_t mem_size) {
  _list.prepend(first, last);
  Atomic::add(&_num_segments, num, memory_order_relaxed);
  Atomic::add(&_mem_size, mem_size, memory_order_relaxed);
}


HGCMonotonicArena::Segment* HGCMonotonicArena::SegmentFreeList::get_all(size_t& num_segments,
                                                                        size_t& mem_size) {
  GlobalCounter::CriticalSection cs(Thread::current());

  Segment* result = _list.pop_all();
  num_segments = Atomic::load(&_num_segments);
  mem_size = Atomic::load(&_mem_size);

  if (result != nullptr) {
    Atomic::sub(&_num_segments, num_segments, memory_order_relaxed);
    Atomic::sub(&_mem_size, mem_size, memory_order_relaxed);
  }
  return result;
}

void HGCMonotonicArena::SegmentFreeList::free_all() {
  size_t num_freed = 0;
  size_t mem_size_freed = 0;
  Segment* cur;

  while ((cur = _list.pop()) != nullptr) {
    mem_size_freed += cur->mem_size();
    num_freed++;
    Segment::delete_segment(cur);
  }

  Atomic::sub(&_num_segments, num_freed, memory_order_relaxed);
  Atomic::sub(&_mem_size, mem_size_freed, memory_order_relaxed);
}

HGCMonotonicArena::Segment* HGCMonotonicArena::new_segment(Segment* const prev) {
  Segment* next = _segment_free_list->get();
  if (next == nullptr) {
    uint prev_num_slots = (prev != nullptr) ? prev->num_slots() : 0;
    uint num_slots = _alloc_options->next_num_slots(prev_num_slots);

    next = Segment::create_segment(slot_size(), num_slots, prev, _alloc_options->mem_tag());
  } else {
    assert(slot_size() == next->slot_size(),
           "Mismatch %d != %d", slot_size(), next->slot_size());
    next->reset(prev);
  }

  Segment* old = Atomic::cmpxchg(&_first, prev, next);
  if (old != prev) {
    Segment::delete_segment(next);
    return old;
  } else {
    if (prev == nullptr) {
      _last = next;
    }
    Atomic::inc(&_num_segments, memory_order_relaxed);
    Atomic::add(&_mem_size, next->mem_size(), memory_order_relaxed);
    Atomic::add(&_num_total_slots, next->num_slots(), memory_order_relaxed);
    return next;
  }
}

HGCMonotonicArena::HGCMonotonicArena(const AllocOptions* alloc_options,
                                     SegmentFreeList* segment_free_list) :
  _alloc_options(alloc_options),
  _first(nullptr),
  _last(nullptr),
  _num_segments(0),
  _mem_size(0),
  _segment_free_list(segment_free_list),
  _num_total_slots(0),
  _num_allocated_slots(0) {
  assert(_segment_free_list != nullptr, "precondition!");
}

HGCMonotonicArena::~HGCMonotonicArena() {
  drop_all();
}

uint HGCMonotonicArena::slot_size() const {
  return _alloc_options->slot_size();
}

void HGCMonotonicArena::drop_all() {
  Segment* cur = Atomic::load_acquire(&_first);

  if (cur != nullptr) {
    assert(_last != nullptr, "If there is at least one segment, there must be a last one.");

    Segment* first = cur;
#ifdef ASSERT
    Segment* last = cur;
    uint num_segments = 0;
    size_t mem_size = 0;
    while (cur != nullptr) {
      mem_size += cur->mem_size();
      num_segments++;

      Segment* next = cur->next();
      last = cur;
      cur = next;
    }
#endif
    assert(num_segments == _num_segments, "Segment count inconsistent %u %u", num_segments, _num_segments);
    assert(mem_size == _mem_size, "Memory size inconsistent");
    assert(last == _last, "Inconsistent last segment");

    _segment_free_list->bulk_add(*first, *_last, _num_segments, _mem_size);
  }

  _first = nullptr;
  _last = nullptr;
  _num_segments = 0;
  _mem_size = 0;
  _num_total_slots = 0;
  _num_allocated_slots = 0;
}

void* HGCMonotonicArena::allocate() {
  assert(slot_size() > 0, "instance size not set.");

  Segment* cur = Atomic::load_acquire(&_first);
  if (cur == nullptr) {
    cur = new_segment(cur);
  }

  while (true) {
    void* slot = cur->allocate_slot();
    if (slot != nullptr) {
      Atomic::inc(&_num_allocated_slots, memory_order_relaxed);
      guarantee(is_aligned(slot, _alloc_options->slot_alignment()),
                "result " PTR_FORMAT " not aligned at %u", p2i(slot), _alloc_options->slot_alignment());
      return slot;
    }
    assert(cur->is_full(), "must be");
    cur = new_segment(cur);
  }
}

uint HGCMonotonicArena::num_segments() const {
  return Atomic::load(&_num_segments);
}

#ifdef ASSERT
class LengthClosure {
  uint _total;
public:
  LengthClosure() : _total(0) {}
  void do_segment(HGCMonotonicArena::Segment* segment, uint limit) {
    _total += limit;
  }
  uint length() const {
    return _total;
  }
};

uint HGCMonotonicArena::calculate_length() const {
  LengthClosure closure;
  iterate_segments(closure);
  return closure.length();
}
#endif

template <typename SegmentClosure>
void HGCMonotonicArena::iterate_segments(SegmentClosure& closure) const {
  Segment* cur = Atomic::load_acquire(&_first);

  assert((cur != nullptr) == (_last != nullptr),
         "If there is at least one segment, there must be a last one");

  while (cur != nullptr) {
    closure.do_segment(cur, cur->length());
    cur = cur->next();
  }
}

void HGCMonotonicArena::SegmentFreeList::print_on(outputStream* out, const char* prefix) {
  out->print_cr("%s: segments %zu size %zu",
                prefix, Atomic::load(&_num_segments), Atomic::load(&_mem_size));
}
