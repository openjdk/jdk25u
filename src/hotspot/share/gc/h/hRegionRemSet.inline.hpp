#ifndef SHARE_GC_H_HREGIONREMSET_INLINE_HPP
#define SHARE_GC_H_HREGIONREMSET_INLINE_HPP

#include "gc/h/hRegionRemSet.hpp"

#include "gc/h/hCardSet.inline.hpp"
#include "gc/h/hCardTable.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"

inline void HGCHeapRegionRemSet::lock() {
  while (Atomic::cmpxchg(&_lock, 0u, 1u) != 0) {
    os::naked_yield();
  }
}

inline void HGCHeapRegionRemSet::unlock() {
  Atomic::release_store(&_lock, 0u);
}

void HGCHeapRegionRemSet::set_state_untracked() {
  guarantee(SafepointSynchronize::is_at_safepoint() || !is_tracked(),
            "Should only set to Untracked during safepoint but is %s.", get_state_str());
  if (_state == Untracked) {
    return;
  }
  clear_fcc();
  _state = Untracked;
}

void HGCHeapRegionRemSet::set_state_updating() {
  guarantee(SafepointSynchronize::is_at_safepoint() && !is_tracked(),
            "Should only set to Updating from Untracked during safepoint but is %s", get_state_str());
  clear_fcc();
  _state = Updating;
}

void HGCHeapRegionRemSet::set_state_complete() {
  clear_fcc();
  _state = Complete;
}

template <typename Closure>
class HGCContainerCardsOrRanges {
  Closure& _cl;
  uint _region_idx;
  uint _offset;

public:
  HGCContainerCardsOrRanges(Closure& cl, uint region_idx, uint offset) : _cl(cl), _region_idx(region_idx), _offset(offset) { }

  bool start_iterate(uint tag) {
    return _cl.start_iterate(tag, _region_idx);
  }

  void operator()(uint card_idx) {
    _cl.do_card(card_idx + _offset);
  }

  void operator()(uint card_idx, uint length) {
    _cl.do_card_range(card_idx + _offset, length);
  }
};

template <typename Closure, template <typename> class CardOrRanges>
class HGCHeapRegionRemSetMergeCardClosure : public HGCCardSet::ContainerPtrClosure {
  HGCCardSet* _card_set;
  Closure& _cl;
  uint _log_card_regions_per_region;
  uint _card_regions_per_region_mask;
  uint _log_card_region_size;

public:

  HGCHeapRegionRemSetMergeCardClosure(HGCCardSet* card_set,
                            Closure& cl,
                            uint log_card_regions_per_region,
                            uint log_card_region_size) :
    _card_set(card_set),
    _cl(cl),
    _log_card_regions_per_region(log_card_regions_per_region),
    _card_regions_per_region_mask((1 << log_card_regions_per_region) - 1),
    _log_card_region_size(log_card_region_size) {
  }

  void do_containerptr(uint card_region_idx, size_t num_occupied, HGCCardSet::ContainerPtr container) override {
    CardOrRanges<Closure> cl(_cl,
                             card_region_idx >> _log_card_regions_per_region,
                             (card_region_idx & _card_regions_per_region_mask) << _log_card_region_size);
    _card_set->iterate_cards_or_ranges_in_container(container, cl);
  }
};

template <class CardOrRangeVisitor>
inline void HGCHeapRegionRemSet::iterate_for_merge(CardOrRangeVisitor& cl, bool at_safepoint) {
  iterate_for_merge(card_set(), cl, at_safepoint);
}

template <class CardOrRangeVisitor>
void HGCHeapRegionRemSet::iterate_for_merge(HGCCardSet* card_set, CardOrRangeVisitor& cl,
                                            bool at_safepoint) {
  HGCHeapRegionRemSetMergeCardClosure<CardOrRangeVisitor, HGCContainerCardsOrRanges> cl2(card_set,
                                                                              cl,
                                                                              card_set->config()->log2_card_regions_per_heap_region(),
                                                                              card_set->config()->log2_cards_per_card_region());
  card_set->iterate_containers(&cl2, at_safepoint);
}

uintptr_t HGCHeapRegionRemSet::to_card(OopOrNarrowOopStar from) const {
  return pointer_delta(from, _heap_base_address, 1) >> HGCCardTable::card_shift();
}

void HGCHeapRegionRemSet::add_reference(OopOrNarrowOopStar from, uint tid) {
  assert(has_card_set(), "pre-condition");

  assert(_state != Untracked, "must be");

  struct Locker {
    HGCHeapRegionRemSet* rs;
    explicit Locker(HGCHeapRegionRemSet* r) : rs(r) { rs->lock(); }
    ~Locker() { rs->unlock(); }
  } locker(this);

  uint cur_idx = region_index();
  uintptr_t from_card = uintptr_t(from) >> HGCCardTable::card_shift();

  Atomic::inc(&hgc_add_ref_calls);
  uintptr_t card = to_card(from);
  if (HGCFromCardCache::contains(tid, cur_idx, from_card)) {
    if (card_set()->contains_card(card)) {
      Atomic::inc(&hgc_add_ref_cache_hits);
      return;
    }

  }

  card_set()->add_card(card);
  for (int attempt = 0; attempt < 4 && !card_set()->contains_card(card); ++attempt) {
    card_set()->add_card(card);
  }
  assert(card_set()->contains_card(card),
         "from-card " PTR_FORMAT " for target region %u is still missing after retries",
         p2i(from), cur_idx);
  if (card_set()->contains_card(card)) {
    HGCFromCardCache::set(tid, cur_idx, from_card);
  }
  hgc_diag_ring_push(hgc_cards_added, &hgc_cards_added_idx,
                     (uintptr_t)HGCCollectedHeap::heap()->card_table()->byte_for(from));
}

bool HGCHeapRegionRemSet::contains_reference(OopOrNarrowOopStar from) {
  return card_set()->contains_card(to_card(from));
}

void HGCHeapRegionRemSet::print_info(outputStream* st, OopOrNarrowOopStar from) {
  card_set()->print_info(st, to_card(from));
}

#endif
