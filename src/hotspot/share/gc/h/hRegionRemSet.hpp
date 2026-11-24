#ifndef SHARE_GC_H_HREGIONREMSET_HPP
#define SHARE_GC_H_HREGIONREMSET_HPP

#include "gc/h/hCardSet.hpp"
#include "gc/h/hCardSetMemory.hpp"
#include "gc/h/hFromCardCache.hpp"
#include "memory/allocation.hpp"

class outputStream;
class HGCRegion;

extern volatile size_t hgc_refine_cards;
extern volatile size_t hgc_refine_oops;
extern volatile size_t hgc_refine_same;
extern volatile size_t hgc_refine_added;
extern volatile size_t hgc_add_ref_calls;
extern volatile size_t hgc_add_ref_cache_hits;
extern volatile size_t hgc_probe_hits;
extern const void* hgc_probe_field;

extern volatile uintptr_t hgc_cards_enqueued[];
extern volatile size_t hgc_cards_enqueued_idx;
extern volatile uintptr_t hgc_cards_cleaned[];
extern volatile size_t hgc_cards_cleaned_idx;
extern volatile uintptr_t hgc_cards_added[];
extern volatile size_t hgc_cards_added_idx;
extern volatile uintptr_t hgc_cards_merged[];
extern volatile size_t hgc_cards_merged_idx;
size_t hgc_diag_ring_count(volatile uintptr_t* ring, volatile size_t* idx, uintptr_t value);
void hgc_diag_ring_push(volatile uintptr_t* ring, volatile size_t* idx, uintptr_t value);

enum {
  HGC_CARD_WRITER_SLOW = 1,
  HGC_CARD_WRITER_RUNTIME = 2,
  HGC_CARD_WRITER_REDIRTY = 3,
  HGC_CARD_WRITER_CLEAN = 4,
  HGC_CARD_WRITER_CLEAN_NEW = 5,
  HGC_CARD_WRITER_CLEAN_YOUNG = 6,
  HGC_CARD_WRITER_DIRTY_YOUNG = 7,
  HGC_CARD_WRITER_SHARED = 8,
  HGC_CARD_WRITER_MIXED_YOUNG = 11
};
extern u1 hgc_card_writer[];
int hgc_card_writer_site(const void* card);

class HGCHeapRegionRemSet : public CHeapObj<mtGC> {
  friend class VMStructs;
  HGCCardSet* _card_set;

  HGCRegion* _hr;

  static HeapWord* _heap_base_address;

  void clear_fcc();

public:
  HGCHeapRegionRemSet(HGCRegion* hr);
  ~HGCHeapRegionRemSet();

  bool cardset_is_empty() const {
    return _card_set == nullptr || card_set()->is_empty();
  }

  void install_card_set(HGCCardSet* card_set) {
    assert(card_set != nullptr, "pre-condition");
    assert(_card_set == nullptr, "pre-condition");

    _card_set = card_set;
  }

  void uninstall_card_set();

  bool has_card_set() const {
    return _card_set != nullptr;
  }

  HGCCardSet* card_set() {
    assert(has_card_set(), "pre-condition");
    return _card_set;
  }

  const HGCCardSet* card_set() const {
    assert(has_card_set(), "pre-condition");
    return _card_set;
  }

  bool is_empty() const {
    return cardset_is_empty();
  }

  bool occupancy_less_or_equal_than(size_t occ) const {
    return card_set()->occupancy_less_or_equal_to(occ);
  }

  template <class CardOrRangeVisitor>
  inline void iterate_for_merge(CardOrRangeVisitor& cl, bool at_safepoint = true);

  template <class CardOrRangeVisitor>
  inline static void iterate_for_merge(HGCCardSet* card_set, CardOrRangeVisitor& cl,
                                       bool at_safepoint = true);

  size_t occupied() {
    assert(has_card_set(), "pre-condition");
    return card_set()->occupied();
  }

  static void initialize(MemRegion reserved);

  static HGCCardSetCoarsenStats coarsen_stats() { return HGCCardSet::coarsen_stats(); }

  inline uintptr_t to_card(OopOrNarrowOopStar from) const;

private:
  uint region_index() const;

  enum RemSetState : u1 {
    Untracked,
    Updating,
    Complete
  };

  RemSetState _state;
  volatile uint _lock;

  static const char* _state_strings[];
  static const char* _short_state_strings[];
public:
  const char* get_state_str() const { return _state_strings[_state]; }
  const char* get_short_state_str() const { return _short_state_strings[_state]; }

  bool is_tracked() { return _state != Untracked; }
  bool is_updating() { return _state == Updating; }
  bool is_complete() { return _state == Complete; }

  void lock();
  void unlock();

  inline void set_state_untracked();
  inline void set_state_updating();
  inline void set_state_complete();

  inline void add_reference(OopOrNarrowOopStar from, uint tid);

  void clear(bool only_cardset = false, bool keep_tracked = false);

  void reset_table_scanner();

  HGCMonotonicArenaMemoryStats card_set_memory_stats() const;

  size_t mem_size() {
    return sizeof(HGCHeapRegionRemSet);
  }

  static size_t static_mem_size() {
    return HGCCardSet::static_mem_size();
  }

  static void print_static_mem_size(outputStream* out);

  inline bool contains_reference(OopOrNarrowOopStar from);

  inline void print_info(outputStream* st, OopOrNarrowOopStar from);

  static void invalidate_from_card_cache(uint start_idx, size_t num_regions) {
    HGCFromCardCache::invalidate(start_idx, num_regions);
  }

#ifndef PRODUCT
  static void print_from_card_cache() {
    HGCFromCardCache::print();
  }

  static void test();
#endif
};

#endif
