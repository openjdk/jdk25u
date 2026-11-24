#include "gc/h/hRemSet.hpp"

#include "gc/h/hArena.inline.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hFromCardCache.hpp"
#include "gc/h/hOopClosures.inline.hpp"
#include "gc/h/hRegion.hpp"
#include "runtime/globals.hpp"

HGCRemSet::HGCRemSet(HGCCollectedHeap* heap, HGCCardTable* ct) :
  _heap(heap),
  _ct(ct) {
}

HGCRemSet::~HGCRemSet() {
}

void HGCRemSet::initialize(uint max_reserved_regions) {
  HGCFromCardCache::initialize(max_reserved_regions);
}

bool HGCRemSet::clean_card_before_refine(CardValue** const card_ptr_addr) {
  CardValue* card_ptr = *card_ptr_addr;
  HeapWord* start = _ct->addr_for(card_ptr);
  HGCRegion* r = _heap->region(start);

  if (r == nullptr) {
    return false;
  }

  CardTable::CardValue value = *card_ptr;
  CardTable::CardValue clean_value;
  if (value == HGCCardTable::dirty_card_val()) {
    clean_value = HGCCardTable::clean_card_val();
  } else if (value == HGCCardTable::mixed_dirty_card_val()) {
    clean_value = HGCCardTable::mixed_clean_card_val();
  } else {
    return false;
  }

  if (Atomic::cmpxchg(card_ptr, value, clean_value) != value) {
    return false;
  }

  hgc_card_writer[((uintptr_t)card_ptr) & ((1 << 20) - 1)] = HGC_CARD_WRITER_CLEAN;
  hgc_diag_ring_push(hgc_cards_cleaned, &hgc_cards_cleaned_idx, (uintptr_t)card_ptr);

  return true;
}

volatile size_t hgc_refine_cards = 0;
volatile size_t hgc_refine_oops = 0;
volatile size_t hgc_refine_same = 0;
volatile size_t hgc_refine_added = 0;
volatile size_t hgc_add_ref_calls = 0;
volatile size_t hgc_add_ref_cache_hits = 0;
volatile size_t hgc_probe_hits = 0;
const void* hgc_probe_field = nullptr;

enum { HGC_DIAG_CARDS = 1 << 16 };
volatile uintptr_t hgc_cards_enqueued[HGC_DIAG_CARDS];
volatile size_t hgc_cards_enqueued_idx = 0;
volatile uintptr_t hgc_cards_cleaned[HGC_DIAG_CARDS];
volatile size_t hgc_cards_cleaned_idx = 0;
volatile uintptr_t hgc_cards_added[HGC_DIAG_CARDS];
volatile size_t hgc_cards_added_idx = 0;
volatile uintptr_t hgc_cards_merged[HGC_DIAG_CARDS];
volatile size_t hgc_cards_merged_idx = 0;

enum { HGC_CARD_WRITER_SLOTS = 1 << 20 };
u1 hgc_card_writer[HGC_CARD_WRITER_SLOTS];

static inline size_t hgc_card_writer_index(const void* card) {
  return ((uintptr_t)card) & (HGC_CARD_WRITER_SLOTS - 1);
}

int hgc_card_writer_site(const void* card) {
  return (int)hgc_card_writer[hgc_card_writer_index(card)];
}

void hgc_diag_ring_push(volatile uintptr_t* ring, volatile size_t* idx, uintptr_t value) {
  size_t slot = Atomic::fetch_then_add(idx, size_t(1));
  ring[slot & (HGC_DIAG_CARDS - 1)] = value;
}

size_t hgc_diag_ring_count(volatile uintptr_t* ring, volatile size_t* idx, uintptr_t value) {
  size_t total = MIN2((size_t)Atomic::load(idx), (size_t)HGC_DIAG_CARDS);
  size_t count = 0;
  for (size_t i = 0; i < total; ++i) {
    if (Atomic::load(&ring[i]) == value) {
      ++count;
    }
  }
  return count;
}

void HGCRemSet::refine_card_concurrently(CardValue* const card_ptr, const uint worker_id) {
  Atomic::inc(&hgc_refine_cards);
  HeapWord* start = _ct->addr_for(card_ptr);
  HGCRegion* r = _heap->region(start);
  if (r == nullptr) {
    return;
  }
  HeapWord* region_end = r->start() + (HGCRegionSize >> LogHeapWordSize);
  HeapWord* end = MIN2(start + HGCCardTable::card_size_in_words(), region_end);

  HGCConcurrentRefineOopClosure conc_refine_cl(_heap, worker_id);
  if (r->is_large()) {

    oop obj = cast_to_oop(r->large_owner()->start());
    obj->oop_iterate(&conc_refine_cl, MemRegion(start, end));
    return;
  }
  r->arena(start)->oops_on_memregion(start, end, &conc_refine_cl);
}
