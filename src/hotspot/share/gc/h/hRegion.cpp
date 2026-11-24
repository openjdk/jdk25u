#include "gc/h/hArena.hpp"
#include "gc/h/hArena.inline.hpp"
#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/h/hRemSet.hpp"
#include "gc/h/hRegionRemSet.inline.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "logging/log.hpp"
#include "memory/iterator.hpp"
#include "oops/access.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "utilities/count_leading_zeros.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "runtime/globals_extension.hpp"
#include <cstddef>
#include <cstring>

#ifdef ASSERT
enum HGCRegionCtorPart : uint {
  HGC_REGION_CTOR_COMMIT,
  HGC_REGION_CTOR_BITMAPS,
  HGC_REGION_CTOR_ARENAS,
  HGC_REGION_CTOR_OTHER,
  HGC_REGION_CTOR_COUNT
};

static volatile jlong hgc_region_ctor_ns[HGC_REGION_CTOR_COUNT];
static volatile size_t hgc_region_ctor_count;

static bool hgc_region_ctor_stats_enabled();
static void hgc_region_ctor_add(HGCRegionCtorPart part, jlong ns);


#endif

void* HGCRegion::operator new(size_t size, size_t k) {
  return CHeapObj<mtGC>::operator new(size + (1 << k) * HGCArena::sizeof_me(k) +
      sizeof(HGCArena*) * HGCArena::count_of_categors_by_k(k));
}

size_t HGCRegion::CardsPerRegion = 0;
uint HGCRegion::LogCardsPerRegion = 0;

void HGCRegion::setup_region_size() {
  guarantee(CardsPerRegion == 0, "we should only set it once");
  CardsPerRegion = HGCRegionSize >> HGCCardTable::card_shift();
  LogCardsPerRegion = log2i_exact(CardsPerRegion);
}

HGCVolatileRegionList::HGCVolatileRegionList() :
  _head(nullptr),
  _size(0) {}

HGCRegion* HGCVolatileRegionList::pop() {
  if (_head == nullptr) return nullptr;

  auto cur = _head;

  for (;;) {
    auto next = cur->next_in_cset;
    auto observed = Atomic::cmpxchg(&_head, cur, next);

    if (observed == cur)
      break;

    cur = observed;
    if (cur == nullptr) return nullptr;
  }
  Atomic::sub(&_size, (size_t)1);
  return cur;
}

void HGCVolatileRegionList::push(HGCRegion* region) {
  HGCRegion* old_head = Atomic::load(&_head);

  for (;;) {
    region->next_in_cset = old_head;
    auto observed = Atomic::cmpxchg(&_head, old_head, region);
    if (observed == old_head) {
      break;
    }
    old_head = observed;
  }
  Atomic::add(&_size, (size_t)1);
}

HGCRegionDoubleList::HGCRegionDoubleList() :
    _first(nullptr),
    _last(nullptr),
    _size(0) {}

void HGCRegionDoubleList::push_back(HGCRegion* region) {
  if (hgc_verify_enabled()) {
    assert(!contains(region), "region already in list");
  }
  region->next = nullptr;
  region->prev = _last;
  if (_last != nullptr) {
    _last->next = region;
  } else {
    _first = region;
  }
  _last = region;
  ++_size;
}

void HGCRegionDoubleList::push_front(HGCRegion* region) {
  if (hgc_verify_enabled()) {
    assert(!contains(region), "region already in list");
  }
  region->prev = nullptr;
  region->next = _first;
  if (_first != nullptr) {
    _first->prev = region;
  } else {
    _last = region;
  }
  _first = region;
  ++_size;
}

HGCRegion* HGCRegionDoubleList::pop() {
  HGCRegion* region = _first;
  if (region != nullptr) {
    remove(region);
  }
  return region;
}

void HGCRegionDoubleList::remove(HGCRegion* region) {
  if (region->prev != nullptr) {
    region->prev->next = region->next;
  } else {
    _first = region->next;
  }
  if (region->next != nullptr) {
    region->next->prev = region->prev;
  } else {
    _last = region->prev;
  }
  region->next = nullptr;
  region->prev = nullptr;
  --_size;
}

bool HGCRegionDoubleList::contains(HGCRegion* region) const {
  size_t n = 0;
  for (HGCRegion* r = _first; r != nullptr && n <= _size; r = r->next, ++n) {
    if (r == region) {
      return true;
    }
  }
  return false;
}

HGCRegion::HGCRegion(HeapWord* start, size_t k) :
    HGCRegion(start, k, false, 0, nullptr) {}

HGCRegion::HGCRegion(HeapWord* start, size_t k, bool large, size_t large_regions, HGCRegion* large_owner) :
    _lock(new Mutex(Mutex::nosafepoint, "HGCRegion")),
    next(nullptr),
    prev(nullptr),
    next_in_cset(nullptr),
    _tlab_size(0),
    _non_empty_arenas(0),
    _state(New),
    _age(0),
    _rem_set(nullptr),
    _start(start),
    _bitmap_storage(nullptr),
    _k(k),
    _evristik_k(k),
    _large_owner(large ? (large_owner != nullptr ? large_owner : this) : nullptr),
    _large_regions((large && large_owner == nullptr) ? large_regions : 0),
    _large_object_bytes(0),
    _large_committed_bytes(0),
    _large_mark(0),
    _pages_uncommitted(0) {
  assert(start != nullptr, "start must not be null");

  if (is_large()) {

    log_trace(gc, heap)("HGC region " PTR_FORMAT ": large %s, regions=%zu",
                        p2i(_start), is_large_owner() ? "owner" : "continuation",
                        _large_regions);
    return;
  }

  assert(HGCRegionSize >> _k <= HGCMaxArenaSize, "invalid k");
  assert(HGCRegionSize >> _k >= HGCMinArenaSize, "invalid k");
#ifdef ASSERT
  const bool ctor_stats = hgc_region_ctor_stats_enabled();
  jlong ctor_start = 0;
  jlong commit_end = 0;
  jlong bitmaps_end = 0;
  jlong arenas_end = 0;
  if (ctor_stats) {
    ctor_start = os::elapsed_counter();
  }
#endif
  os::commit_memory_or_exit((char*)_start, HGCRegionSize, false, "HGC region");
#ifdef ASSERT
  if (ctor_stats) {
    commit_end = os::elapsed_counter();
  }
#endif

  const size_t num_of_arenas = 1 << _k;
  const size_t arena_size_in_words = (HGCRegionSize >> _k) >> LogHeapWordSize;

  const BitMap::idx_t arena_bits = HGCRegionSize >> (LogMinObjAlignmentInBytes + _k);
  const BitMap::idx_t pending_bits = MAX2((BitMap::idx_t)1, arena_bits / BitsPerWord);
  const size_t arena_bitmap_words = BitMap::calc_size_in_words(arena_bits);
  const size_t pending_bitmap_words = BitMap::calc_size_in_words(pending_bits);
  const size_t block_words = 3 * arena_bitmap_words + pending_bitmap_words;
  const size_t total_words = num_of_arenas * block_words;
  _bitmap_storage = NEW_C_HEAP_ARRAY(BitMap::bm_word_t, total_words, mtGC);
  memset(_bitmap_storage, 0, total_words * BytesPerWord);
#ifdef ASSERT
  if (ctor_stats) {
    bitmaps_end = os::elapsed_counter();
  }
#endif

  for (size_t i = 0; i < HGCArena::count_of_categors_by_k(k); ++i)
    *free_arena_head(i) = nullptr;

  for (size_t i = 0; i < num_of_arenas; ++i) {
    HeapWord* arena_start = _start + i * arena_size_in_words;
    BitMap::bm_word_t* bitmap_storage = _bitmap_storage + i * block_words;
    HGCArena* arena = ::new (arena_at(i)) HGCArena(
        _k, arena_start, this, bitmap_storage, arena_bits, pending_bits,
        HGCArena::ctor_timing_start());
    push_free_arena(count_of_categories() - 1, arena);
  }
#ifdef ASSERT
  if (ctor_stats) {
    arenas_end = os::elapsed_counter();
  }
#endif
  log_trace(gc, heap)("HGC region " PTR_FORMAT ": created k=%zu, arenas=%zu x %zu bytes",
                      p2i(_start), _k, num_of_arenas, HGCRegionSize >> _k);
#ifdef ASSERT
  if (ctor_stats) {
    const jlong now = os::elapsed_counter();
    hgc_region_ctor_add(HGC_REGION_CTOR_COMMIT, commit_end - ctor_start);
    hgc_region_ctor_add(HGC_REGION_CTOR_BITMAPS, bitmaps_end - commit_end);
    hgc_region_ctor_add(HGC_REGION_CTOR_ARENAS, arenas_end - bitmaps_end);
    hgc_region_ctor_add(HGC_REGION_CTOR_OTHER, now - arenas_end);
    Atomic::add(&hgc_region_ctor_count, size_t(1));
  }
#endif
}

HGCRegion::~HGCRegion() {
  if (!is_large()) {
    for (size_t i = 0; i < (size_t(1) << _k); ++i) {
      arena_at(i)->~HGCArena();
    }
  }
  if (_bitmap_storage != nullptr) {
    FREE_C_HEAP_ARRAY(mtGC, _bitmap_storage);
  }
  delete _lock;
}

HeapWord* HGCRegion::start() const {
  return _start;
}

void HGCRegion::push_free_arena(size_t arena_category, HGCArena* arena) {
  MutexLocker ml(_lock, Mutex::_no_safepoint_check_flag);
  arena->next = *free_arena_head(arena_category);
  set_free_arena_head(arena_category, arena);
  _non_empty_arenas |= HGCArena::bit_of(arena_category);
  log_trace(gc, alloc)("HGC region " PTR_FORMAT ": push arena " PTR_FORMAT " category=%zu",
                       p2i(_start), p2i(arena->start()), arena_category);
}

HGCArena* HGCRegion::pop_arena_at_least(size_t request_category) {
  assert(request_category < 64, "category out of range");
  MutexLocker ml(_lock, Mutex::_no_safepoint_check_flag);

  u8 available = _non_empty_arenas & (u8(-1) >> request_category);
  if (available == 0) {
    return nullptr;
  }

  size_t arena_category = count_leading_zeros(available);
  HGCArena* arena = *free_arena_head(arena_category);
  assert(arena != nullptr, "bitmap points at an empty free-arena list");
  set_free_arena_head(arena_category, arena->next);
  if (arena->next == nullptr) {
    _non_empty_arenas &= ~HGCArena::bit_of(arena_category);
  }

  log_trace(gc, alloc)("HGC region " PTR_FORMAT ": pop arena " PTR_FORMAT " category=%zu",
                       p2i(_start), p2i(arena->start()), arena_category);
  return arena;
}

void HGCRegion::return_arena(HGCArena* arena) {
  assert(arena != nullptr, "arena must not be null");
  HGCArena::Alignments largest_span_size = arena->find_largest_span().size_of_span;
  if (!largest_span_size) return;
  size_t category = HGCArena::category_of_span_by_size(largest_span_size);
  log_trace(gc, alloc)("HGC region " PTR_FORMAT ": return arena " PTR_FORMAT ", category=%zu",
                       p2i(_start), p2i(arena->start()), category);
  push_free_arena(category, arena);
}

size_t HGCRegion::k() const {
  return _k;
}

size_t HGCRegion::evristik_k() const {
  return _evristik_k;
}

void HGCRegion::set_evristik_k(size_t k) {
  _evristik_k = k;
}

bool HGCRegion::claim_for_cset() {
  u1 expected = static_cast<u1>(Surv);
  u1* state = reinterpret_cast<u1*>(&_state);
  return Atomic::cmpxchg(state, expected, static_cast<u1>(Full)) == expected;
}

size_t HGCRegion::used() const {
  if (is_large()) {
    return is_large_owner() ? _large_object_bytes : 0;
  }
  size_t result = 0;
  for (size_t i = 0; i < (size_t(1) << _k); ++i) {
    const HGCArena* arena =
        reinterpret_cast<const HGCArena*>(_arenas + i * HGCArena::sizeof_me(_k));
    result += arena->memory_used();
  }
  return result;
}

void HGCRegion::clear_mark_bits() {
  if (is_large()) {
    return;
  }

  for (size_t i = 0; i < (size_t(1) << _k); ++i) {
    arena_at(i)->clear_mark_bits();
  }
}

bool HGCRegion::is_empty() {
  assert(!is_large(), "large regions have no arenas");
  return used() == 0;
}

size_t HGCRegion::tlab_size() const {
  return Atomic::load(&_tlab_size);
}

void HGCRegion::add_tlab_size(size_t size) {
  Atomic::add(&_tlab_size, size);
}

void HGCRegion::sub_tlab_size(size_t size) {
  Atomic::sub(&_tlab_size, size);
}

#ifdef ASSERT
void HGCRegion::print_constructor_stats(const char* phase) {
  if (!hgc_region_ctor_stats_enabled() || hgc_region_ctor_count == 0) {
    return;
  }
  const size_t n = hgc_region_ctor_count;
  const jlong commit = hgc_region_ctor_ns[HGC_REGION_CTOR_COMMIT];
  const jlong bitmaps = hgc_region_ctor_ns[HGC_REGION_CTOR_BITMAPS];
  const jlong arenas = hgc_region_ctor_ns[HGC_REGION_CTOR_ARENAS];
  const jlong other = hgc_region_ctor_ns[HGC_REGION_CTOR_OTHER];
  const jlong total = commit + bitmaps + arenas + other;
  tty->print_cr("HGC REGION CTOR TIMES (%s): n=%zu "
                "total avg=%.1fns; commit avg=%.1fns; bitmaps avg=%.1fns; "
                "arenas avg=%.1fns; other avg=%.1fns",
                phase, n,
                double(total) / double(n),
                double(commit) / double(n),
                double(bitmaps) / double(n),
                double(arenas) / double(n),
                double(other) / double(n));
}
#endif

void HGCRegion::rebuild_arenas() {
  assert(_state == Reclaiming, "rebuilding a region that is not collecting: state=%d", (int)_state);
  for (size_t i = 0; i < count_of_categories(); ++i) {
    set_free_arena_head(i, nullptr);
  }
  _non_empty_arenas = 0;
  for (size_t i = 0; i < (size_t(1) << _k); ++i) {
    HGCArena* arena = arena_at(i);
    arena->rebuild();
    return_arena(arena);
  }
  set_evristik_k(_k);

  switch (_state) {
    case Reclaiming:
      ++_age;
      set_state(Surv);
      break;
    case New:
    case Full:
    case Surv:
    case Old:
    case Compacted:
      ShouldNotReachHere();
  }
}

void HGCRegion::prepare_for_mark() {
  if (is_large()) {
    if (is_large_owner()) {
      clear_large_mark();
    }
    return;
  }
  for (size_t i = 0; i < (size_t(1) << _k); ++i) {
    arena_at(i)->prepare_for_mark();
  }
}

static void hgc_region_force_card(CardTable::CardValue* card, CardTable::CardValue to) {
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

static void hgc_region_translate_dirty_card(CardTable::CardValue* card,
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

static void hgc_region_translate_card(CardTable::CardValue* card, bool fully_free,
                                      bool partially_free, bool enqueue) {
  if (fully_free) {
    hgc_region_force_card(card, HGCCardTable::young_card_val());
    return;
  }
  if (partially_free) {

    if (enqueue) {
      hgc_region_translate_dirty_card(card,
                                      HGCCardTable::mixed_dirty_card_val());
    } else {
      hgc_region_force_card(card, HGCCardTable::mixed_clean_card_val());
    }
    return;
  }

  if (enqueue) {
    hgc_region_translate_dirty_card(card, HGCCardTable::dirty_card_val());
  } else {
    hgc_region_force_card(card, HGCCardTable::clean_card_val());
  }
}

void HGCRegion::translate_cards_after_rebuild(HGCCardTable* ct, bool enqueue) {
  if (is_large()) {
    if (!is_large_owner() || !is_large_marked()) {
      return;
    }
    const size_t run_bytes = _large_regions * HGCRegionSize;
    HeapWord* run_end = _start + (run_bytes >> LogHeapWordSize);
    HeapWord* obj_end = _start + (_large_object_bytes >> LogHeapWordSize);
    for (CardTable::CardValue* c = ct->byte_for(_start); c < ct->byte_after(run_end - 1); ++c) {
      HeapWord* card_start = ct->addr_for(c);
      HeapWord* card_end = MIN2(card_start + HGCCardTable::card_size_in_words(), run_end);
      bool occupied = card_start < obj_end;
      bool fully = occupied && (card_end <= obj_end);
      bool partially = occupied && !fully;
      bool fully_free = !occupied;
      hgc_region_translate_card(c, fully_free, partially, enqueue);
      (void)fully;
    }
    return;
  }
  for (size_t i = 0; i < (size_t(1) << _k); ++i) {
    arena_at(i)->translate_cards_after_rebuild(ct, enqueue);
  }
}

void HGCRegion::invalidate_start_bitset() {
  if (is_large()) {
    return;
  }
  for (size_t i = 0; i < (size_t(1) << _k); ++i) {
    arena_at(i)->invalidate_start_bitset();
  }
}

void HGCRegion::prepare_cards_for_allocation(HGCCardTable* ct) {
  if (is_large_continuation()) {
    return;
  }

  const bool join_sts = HGCCollectedHeap::heap()->is_concurrent_marking();
  SuspendibleThreadSetTryJoiner sts_join(join_sts);
  size_t bytes = is_large_owner() ? _large_regions * HGCRegionSize : HGCRegionSize;
  HeapWord* end = _start + (bytes >> LogHeapWordSize);
  for (CardTable::CardValue* c = ct->byte_for(_start); c < ct->byte_after(end - 1); ++c) {
    for (;;) {
      CardTable::CardValue value = Atomic::load(c);
      if (value == HGCCardTable::mixed_dirty_card_val()) {

        if (Atomic::cmpxchg(c, value, HGCCardTable::mixed_clean_card_val()) != value) {
          continue;
        }
        HGCCollectedHeap::heap()->rem_set()->refine_card_concurrently(c, 0);

        if (Atomic::cmpxchg(c, HGCCardTable::mixed_clean_card_val(),
                            HGCCardTable::mixed_young_card_val()) ==
            HGCCardTable::mixed_clean_card_val()) {
          break;
        }
        continue;
      }
      if (value != HGCCardTable::mixed_clean_card_val()) {
        break;
      }

      HGCCollectedHeap::heap()->rem_set()->refine_card_concurrently(c, 0);
      if (Atomic::cmpxchg(c, HGCCardTable::mixed_clean_card_val(),
                          HGCCardTable::mixed_young_card_val()) ==
          HGCCardTable::mixed_clean_card_val()) {
        break;
      }
    }
  }
  if (is_large()) {
    if (is_large_owner()) {
      clear_large_mark();
    }
  } else {
    for (size_t i = 0; i < (size_t(1) << _k); ++i) {
      arena_at(i)->clear_mark_bits();
    }
  }
}

void HGCRegion::print_map_on(outputStream* st, size_t index) {
  if (is_large()) {
    st->print_cr("  {");
    st->print_cr("    \"index\": %zu,", index);
    st->print_cr("    \"start\": \"" PTR_FORMAT "\",", p2i(_start));
    st->print_cr("    \"large\": true,");
    st->print_cr("    \"owner\": \"%s\",", is_large_owner() ? "self" : "continuation");
    st->print_cr("    \"owner_start\": \"" PTR_FORMAT "\",", p2i(_large_owner->start()));
    st->print_cr("    \"regions\": %zu,", _large_regions);
    st->print_cr("    \"used\": %zu,", used());
    st->print_cr("    \"marked\": %s", is_large_owner() && is_large_marked() ? "true" : "false");
    st->print_cr("  }");
    return;
  }
  st->print_cr("  {");
  st->print_cr("    \"index\": %zu,", index);
  st->print_cr("    \"start\": \"" PTR_FORMAT "\",", p2i(_start));
  st->print_cr("    \"k\": %zu,", _k);
  st->print_cr("    \"evristik_k\": %zu,", _evristik_k);
  st->print_cr("    \"in_cset\": %s,", in_cset() ? "true" : "false");
  st->print_cr("    \"used\": %zu,", used());
  st->print_cr("    \"arenas\": [");
  size_t num_of_arenas = size_t(1) << _k;
  for (size_t i = 0; i < num_of_arenas; ++i) {
    arena_at(i)->print_map_on(st);
    if (i + 1 < num_of_arenas) {
      st->print_cr(",");
    }
  }
  st->print_cr("    ]");
  st->print_cr("  }");
}

HGCArena** HGCRegion::free_arena_head(size_t arena_category) {
  return reinterpret_cast<HGCArena**>(_arenas + (1 << _k) * HGCArena::sizeof_me(_k) +
      arena_category * sizeof(HGCArena*));
}

void HGCRegion::set_free_arena_head(size_t arena_category, HGCArena* arena) {
  *free_arena_head(arena_category) = arena;
}

HGCArena* HGCRegion::arena_at(size_t i) {
  return reinterpret_cast<HGCArena*>(_arenas + i * HGCArena::sizeof_me(_k));
}

HGCArena* HGCRegion::arena(HeapWord* ptr) {
  if (is_large()) {
    return nullptr;
  }
  if (ptr < _start || ptr >= _start + (HGCRegionSize >> LogHeapWordSize)) {
    return nullptr;
  }
  size_t arena_size_in_words = (HGCRegionSize >> _k) >> LogHeapWordSize;
  return arena_at(static_cast<size_t>(ptr - _start) / arena_size_in_words);
}

HGCArena* HGCRegion::acquire_arena_for(HGCArena::Alignments size_in_alignments) {
  size_t request_category = HGCArena::category_of_request_by_size(size_in_alignments);

  log_trace(gc, alloc)("HGC region " PTR_FORMAT ": acquire arena, size=%zu alignments, category=%zu",
                       p2i(_start), size_in_alignments.value(), request_category);

  HGCArena* arena = pop_arena_at_least(request_category);
  if (arena != nullptr) {
    log_trace(gc, alloc)("HGC region " PTR_FORMAT ": acquired arena " PTR_FORMAT " for size=%zu alignments",
                         p2i(_start), p2i(arena->start()), size_in_alignments.value());
    return arena;
  }

  log_trace(gc, alloc)("HGC region " PTR_FORMAT ": no arena for size=%zu alignments",
                       p2i(_start), size_in_alignments.value());
  return nullptr;
}

static bool hgc_region_ctor_stats_enabled() {
  static int value = -1;
  if (value < 0) {
    value = (::getenv("HGC_ALLOC_STATS") == nullptr) ? 0 : 1;
  }
  return value == 1;
}

static void hgc_region_ctor_add(HGCRegionCtorPart part, jlong ns) {
  Atomic::add(&hgc_region_ctor_ns[part], ns);
}
