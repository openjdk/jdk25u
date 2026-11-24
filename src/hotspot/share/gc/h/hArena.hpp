#ifndef SHARE_GC_H_HARENA_HPP
#define SHARE_GC_H_HARENA_HPP

#include "memory/allocation.hpp"
#include "nmt/memTag.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/pair.hpp"
#include "utilities/tuple.hpp"
#include <cstddef>

class outputStream;
class HGCCardTable;
class HGCRebuildRemSetClosure;
class HGCRegion;

class HGCBitMap : public BitMapView {
public:
  HGCBitMap() : BitMapView() {}
  HGCBitMap(bm_word_t* words, idx_t size_in_bits) :
      BitMapView(words, size_in_bits) {}

  bm_word_t* words() { return map(); }
};

bool hgc_arena_diag_enabled();
bool hgc_mark_diag_enabled();
bool hgc_verify_enabled();

class HGCArena : public CHeapObj<mtGC> {
  friend class VMStructs;

public:
  typedef u4 Offset;

  static constexpr Offset no_span = Offset(-1);

  struct DeadSpanNode {
    u4 size;
    Offset next;
  };
  class Alignments {
    size_t _value;

public:
    constexpr explicit Alignments(size_t alignments) : _value(alignments) {}

    constexpr Alignments() : _value(0) {}

    static constexpr Alignments from_alignments(size_t alignments) {
      return Alignments(alignments);
    }

    static Alignments from_words(size_t words) {
      return Alignments(align_up(words, MinObjAlignment) >> LogMinObjAlignment);
    }

    static Alignments from_bytes(size_t bytes) {
      return Alignments(align_up(bytes, MinObjAlignmentInBytes) >> LogMinObjAlignmentInBytes);
    }

    size_t value() const {
      return _value;
    }

    size_t to_words() const {
      return value() << LogMinObjAlignment;
    }

    size_t to_bytes() const {
      return value() << LogMinObjAlignmentInBytes;
    }

    friend bool operator==(Alignments a, Alignments b) {
      return a.value() == b.value();
    }

    friend bool operator!=(Alignments a, Alignments b) {
      return a.value() != b.value();
    }

    friend bool operator< (Alignments a, Alignments b) {
      return a.value() < b.value();
    }

    friend bool operator<= (Alignments a, Alignments b) {
      return a.value() <= b.value();
    }

    friend bool operator> (Alignments a, Alignments b) {
      return a.value() > b.value();
    }

    friend bool operator>= (Alignments a, Alignments b) {
      return a.value() >= b.value();
    }

    friend bool operator!(Alignments a) {
      return a.value() == 0;
    }

    friend Alignments operator+(Alignments a, Alignments b) {
      return Alignments(a.value() + b.value());
    }

    friend Alignments operator-(Alignments a, Alignments b) {
      return Alignments(a.value() - b.value());
    }

    Alignments& operator+=(Alignments b) {
      _value += b.value();
      return *this;
    }
  };

  struct AllocResult {
    friend HGCArena;

    HeapWord* mem;
    Alignments size;
    Alignments size_of_span;
    Offset offset_of_span;

    AllocResult(HeapWord* m, Alignments s, Alignments ss, Offset o) :
        mem(m), size(s), size_of_span(ss), offset_of_span(o) {}
  };

private:
  size_t _k;

  HeapWord* _start;
  HGCRegion* _region;
  HGCBitMap _black_bitset;
  HGCBitMap _gray_bitset;

  HGCBitMap _pending_word_bitset;

  HGCBitMap _start_bitset;
  volatile u1 _mark_state;
  enum class StartState : u1 {
    incorrect,
    freezing,
    correct
  };
  volatile u1 _start_state;

  u8 _non_empty;

  class ArenaObserver {
    size_t _number_of_alloca = 0;
    size_t _memory_used = 0;
  public:
    size_t number_of_allocations() const {
      return _number_of_alloca;
    }
    size_t memory_used() const {
      return _memory_used;
    }

    void operator+=(size_t size) {
      _memory_used += size;
      ++_number_of_alloca;
    }

    void reset_memory_used() {
      _memory_used = 0;
    }

    void account_memory_used(size_t size) {
      _memory_used += size;
    }
  } _arena_observer;

public:
  HGCArena* next;

  void* operator new(size_t size, size_t k);
  HGCArena(size_t k, HeapWord* start, HGCRegion* region,
            BitMap::bm_word_t* bitmap_storage, BitMap::idx_t arena_bits,
            BitMap::idx_t pending_bits, jlong ctor_start = 0);
  ~HGCArena();

  static jlong ctor_timing_start();
#ifdef ASSERT
  static void print_constructor_stats(const char* phase);
#endif

  void print_on(outputStream* st);
  void print_map_on(outputStream* st);

  size_t size() const;
  HeapWord* start() const;
  HGCRegion* region() const { return _region; }

  Offset offset(HeapWord* ptr) const;
  bool is_black(HeapWord* ptr) const;
  bool is_gray(HeapWord* ptr) const;

  void translate_cards_after_rebuild(HGCCardTable* ct, bool enqueue = true);

  void invalidate_start_bitset();
  void ensure_start_bitset();
  void prepare_start_bitset_for_mark();
  void update_start_bitset();
  bool mark(HeapWord* ptr);
  bool mark_gray(HeapWord* ptr);
  bool has_pending_gray() const { return !_pending_word_bitset.is_empty(); }
  enum class MarkState : u1 {
    outside,
    queued,
    claimed
  };

  bool try_enqueue();
  bool try_claim();
  void release();

  template <typename OopClosureType> void process_gray(OopClosureType* cl);
  template <typename OopClosureType> void oops_on_memregion(HeapWord* start, HeapWord* end, OopClosureType* cl,
                                                             bool only_marked = false);

  template <typename Closure> void oops_black_do(Closure* cl);

  template <typename Closure> void oops_starts_do(Closure* cl);

  size_t memory_used() const { return _arena_observer.memory_used(); }
  size_t number_of_allocations() const { return _arena_observer.number_of_allocations(); }

  size_t count_of_categors() const;
  static size_t count_of_categors_by_k(size_t k);

  static size_t sizeof_me(size_t k);

  static size_t category_of_object_by_size(Alignments size);
  static size_t category_of_span_by_size(Alignments size);

  static size_t span_class_for_request(Alignments size);
  static size_t category_of_request_by_size(Alignments size);

  static u8 bit_of(size_t category);
  static Pair<Alignments, Alignments> require_range_of_object_by_category_of_span(size_t c);
  static Pair<Alignments, Alignments> require_range_of_span_by_category_of_span(size_t c);

  AllocResult alloc(Alignments size);
  AllocResult alloc_at(AllocResult ar);
  AllocResult find_largest_span();

  bool rebuild();
  void prepare_for_mark();

  void clear_mark_bits();

  Offset end_of_object(Offset last_object);
  u1* addr_of_offset(Offset offset);

private:
  DeadSpanNode* node_at(Offset offset);
  size_t highest_category() const;
  size_t smallest_category_at_least(size_t category) const;
  void push_span(Offset span);
#ifdef ASSERT

  void verify_free_lists(const char* where);
  void verify_span_alone(Offset span, Alignments size);
#endif

  Offset _free_spans[];
};

#endif
