#include "gc/h/hArena.hpp"
#include "gc/h/hArena.inline.hpp"
#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hOopClosures.inline.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "nmt/memTag.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/count_leading_zeros.hpp"
#include "utilities/count_trailing_zeros.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "utilities/powerOfTwo.hpp"
#include <cstddef>
#include <cstring>
#include <limits>

static bool hgc_arena_verify();

u8 HGCArena::bit_of(size_t category) {
  assert(category < 64, "category %zu does not fit the bitset", category);
  return u8(1) << (63 - category);
}

HGCArena::DeadSpanNode* HGCArena::node_at(Offset offset) {
  return reinterpret_cast<DeadSpanNode*>(addr_of_offset(offset));
}




#ifdef ASSERT
#endif

enum {
  HGC_SPAN_TAKE = 1,
  HGC_SPAN_EXHAUST = 2,
  HGC_SPAN_SPLIT_SAME = 3,
  HGC_SPAN_SPLIT_DIFF = 4,
  HGC_SPAN_PUSH = 5,
  HGC_SPAN_CLEAR = 6
};

#ifdef ASSERT
struct HGCSpanEvent {
  const void* arena;
  u1 op;
  u1 cat_before;
  u1 cat_after;
  u1 pad;
  size_t span;
  size_t size_before;
  size_t req;
  size_t new_start;
  size_t new_size;
  size_t head_before;
  size_t head_after;
  size_t next;
};

enum { HGC_SPAN_EVENTS = 1 << 13 };
static HGCSpanEvent hgc_span_events[HGC_SPAN_EVENTS];
static volatile size_t hgc_span_events_idx = 0;

static void hgc_span_event(const void* arena, int op, size_t span,
                           size_t size_before, size_t req, size_t new_start,
                           size_t new_size, size_t head_before,
                           size_t head_after, size_t next,
                           size_t cat_before, size_t cat_after) {
  if (!hgc_arena_diag_enabled()) {
    return;
  }
  size_t i = Atomic::fetch_then_add(&hgc_span_events_idx, size_t(1));
  HGCSpanEvent& e = hgc_span_events[i & (HGC_SPAN_EVENTS - 1)];
  e.arena = arena;
  e.op = (u1)op;
  e.cat_before = (u1)cat_before;
  e.cat_after = (u1)cat_after;
  e.span = span;
  e.size_before = size_before;
  e.req = req;
  e.new_start = new_start;
  e.new_size = new_size;
  e.head_before = head_before;
  e.head_after = head_after;
  e.next = next;
}

static const char* hgc_span_op_name(int op) {
  switch (op) {
    case HGC_SPAN_TAKE: return "TAKE";
    case HGC_SPAN_EXHAUST: return "EXHAUST";
    case HGC_SPAN_SPLIT_SAME: return "SPLIT_SAME";
    case HGC_SPAN_SPLIT_DIFF: return "SPLIT_DIFF";
    case HGC_SPAN_PUSH: return "PUSH";
    case HGC_SPAN_CLEAR: return "CLEAR";
    default: return "?";
  }
}


#define HGC_SPAN_EVENT(...) hgc_span_event(__VA_ARGS__)
#else
#define HGC_SPAN_EVENT(...) ((void)0)
#endif

#ifdef ASSERT

#endif

size_t HGCArena::highest_category() const {
  assert(_non_empty != 0, "no free spans");

  return 63 - count_trailing_zeros(_non_empty);
}

size_t HGCArena::smallest_category_at_least(size_t category) const {
  u8 mask = u8(-1) >> category;
  u8 available = _non_empty & mask;
  assert(available != 0, "no span of a suitable category");

  return count_leading_zeros(available);
}

void HGCArena::push_span(Offset span) {
  DeadSpanNode* node = node_at(span);
  size_t category = category_of_span_by_size(Alignments(node->size));
  HGC_SPAN_EVENT(this, HGC_SPAN_PUSH, (size_t)span, node->size, 0, 0, 0,
                 (size_t)_free_spans[category], (size_t)span, (size_t)node->next,
                 0, category);
  node->next = _free_spans[category];
  _free_spans[category] = span;
  _non_empty |= bit_of(category);
}

size_t HGCArena::span_class_for_request(Alignments size) {
  return category_of_span_by_size(size);
}

size_t HGCArena::category_of_request_by_size(Alignments size) {
  return category_of_object_by_size(size);
}

HGCArena::AllocResult HGCArena::alloc(Alignments size) {

  size_t category = span_class_for_request(size);
  if (category >= count_of_categors() || _non_empty == 0) {
    log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": alloc %zu alignments, category=%zu exceeds categories=%zu",
                         p2i(_start), size.value(), category, count_of_categors());
    return find_largest_span();
  }

  if ((_non_empty & (u8(-1) >> category)) == 0) {
    log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": alloc %zu alignments, no span in category>=%zu, non_empty=%016lx",
                         p2i(_start), size.value(), category, (unsigned long)_non_empty);
    return find_largest_span();
  }

  size_t span_category = smallest_category_at_least(category);
  Offset span = _free_spans[span_category];
  Alignments size_of_span = Alignments(node_at(span)->size);

  if (size_of_span < size) {

    size_t next_category = span_category + 1;
    if (next_category >= count_of_categors() ||
        (_non_empty & (u8(-1) >> next_category)) == 0) {
      return find_largest_span();
    }
    span_category = smallest_category_at_least(next_category);
    span = _free_spans[span_category];
    size_of_span = Alignments(node_at(span)->size);
    if (size_of_span < size) {
      return find_largest_span();
    }
  }

  log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": alloc %zu alignments, category=%zu -> span category=%zu span=%zu size=%zu",
                       p2i(_start), size.value(), category, span_category,
                       (size_t)span, size_of_span.value());

  return alloc_at({nullptr, size, size_of_span, span});
}

HGCArena::AllocResult HGCArena::alloc_at(AllocResult ar) {
  assert(ar.offset_of_span != no_span, "ar is complete");
  assert(ar.mem == nullptr, "candidate was already committed");
#ifdef ASSERT
  if (hgc_arena_verify()) { verify_free_lists("alloc_at"); }
#endif

  Offset span = ar.offset_of_span;
  DeadSpanNode* node = node_at(span);
  Alignments size_of_span = Alignments(node->size);
#ifdef ASSERT
  if (hgc_arena_verify()) { verify_span_alone(span, size_of_span); }
#endif
  size_t category_of_span = category_of_span_by_size(size_of_span);
  assert(size_of_span == ar.size_of_span, "stale candidate");
  assert(category_of_span < 64, "category fits the bitset");
  HGC_SPAN_EVENT(this, HGC_SPAN_TAKE, (size_t)span, size_of_span.value(), ar.size.value(),
                 0, 0, (size_t)_free_spans[category_of_span], (size_t)node->next,
                 (size_t)node->next, category_of_span, category_of_span);
  if (_free_spans[category_of_span] != span) {
#ifdef ASSERT
    if (hgc_arena_diag_enabled()) {
      verify_free_lists("alloc_at-bad");
    }
#endif
  }
  assert(_free_spans[category_of_span] == span, "span is not the head of its category");
  assert(size_of_span >= ar.size, "span does not fit the request");

  log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": alloc_at span=%zu span_size=%zu take=%zu",
                       p2i(_start), (size_t)span, size_of_span.value(), ar.size.value());

  Offset next = node->next;
  Alignments new_size_of_span = size_of_span - ar.size;

  if (new_size_of_span.value() == 0) {
    log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": alloc_at span=%zu exhausted, category=%zu",
                         p2i(_start), (size_t)span, category_of_span);
    HGC_SPAN_EVENT(this, HGC_SPAN_EXHAUST, (size_t)span, size_of_span.value(), ar.size.value(),
                   0, 0, (size_t)_free_spans[category_of_span], (size_t)next, (size_t)next,
                   category_of_span, category_of_span);
    _free_spans[category_of_span] = next;
    if (next == no_span) {
      _non_empty &= ~bit_of(category_of_span);
    }
  } else {
    Offset new_start_of_span = span + ar.size.value();
    size_t new_category_of_span = category_of_span_by_size(new_size_of_span);
    assert(new_category_of_span < 64, "category fits the bitset");
    HGC_SPAN_EVENT(this,
                   new_category_of_span == category_of_span ? HGC_SPAN_SPLIT_SAME : HGC_SPAN_SPLIT_DIFF,
                   (size_t)span, size_of_span.value(), ar.size.value(),
                   (size_t)new_start_of_span, new_size_of_span.value(),
                   (size_t)_free_spans[category_of_span],
                   new_category_of_span == category_of_span
                       ? (size_t)new_start_of_span
                       : (size_t)_free_spans[new_category_of_span],
                   (size_t)next, category_of_span, new_category_of_span);
    node_at(new_start_of_span)->size = new_size_of_span.value();
    if (new_category_of_span == category_of_span) {

      node_at(new_start_of_span)->next = next;
      _free_spans[category_of_span] = new_start_of_span;
    } else {

      node_at(new_start_of_span)->next = _free_spans[new_category_of_span];
      _free_spans[new_category_of_span] = new_start_of_span;
      _non_empty |= bit_of(new_category_of_span);
      _free_spans[category_of_span] = next;
      if (next == no_span) {
        _non_empty &= ~bit_of(category_of_span);
      }
    }

    log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": alloc_at span=%zu -> %zu, size=%zu -> %zu, category=%zu -> %zu",
                         p2i(_start), (size_t)span, (size_t)new_start_of_span,
                         size_of_span.value(), new_size_of_span.value(),
                         category_of_span, new_category_of_span);
  }

  _arena_observer += ar.size.to_bytes();

  return {reinterpret_cast<HeapWord*>(addr_of_offset(span)), ar.size,
    size_of_span, no_span};
}

HGCArena::AllocResult HGCArena::find_largest_span() {
  if (_non_empty == 0) {
    log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": find_largest found nothing", p2i(_start));
    return {nullptr, Alignments(0), Alignments(0), no_span};
  }

  size_t category = highest_category();
  Offset span = _free_spans[category];
  Alignments size_of_span = Alignments(node_at(span)->size);

  log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": find_largest category=%zu span=%zu size=%zu",
                       p2i(_start), category, (size_t)span, size_of_span.value());

  return {nullptr, size_of_span, size_of_span, span};
}

bool HGCArena::rebuild() {

  {
    const BitMap::bm_word_t* gray = _gray_bitset.words();
    const BitMap::bm_word_t* black = _black_bitset.words();
    const size_t words = _gray_bitset.size() / BitsPerWord;
    for (size_t w = 0; w < words; ++w) {
      if ((gray[w] & ~black[w]) != 0) {
        assert(false, "gray object was not scanned before rebuild: word=%zu gray=%016lx black=%016lx",
               w, (unsigned long)gray[w], (unsigned long)black[w]);
      }
    }
  }

  Alignments free_space(0);
  log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": rebuild begin, used=%zu", p2i(_start), memory_used());

  HGC_SPAN_EVENT(this, HGC_SPAN_CLEAR, 0, size() >> LogMinObjAlignmentInBytes, 0, 0, 0, 0, 0, 0, 0, 0);
  _non_empty = 0;
  for (size_t c = 0; c < count_of_categors(); ++c) {
    _free_spans[c] = no_span;
  }

  Offset last_object = -1;
  Offset last_free_end = 0;
  Offset last_live_end = 0;
  BitMap::idx_t end_of_arena = _black_bitset.size();
  BitMap::idx_t end_of_span = -1;
  do {
    end_of_span = _black_bitset.find_first_set_bit(end_of_span + 1, end_of_arena);
    Offset start_of_span = last_live_end;
    last_object = end_of_span;
    if (end_of_span != end_of_arena) {
      oop live = cast_to_oop(addr_of_offset(static_cast<Offset>(end_of_span)));
      if (!oopDesc::is_oop(live, true)) {
        tty->print_cr("REBUILD BAD START: arena=" PTR_FORMAT " start=%zu size=%zu bit=0x%016lx",
                      p2i(_start), (size_t)end_of_span, size(), (uintptr_t)end_of_span);
      } else {
        Offset live_end = end_of_object(static_cast<Offset>(end_of_span));
        if (live_end < end_of_span || live_end > (Offset)end_of_arena) {
          tty->print_cr("REBUILD BAD SIZE: arena=" PTR_FORMAT " start=%zu end=%zu arena_end=%zu",
                        p2i(_start), (size_t)end_of_span, (size_t)live_end, (size_t)end_of_arena);
        } else if (end_of_span < last_live_end) {
          tty->print_cr("REBUILD LIVE OVERLAP: arena=" PTR_FORMAT " start=%zu prev_end=%zu",
                        p2i(_start), (size_t)end_of_span, (size_t)last_live_end);
        }
        last_live_end = live_end;
      }
    }
    if (start_of_span != end_of_span) {
      if (start_of_span > end_of_span || start_of_span < last_free_end) {
        static volatile int overlap_reported = 0;
        if (Atomic::fetch_then_add(&overlap_reported, 1) < 10) {
          tty->print_cr("REBUILD OVERLAP: arena=" PTR_FORMAT " span=[%zu,%zu) prev_end=%zu size=%zu",
                        p2i(_start), (size_t)start_of_span, (size_t)end_of_span, (size_t)last_free_end,
                        (size_t)(end_of_span - start_of_span));
        }
      }
      last_free_end = end_of_span;

      Offset free_start = start_of_span;
      Alignments span_size = Alignments::from_alignments(end_of_span - start_of_span);
      node_at(free_start)->size = span_size.value();
      node_at(free_start)->next = no_span;
      push_span(free_start);
      free_space += span_size;
    }
  } while (end_of_span != end_of_arena);

  _arena_observer.reset_memory_used();
  _arena_observer.account_memory_used(size() - free_space.to_bytes());

  bool is_empty = free_space.to_bytes() == size();
#ifdef ASSERT
  if (hgc_arena_verify()) { verify_free_lists("rebuild"); }
#endif
  log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": rebuild end free=%zu used=%zu empty=%d",
                       p2i(_start), free_space.to_bytes(), memory_used(), is_empty);

  if (hgc_verify_enabled()) {

    const BitMap::bm_word_t* gray = _gray_bitset.words();
    const BitMap::bm_word_t* black = _black_bitset.words();
    const size_t words = _gray_bitset.size() / BitsPerWord;
    size_t gray_count = _gray_bitset.count_one_bits();
    size_t black_count = _black_bitset.count_one_bits();
    size_t pending = 0;
    for (size_t i = 0; i < words; ++i) {
      pending += population_count(gray[i] & ~black[i]);
    }
    log_info(gc, mark)("HGC arena " PTR_FORMAT ": rebuild marks gray=%zu black=%zu pending=%zu%s",
                       p2i(_start), gray_count, black_count, pending,
                       pending == 0 ? "" : " PENDING GRAY");
    assert(pending == 0, "concurrent mark left unscanned gray objects in arena " PTR_FORMAT,
           p2i(_start));
  }

  return is_empty;
}

void HGCArena::prepare_for_mark() {
  prepare_start_bitset_for_mark();
  _black_bitset.clear();
  _gray_bitset.clear();
  _pending_word_bitset.clear();
  Atomic::release_store(&_mark_state, static_cast<u1>(MarkState::outside));
}

void HGCArena::clear_mark_bits() {

  ensure_start_bitset();
  _black_bitset.clear();
  _gray_bitset.clear();
  _pending_word_bitset.clear();
  Atomic::release_store(&_mark_state, static_cast<u1>(MarkState::outside));
}

HeapWord* HGCArena::start() const {
  return _start;
}

HGCArena::Offset HGCArena::offset(HeapWord* ptr) const {
  return (ptr - _start) >> LogMinObjAlignment;
}

bool HGCArena::is_black(HeapWord* ptr) const {
  return _black_bitset.at(offset(ptr));
}

bool HGCArena::is_gray(HeapWord* ptr) const {
  return _gray_bitset.at(offset(ptr));
}

bool HGCArena::mark(HeapWord* ptr) {
  return _black_bitset.par_set_bit(offset(ptr));
}

bool HGCArena::mark_gray(HeapWord* ptr) {

  BitMap::idx_t index = offset(ptr);
  if (_black_bitset.at(index)) {
    return false;
  }
  if (!_gray_bitset.par_set_bit(index)) {
    return false;
  }

  _start_bitset.par_set_bit(index);
  _pending_word_bitset.par_set_bit(index / BitsPerWord);
  return true;
}

static void hgc_force_card_value(CardTable::CardValue* card, CardTable::CardValue to) {
  for (;;) {
    CardTable::CardValue value = Atomic::load(card);
    if (value == to) {
      return;
    }
    if (Atomic::cmpxchg(card, value, to) == value) {
      return;
    }
  }
}

static void hgc_translate_dirty_card(CardTable::CardValue* card,
                                     CardTable::CardValue to) {
  for (;;) {
    CardTable::CardValue value = Atomic::load(card);
    if (value == to) {
      return;
    }
    const bool was_dirty = HGCCardTable::is_dirty_card_value(value);
    if (Atomic::cmpxchg(card, value, to) == value) {
      if (!was_dirty) {

        HGCBarrierSet::mark_card_dirty_and_enqueue(card, to);
      }
      return;
    }
  }
}

void HGCArena::translate_cards_after_rebuild(HGCCardTable* ct, bool enqueue) {
  const size_t card_bytes = HGCCardTable::card_size();
  assert((size() % card_bytes) == 0, "arena is not card aligned");
  const size_t card_count = size() / card_bytes;
  if (card_count == 0) {
    return;
  }

  u1* free_state = NEW_C_HEAP_ARRAY(u1, card_count, mtGC);
  memset(free_state, 0, card_count);

  const uintptr_t base = reinterpret_cast<uintptr_t>(_start);
  for (size_t category = 0; category < count_of_categors(); ++category) {
    for (Offset span = _free_spans[category]; span != no_span; span = node_at(span)->next) {
      const uintptr_t start = reinterpret_cast<uintptr_t>(addr_of_offset(span));
      const uintptr_t end = start + Alignments(node_at(span)->size).to_bytes();
      const size_t first = (start - base) / card_bytes;
      const size_t last = (end - 1 - base) / card_bytes;
      for (size_t i = first; i <= last; ++i) {
        const uintptr_t card_start = base + i * card_bytes;
        const uintptr_t card_end = card_start + card_bytes;
        if (start <= card_start && end >= card_end) {
          free_state[i] = 1;
        } else if (free_state[i] == 0) {
          free_state[i] = 2;
        }
      }
    }
  }

  for (size_t i = 0; i < card_count; ++i) {
    CardTable::CardValue* card = ct->byte_for(reinterpret_cast<void*>(base + i * card_bytes));
    if (free_state[i] == 1) {

      hgc_force_card_value(card, HGCCardTable::young_card_val());
      continue;
    }
    if (free_state[i] == 2) {

      if (enqueue) {
        hgc_translate_dirty_card(card, HGCCardTable::mixed_dirty_card_val());
      } else {
        hgc_force_card_value(card, HGCCardTable::mixed_clean_card_val());
      }
      continue;
    }

    if (enqueue) {
      hgc_translate_dirty_card(card, HGCCardTable::dirty_card_val());
    } else {
      hgc_force_card_value(card, HGCCardTable::clean_card_val());
    }
  }

  FREE_C_HEAP_ARRAY(u1, free_state);
}

void HGCArena::invalidate_start_bitset() {
  Atomic::release_store(&_start_state, static_cast<u1>(StartState::incorrect));
}

void HGCArena::prepare_start_bitset_for_mark() {

  ensure_start_bitset();
}

void HGCArena::ensure_start_bitset() {
  if (_start_state == static_cast<u1>(StartState::correct)) {
    return;
  }

  for (;;) {
    u1 state = Atomic::load_acquire(&_start_state);
    if (state == static_cast<u1>(StartState::correct)) {
      return;
    }
    if (state == static_cast<u1>(StartState::incorrect) &&
        Atomic::cmpxchg(&_start_state,
                        static_cast<u1>(StartState::incorrect),
                        static_cast<u1>(StartState::freezing),
                        memory_order_acq_rel) ==
            static_cast<u1>(StartState::incorrect)) {
      update_start_bitset();
      Atomic::release_store(&_start_state, static_cast<u1>(StartState::correct));
      return;
    }

    os::naked_yield();
  }
}

void HGCArena::update_start_bitset() {

  const size_t word_count = _black_bitset.size() / BitsPerWord;
  assert(_start_bitset.size() == _black_bitset.size(), "bitmap sizes differ");
  memcpy(_start_bitset.words(), _black_bitset.words(),
         word_count * sizeof(BitMap::bm_word_t));
}

bool HGCArena::try_enqueue() {
  for (;;) {
    u1 state = Atomic::load_acquire(&_mark_state);

    if (state == static_cast<u1>(MarkState::queued) ||
        state == static_cast<u1>(MarkState::claimed)) {
      return false;
    }
    if (Atomic::cmpxchg(&_mark_state, state, static_cast<u1>(MarkState::queued),
                        memory_order_acq_rel) == state) {
      return true;
    }
  }
}

bool HGCArena::try_claim() {
  return Atomic::cmpxchg(&_mark_state, static_cast<u1>(MarkState::queued),
                         static_cast<u1>(MarkState::claimed), memory_order_acq_rel) ==
         static_cast<u1>(MarkState::queued);
}

void HGCArena::release() {
  Atomic::cmpxchg(&_mark_state, static_cast<u1>(MarkState::claimed),
                  static_cast<u1>(MarkState::outside), memory_order_acq_rel);
}

u1* HGCArena::addr_of_offset(Offset offset) {
  return reinterpret_cast<u1*>(_start) + Alignments(offset).to_bytes();
}

size_t HGCArena::count_of_categors_by_k(size_t k) {
  return category_of_span_by_size(Alignments::from_bytes(HGCRegionSize >> k)) + 1;
}

size_t HGCArena::count_of_categors() const {
  return count_of_categors_by_k(_k);
}

HGCArena::Offset HGCArena::end_of_object(Offset last_object) {
  oop obj = cast_to_oop(addr_of_offset(last_object));
  return last_object + Alignments::from_words(obj->size()).value();
}

struct HGCAlignCategory {
  size_t first;
  size_t last;
};

static constexpr HGCAlignCategory hgc_align_categories[64] = {
  {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 7}, {8, 9}, {10, 11},
  {12, 14}, {15, 18}, {19, 22}, {23, 27}, {28, 31}, {32, 35}, {36, 40},
  {41, 46}, {47, 53}, {54, 61}, {62, 70}, {71, 80}, {81, 90}, {91, 100},
  {101, 120}, {121, 140}, {141, 160}, {161, 200}, {201, 250}, {251, 300},
  {301, 350}, {351, 400}, {401, 450}, {451, 512}, {513, 1024},
  {1025, 1536}, {1537, 2048}, {2049, 3072}, {3073, 4096},
  {4097, 6144}, {6145, 8192}, {8193, 12288}, {12289, 16384},
  {16385, 24576}, {24577, 32768}, {32769, 49152}, {49153, 65536},
  {65537, 98304}, {98305, 131072}, {131073, 196608}, {196609, 262144},
  {262145, 393216}, {393217, 524288}, {524289, 786432}, {786433, 1048576},
  {1048577, 1572864}, {1572865, 2097152}, {2097153, 3145728},
  {3145729, 4194304}, {4194305, 6291456}, {6291457, 8388608},
  {8388609, 12582912}, {12582913, 16777216}, {16777217, 25165824},
  {25165825, 33554432}, {33554433, std::numeric_limits<size_t>::max()}
};

static size_t hgc_align_category_of(size_t alignments) {
  assert(alignments > 0, "alignments must be positive");
  size_t lo = 0;
  size_t hi = 64;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (alignments <= hgc_align_categories[mid].last) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo < 64 ? lo : 63;
}

size_t HGCArena::category_of_span_by_size(Alignments size) {
  assert(size > Alignments(0), "size should be != 0");
  return hgc_align_category_of(size.value());
}

size_t HGCArena::category_of_object_by_size(Alignments size) {
  assert(size > Alignments(0), "size should be != 0");
  size_t value = size.value();
  size_t lo = 0;
  size_t hi = 64;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (hgc_align_categories[mid].first >= value) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo < 64 ? lo : 63;
}

Pair<HGCArena::Alignments, HGCArena::Alignments>
HGCArena::require_range_of_object_by_category_of_span(size_t c) {
  assert(c < 64, "category out of range");

  size_t first = (c == 0) ? 1 : hgc_align_categories[c - 1].first + 1;
  size_t last = hgc_align_categories[c].first;
  return {Alignments(first), Alignments(last)};
}

Pair<HGCArena::Alignments, HGCArena::Alignments>
HGCArena::require_range_of_span_by_category_of_span(size_t c) {
  assert(c < 64, "category out of range");
  return {Alignments(hgc_align_categories[c].first),
          Alignments(hgc_align_categories[c].last)};
}

size_t HGCArena::size() const {
  return HGCRegionSize >> _k;
}

size_t HGCArena::sizeof_me(size_t k) {
  return sizeof(HGCArena) + count_of_categors_by_k(k) * sizeof(Offset);
}

void* HGCArena::operator new(size_t size, size_t k) {
  return CHeapObj<mtGC>::operator new(size + count_of_categors_by_k(k) * sizeof(Offset));
}

#define SIZE_OF_BITSET_FOR_HGCARENA (HGCRegionSize >> (LogMinObjAlignmentInBytes + k))

#ifdef ASSERT
static volatile jlong hgc_arena_ctor_total_ns;
static volatile jlong hgc_arena_ctor_body_ns;
static volatile size_t hgc_arena_ctor_count;

static bool hgc_arena_ctor_stats_enabled() {
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_ALLOC_STATS") == nullptr) ? 0 : 1;
  }
  return value == 1;
}
#endif

jlong HGCArena::ctor_timing_start() {
#ifdef ASSERT
  if (hgc_arena_ctor_stats_enabled()) {
    return os::elapsed_counter();
  }
#endif
  return 0;
}

#ifdef ASSERT
void HGCArena::print_constructor_stats(const char* phase) {
  if (!hgc_arena_ctor_stats_enabled() || hgc_arena_ctor_count == 0) {
    return;
  }
  const size_t n = hgc_arena_ctor_count;
  const jlong total = hgc_arena_ctor_total_ns;
  const jlong body = hgc_arena_ctor_body_ns;
  tty->print_cr("HGC ARENA CTOR TIMES (%s): n=%zu total avg=%.1fns; "
                "member_init avg=%.1fns; body avg=%.1fns",
                phase, n,
                double(total) / double(n),
                double(total - body) / double(n),
                double(body) / double(n));
}
#endif
HGCArena::HGCArena(size_t k, HeapWord* start, HGCRegion* region,
                   BitMap::bm_word_t* bitmap_storage, BitMap::idx_t arena_bits,
                   BitMap::idx_t pending_bits, jlong ctor_start) :
  _k(k),
  _start(start),
  _region(region),
  _black_bitset(bitmap_storage, arena_bits),
  _gray_bitset(bitmap_storage + BitMap::calc_size_in_words(arena_bits), arena_bits),
  _pending_word_bitset(bitmap_storage + 2 * BitMap::calc_size_in_words(arena_bits), pending_bits),
  _start_bitset(bitmap_storage + 2 * BitMap::calc_size_in_words(arena_bits) +
                BitMap::calc_size_in_words(pending_bits), arena_bits),
  _mark_state(static_cast<u1>(MarkState::outside)),
  _start_state(static_cast<u1>(StartState::correct)),
  _non_empty(0),
  next(nullptr) {
#ifdef ASSERT
  const bool ctor_stats = ctor_start != 0;
  jlong body_start = 0;
  if (ctor_stats) {
    body_start = os::elapsed_counter();
  }
#endif
  size_t categories = count_of_categors();
  for (size_t i = 0; i < categories; ++i) {
    _free_spans[i] = no_span;
  }

  Offset span = 0;
  node_at(span)->size = Alignments::from_bytes(size()).value();
  node_at(span)->next = no_span;
  push_span(span);
#ifdef ASSERT
  if (ctor_stats) {
    const jlong now = os::elapsed_counter();
    Atomic::add(&hgc_arena_ctor_total_ns, now - ctor_start);
    Atomic::add(&hgc_arena_ctor_body_ns, now - body_start);
    Atomic::add(&hgc_arena_ctor_count, size_t(1));
  }
#endif

  log_trace(gc, alloc)("HGC arena " PTR_FORMAT ": created k=%zu size=%zu bytes, categories=%zu, span=%zu alignments",
                       p2i(_start), _k, size(), categories, (size_t)node_at(span)->size);
}

void HGCArena::print_on(outputStream* st) {
  st->print_cr("HGC arena " PTR_FORMAT " k=%zu size=%zu bytes, used=%zu bytes, allocations=%zu",
               p2i(_start), _k, size(), memory_used(), number_of_allocations());
  for (size_t c = 0; c < count_of_categors(); ++c) {
    st->print_cr("  category[%zu] -> offset %zu%s", c, (size_t)_free_spans[c],
                 (_non_empty & bit_of(c)) ? "" : " (empty)");
    for (Offset span = _free_spans[c]; span != no_span; span = node_at(span)->next) {
      st->print_cr("    span offset=%zu size=%zu alignments",
                   (size_t)span, (size_t)node_at(span)->size);
    }
  }
}

void HGCArena::print_map_on(outputStream* st) {
  st->print_cr("    {");
  st->print_cr("      \"start\": \"" PTR_FORMAT "\",", p2i(_start));
  st->print_cr("      \"k\": %zu,", _k);
  st->print_cr("      \"size\": %zu,", size());
  st->print_cr("      \"used\": %zu,", memory_used());
  st->print_cr("      \"allocations\": %zu,", number_of_allocations());
  st->print_cr("      \"spans\": [");
  bool first = true;
  for (size_t c = 0; c < count_of_categors(); ++c) {
    for (Offset span = _free_spans[c]; span != no_span; span = node_at(span)->next) {
      st->print_cr("        %s{ \"offset\": %zu, \"size\": %zu, \"category\": %zu }",
                   first ? "" : ",",
                   (size_t)span, (size_t)node_at(span)->size, c);
      first = false;
    }
  }
  st->print_cr("      ]");
  st->print_cr("    }");
}

HGCArena::~HGCArena() {
}

static void hgc_dump_span_events(const void* arena) {
  size_t total = MIN2((size_t)Atomic::load(&hgc_span_events_idx), (size_t)HGC_SPAN_EVENTS);
  size_t printed = 0;
  tty->print_cr("  EVENTS (last %zu total, showing this arena):", total);
  for (size_t n = 0; n < total; ++n) {
    size_t i = total - 1 - n;
    HGCSpanEvent& e = hgc_span_events[i & (HGC_SPAN_EVENTS - 1)];
    if (e.arena != arena) {
      continue;
    }
    tty->print_cr("    #%zu %-10s span=%zu size=%zu req=%zu new=[%zu,%zu) cat=%u->%u head=%zu->%zu next=%zu",
                  i, hgc_span_op_name(e.op), e.span, e.size_before, e.req,
                  e.new_start, e.new_size, (unsigned)e.cat_before, (unsigned)e.cat_after,
                  e.head_before, e.head_after, e.next);
    if (++printed >= 60) {
      break;
    }
  }
}

void HGCArena::verify_span_alone(Offset span, Alignments size) {
  size_t total = 0;
  for (size_t c = 0; c < count_of_categors(); ++c) {
    Offset other = _free_spans[c];
    while (other != no_span) {
      if (++total > 4096) {
        return;
      }
      Alignments other_size(node_at(other)->size);
      if (other != span) {
        size_t s = (size_t)span;
        size_t o = (size_t)other;
        if ((o > s && o < s + size.value()) || (s > o && s < o + other_size.value())) {
          static volatile int reported = 0;
          if (Atomic::fetch_then_add(&reported, 1) < 10) {
            tty->print_cr("SPAN OVERLAP: arena=" PTR_FORMAT " take=[%zu,%zu) other=[%zu,%zu)",
                          p2i(_start), s, s + size.value(), o, o + other_size.value());
            hgc_dump_span_events(this);
          }
          return;
        }
      }
      other = node_at(other)->next;
    }
  }
}

void HGCArena::verify_free_lists(const char* where) {
  static volatile int reported = 0;
  if (Atomic::load(&reported) > 20) {
    return;
  }
  auto dump_state = [&](size_t bad_cat, size_t bad_span, size_t bad_size, size_t bad_next) {
    tty->print_cr("ARENA DUMP(%s): start=" PTR_FORMAT " alignments=%zu k=%zu cats=%zu non_empty=%016lx",
                  where, p2i(_start), size() >> LogMinObjAlignmentInBytes, _k,
                  count_of_categors(), (unsigned long)_non_empty);
    if (bad_cat != SIZE_MAX) {
      tty->print_cr("  bad node: cat=%zu span=%zu size=%zu next=%zu",
                    bad_cat, bad_span, bad_size, bad_next);
    }
    size_t offs[512];
    size_t sizes[512];
    size_t cats[512];
    size_t count = 0;
    for (size_t c = 0; c < count_of_categors(); ++c) {
      tty->print_cr("  cat %2zu: head=%zu", c, (size_t)_free_spans[c]);
      Offset span = _free_spans[c];
      size_t steps = 0;
      while (span != no_span && steps < 64) {
        size_t node_size = node_at(span)->size;
        size_t node_cat = category_of_span_by_size(Alignments(node_size));
        tty->print_cr("      span=%zu size=%zu cat=%zu next=%u", (size_t)span, node_size,
                      node_cat, node_at(span)->next);
        if (count < 512) {
          offs[count] = (size_t)span;
          sizes[count] = node_size;
          cats[count] = c;
          ++count;
        }
        span = node_at(span)->next;
        ++steps;
      }
      if (steps >= 64) {
        tty->print_cr("      ... (chain truncated)");
      }
    }

    for (size_t i = 0; i < count; ++i) {
      for (size_t j = i + 1; j < count; ++j) {
        if (offs[i] == offs[j]) {
          tty->print_cr("  DUPLICATE span=%zu in cat=%zu and cat=%zu", offs[i], cats[i], cats[j]);
        } else if (offs[i] < offs[j] && offs[i] + sizes[i] > offs[j]) {
          tty->print_cr("  OVERLAP [%zu,%zu) cat=%zu and [%zu,%zu) cat=%zu",
                        offs[i], offs[i] + sizes[i], cats[i], offs[j], offs[j] + sizes[j], cats[j]);
        } else if (offs[j] < offs[i] && offs[j] + sizes[j] > offs[i]) {
          tty->print_cr("  OVERLAP [%zu,%zu) cat=%zu and [%zu,%zu) cat=%zu",
                        offs[j], offs[j] + sizes[j], cats[j], offs[i], offs[i] + sizes[i], cats[i]);
        }
      }
    }
    hgc_dump_span_events(this);
  };

  for (size_t c = 0; c < count_of_categors(); ++c) {
    Offset span = _free_spans[c];
    size_t steps = 0;
    while (span != no_span) {
      if ((size_t)span >= (size() >> LogMinObjAlignmentInBytes) || ++steps > 100000) {
        if (Atomic::fetch_then_add(&reported, 1) < 20) {
          tty->print_cr("FREE LIST BAD(%s): arena=" PTR_FORMAT " cat=%zu span=%zu steps=%zu",
                        where, p2i(_start), c, (size_t)span, steps);
          dump_state(SIZE_MAX, (size_t)span, 0, 0);
        }
        return;
      }
      size_t node_cat = category_of_span_by_size(Alignments(node_at(span)->size));
      if (node_cat != c) {
        if (Atomic::fetch_then_add(&reported, 1) < 20) {
          tty->print_cr("FREE LIST BAD(%s): arena=" PTR_FORMAT " list=%zu span=%zu size=%u node_cat=%zu next=%u",
                        where, p2i(_start), c, (size_t)span, node_at(span)->size, node_cat,
                        node_at(span)->next);
          dump_state(c, (size_t)span, node_at(span)->size, node_at(span)->next);
        }
        return;
      }
      span = node_at(span)->next;
    }
  }
}

bool hgc_arena_diag_enabled() {
#ifdef ASSERT
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_ARENA_DIAG") == nullptr) ? 0 : 1;
  }
  return value == 1;
#else
  return false;
#endif
}

bool hgc_mark_diag_enabled() {
#ifdef ASSERT
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_MARK_DIAG") == nullptr) ? 0 : 1;
  }
  return value == 1;
#else
  return false;
#endif
}

bool hgc_verify_enabled() {
#ifdef ASSERT
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_VERIFY_ALL") == nullptr &&
             ::getenv("HGC_VERIFY_MARKS") == nullptr) ? 0 : 1;
  }
  return value == 1;
#else
  return false;
#endif
}

static bool hgc_arena_verify() {
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_ARENA_VERIFY") == nullptr) ? 0 : 1;
  }
  return value == 1;
}
