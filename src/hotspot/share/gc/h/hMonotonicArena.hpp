#ifndef SHARE_GC_H_HMONOTONICARENA_HPP
#define SHARE_GC_H_HMONOTONICARENA_HPP

#include "gc/shared/freeListAllocator.hpp"
#include "nmt/memTag.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/lockFreeStack.hpp"

class outputStream;

class HGCMonotonicArena : public FreeListConfig {
public:
  class AllocOptions;
  class Segment;
  class SegmentFreeList;
private:
  const AllocOptions* _alloc_options;

  Segment* volatile _first;
  Segment* _last;
  volatile uint _num_segments;
  volatile size_t _mem_size;

  SegmentFreeList* _segment_free_list;

  volatile uint _num_total_slots;
  volatile uint _num_allocated_slots;

  inline Segment* new_segment(Segment* const prev);

  DEBUG_ONLY(uint calculate_length() const;)

public:
  const Segment* first_segment() const { return Atomic::load(&_first); }
  uint num_total_slots() const { return Atomic::load(&_num_total_slots); }
  uint num_allocated_slots() const {
    uint allocated = Atomic::load(&_num_allocated_slots);
    assert(calculate_length() == allocated, "Must be");
    return allocated;
  }

  uint slot_size() const;

  HGCMonotonicArena(const AllocOptions* alloc_options,
                    SegmentFreeList* segment_free_list);
  ~HGCMonotonicArena();

  void drop_all();
  uint num_segments() const;

  template<typename SegmentClosure>
  void iterate_segments(SegmentClosure& closure) const;
protected:
  void* allocate() override;
  void deallocate(void* slot) override { ShouldNotReachHere(); }
};

class HGCMonotonicArena::Segment {
  const uint _slot_size;
  const uint _num_slots;
  Segment* volatile _next;
  uint volatile _next_allocate;
  const MemTag _mem_tag;

  char* _bottom;

  static size_t header_size() { return align_up(sizeof(Segment), DEFAULT_PADDING_SIZE); }

  static size_t payload_size(uint slot_size, uint num_slots) {
    return (size_t)slot_size * num_slots;
  }

  size_t payload_size() const { return payload_size(_slot_size, _num_slots); }

  NONCOPYABLE(Segment);

  Segment(uint slot_size, uint num_slots, Segment* next, MemTag mem_tag);
  ~Segment() = default;
public:
  Segment* volatile* next_addr() { return &_next; }

  void* allocate_slot();

  uint num_slots() const { return _num_slots; }
  Segment* next() const { return _next; }

  void set_next(Segment* next) {
    assert(next != this, " loop condition");
    _next = next;
  }

  void reset(Segment* next) {
    _next_allocate = 0;
    assert(next != this, " loop condition");
    set_next(next);
    memset((void*)_bottom, 0, payload_size());
  }

  uint slot_size() const { return _slot_size; }
  size_t mem_size() const { return header_size() + payload_size(); }

  uint length() const {
    return MIN2(_next_allocate, _num_slots);
  }

  static size_t size_in_bytes(uint slot_size, uint num_slots) {
    return header_size() + payload_size(slot_size, num_slots);
  }

  static Segment* create_segment(uint slot_size, uint num_slots, Segment* next, MemTag mem_tag);
  static void delete_segment(Segment* segment);

  void copy_to(void* dest) const {
    ::memcpy(dest, _bottom, length() * _slot_size);
  }

  bool is_full() const { return _next_allocate >= _num_slots; }
};

class HGCMonotonicArena::SegmentFreeList {
  static Segment* volatile* next_ptr(Segment& segment) {
    return segment.next_addr();
  }
  using SegmentStack = LockFreeStack<Segment, &SegmentFreeList::next_ptr>;

  SegmentStack _list;

  volatile size_t _num_segments;
  volatile size_t _mem_size;

public:
  SegmentFreeList() : _list(), _num_segments(0), _mem_size(0) { }
  ~SegmentFreeList() { free_all(); }

  void bulk_add(Segment& first, Segment& last, size_t num, size_t mem_size);

  Segment* get();
  Segment* get_all(size_t& num_segments, size_t& mem_size);

  void free_all();

  void print_on(outputStream* out, const char* prefix = "");

  size_t num_segments() const { return Atomic::load(&_num_segments); }
  size_t mem_size() const { return Atomic::load(&_mem_size); }
};

class HGCMonotonicArena::AllocOptions {
protected:
  const MemTag _mem_tag;
  const uint _slot_size;
  const uint _initial_num_slots;
  const uint _max_num_slots;
  const uint _slot_alignment;

public:
  AllocOptions(MemTag mem_tag, uint slot_size, uint initial_num_slots, uint max_num_slots, uint alignment) :
    _mem_tag(mem_tag),
    _slot_size(align_up(slot_size, alignment)),
    _initial_num_slots(initial_num_slots),
    _max_num_slots(max_num_slots),
    _slot_alignment(alignment) {
    assert(_slot_size > 0, "Must be");
    assert(_initial_num_slots > 0, "Must be");
    assert(_max_num_slots > 0, "Must be");
    assert(_slot_alignment > 0, "Must be");
  }

  virtual uint next_num_slots(uint prev_num_slots) const {
    return _initial_num_slots;
  }

  uint slot_size() const { return _slot_size; }
  uint slot_alignment() const { return _slot_alignment; }
  MemTag mem_tag() const {return _mem_tag; }
};

#endif
