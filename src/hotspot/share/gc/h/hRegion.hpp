#ifndef SHARE_GC_H_HREGION_HPP
#define SHARE_GC_H_HREGION_HPP

#include "gc/h/hArena.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutex.hpp"
#include "utilities/globalDefinitions.hpp"
#include <cstddef>

class HGCRegion;
class HGCHeapRegionRemSet;

class HGCVolatileRegionList {
  HGCRegion* _head;
  size_t _size;

public:
  HGCVolatileRegionList();

  size_t size() const { return _size; }
  void push(HGCRegion* region);
  HGCRegion* pop();

  class Iterator {
    HGCRegion* _cur;

  public:
    explicit Iterator(HGCRegion* cur) : _cur(cur) {}

    HGCRegion* operator*() const { return _cur; }
    Iterator& operator++();
    bool operator!=(const Iterator& rhs) const { return _cur != rhs._cur; }
  };

  Iterator begin() const { return Iterator(_head); }
  Iterator end() const { return Iterator(nullptr); }
};

class HGCRegionDoubleList {
  HGCRegion* _first;
  HGCRegion* _last;
  size_t _size;

public:
  HGCRegionDoubleList();

  HGCRegion* first() const { return _first; }
  HGCRegion* last() const { return _last; }
  size_t size() const { return _size; }
  bool is_empty() const { return _first == nullptr; }

  void push_back(HGCRegion* region);
  void push_front(HGCRegion* region);
  HGCRegion* pop();
  void remove(HGCRegion* region);

  bool contains(HGCRegion* region) const;
};

class HGCRegion : public CHeapObj<mtGC> {
  friend class VMStructs;
public:
  static size_t CardsPerRegion;
  static uint LogCardsPerRegion;

  static void setup_region_size();

  enum State : u1 {
    New,
    Full,
    Reclaiming,
    Surv,
    Old,
    Compacted
  };

  Mutex* _lock;

  HGCRegion* next;
  HGCRegion* prev;
  HGCRegion* next_in_cset;

  void* operator new(size_t size, size_t k);
  HGCRegion(HeapWord* start, size_t k);

  HGCRegion(HeapWord* start, size_t k, bool large, size_t large_regions, HGCRegion* large_owner);
  ~HGCRegion();

  HeapWord* start() const;
  HGCArena* pop_arena_at_least(size_t request_category);
  void return_arena(HGCArena* arena);
  HGCArena* acquire_arena_for(HGCArena::Alignments size_in_alignments);
  HGCArena* arena(HeapWord* ptr);
  size_t k() const;
  size_t evristik_k() const;
  void set_evristik_k(size_t k);
  size_t used() const;
  void clear_mark_bits();

  bool is_empty();
  size_t tlab_size() const;
  void add_tlab_size(size_t size);
  void sub_tlab_size(size_t size);

  bool in_cset() const {
    return state() == New || state() == Full;
  }

  State state() const {
    return (_large_owner != nullptr && _large_owner != this)
             ? _large_owner->state() : _state;
  }

  void set_state(State state) {
    _state = state;
  }

  bool is_large() const { return _large_owner != nullptr; }
  bool is_large_owner() const { return _large_owner == this; }
  bool is_large_continuation() const {
    return _large_owner != nullptr && _large_owner != this;
  }
  HGCRegion* large_owner() const { return _large_owner; }
  size_t large_regions() const { return _large_regions; }
  size_t large_object_bytes() const { return _large_object_bytes; }
  void set_large_object_bytes(size_t bytes) { _large_object_bytes = bytes; }
  size_t large_committed_bytes() const { return _large_committed_bytes; }
  void set_large_committed_bytes(size_t bytes) { _large_committed_bytes = bytes; }

  bool mark_large() {
    u1 expected = 0;
    u1* mark = const_cast<u1*>(&_large_mark);
    return Atomic::cmpxchg(mark, expected, static_cast<u1>(1)) == expected;
  }
  bool is_large_marked() const { return _large_mark != 0; }
  void clear_large_mark() { _large_mark = 0; }

  bool pages_uncommitted() const {
    return Atomic::load(&_pages_uncommitted) != 0;
  }
  void set_pages_uncommitted() {
    Atomic::release_store(&_pages_uncommitted, static_cast<u1>(1));
  }

  bool claim_for_cset();

  size_t age() const {
    return _age;
  }

  HGCHeapRegionRemSet* rem_set() const {
    return _rem_set;
  }

  void set_rem_set(HGCHeapRegionRemSet* rem_set) {
    _rem_set = rem_set;
  }

  void rebuild_arenas();
#ifdef ASSERT
  static void print_constructor_stats(const char* phase);
#endif
  void prepare_for_mark();

  void translate_cards_after_rebuild(HGCCardTable* ct, bool enqueue = true);

  void invalidate_start_bitset();

  void prepare_cards_for_allocation(HGCCardTable* ct);

  template <typename Closure>
  void arenas_do(Closure* cl);

  void print_map_on(outputStream* st, size_t index);

private:

  volatile size_t _tlab_size;

  u8 _non_empty_arenas;
  State _state;
  size_t _age;
  HGCHeapRegionRemSet* _rem_set;
  HeapWord* _start;
  BitMap::bm_word_t* _bitmap_storage;
  size_t _k;
  size_t _evristik_k;

  HGCRegion* _large_owner;
  size_t _large_regions;
  size_t _large_object_bytes;
  size_t _large_committed_bytes;
  volatile u1 _large_mark;
  volatile u1 _pages_uncommitted;

  u1 _arenas[];

  HGCArena* arena_at(size_t i);
  size_t arena_slot(size_t arena_category) const;
  HGCArena** free_arena_head(size_t arena_category);
  void set_free_arena_head(size_t arena_category, HGCArena* arena);
  void push_free_arena(size_t arena_category, HGCArena* arena);

  size_t count_of_categories() const {
    return is_large() ? 0 : HGCArena::count_of_categors_by_k(_k);
  }

public:

  static int c2_state_offset_in_bytes()       { return (int)offset_of(HGCRegion, _state); }
  static int c2_large_owner_offset_in_bytes() { return (int)offset_of(HGCRegion, _large_owner); }
};

inline HGCVolatileRegionList::Iterator& HGCVolatileRegionList::Iterator::operator++() {
  _cur = _cur->next_in_cset;
  return *this;
}

#endif
