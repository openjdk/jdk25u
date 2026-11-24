#ifndef SHARE_GC_H_HCARDSETCONTAINERS_INLINE_HPP
#define SHARE_GC_H_HCARDSETCONTAINERS_INLINE_HPP

#include "gc/h/hCardSetContainers.hpp"

#include "utilities/bitMap.inline.hpp"
#include "utilities/checkedCast.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/spinYield.hpp"

inline HGCCardSetInlinePtr::ContainerPtr HGCCardSetInlinePtr::merge(ContainerPtr orig_value, uint card_in_region, uint idx, uint bits_per_card) {
  assert((idx & (SizeFieldMask >> SizeFieldPos)) == idx, "Index %u too large to fit into size field", idx);
  assert(card_in_region < ((uint)1 << bits_per_card), "Card %u too large to fit into card value field", card_in_region);

  uint card_pos = card_pos_for(idx, bits_per_card);
  assert(card_pos + bits_per_card < BitsInValue, "Putting card at pos %u with %u bits would extend beyond pointer", card_pos, bits_per_card);

  uintptr_t mask = ((((uintptr_t)1 << bits_per_card) - 1) << card_pos);
  assert(((uintptr_t)orig_value & mask) == 0, "The bits in the new range should be empty; orig_value " PTR_FORMAT " mask " PTR_FORMAT, p2i(orig_value), mask);

  uintptr_t value = ((uintptr_t)(idx + 1) << SizeFieldPos) | ((uintptr_t)card_in_region << card_pos);
  uintptr_t res = (((uintptr_t)orig_value & ~SizeFieldMask) | value);
  return (ContainerPtr)res;
}

inline HGCAddCardResult HGCCardSetInlinePtr::add(uint card_idx, uint bits_per_card, uint max_cards_in_inline_ptr) {
  assert(_value_addr != nullptr, "No value address available, cannot add to set.");

  uint cur_idx = 0;
  while (true) {
    uint num_cards = num_cards_in(_value);
    if (num_cards > 0) {
      cur_idx = find(card_idx, bits_per_card, cur_idx, num_cards);
    }
    if (cur_idx < num_cards) {
      return Found;
    }
    if (num_cards >= max_cards_in_inline_ptr) {
      return Overflow;
    }
    ContainerPtr new_value = merge(_value, card_idx, num_cards, bits_per_card);
    ContainerPtr old_value = Atomic::cmpxchg(_value_addr, _value, new_value, memory_order_relaxed);
    if (_value == old_value) {
      return Added;
    }
    _value = old_value;
    if (HGCCardSet::container_type(_value) != HGCCardSet::ContainerInlinePtr) {
      return Overflow;
    }
  }
}

inline uint HGCCardSetInlinePtr::find(uint card_idx, uint bits_per_card, uint start_at, uint num_cards) {
  assert(start_at < num_cards, "Precondition!");

  uintptr_t const card_mask = (1 << bits_per_card) - 1;
  uintptr_t value = ((uintptr_t)_value) >> card_pos_for(start_at, bits_per_card);

  for (uint cur_idx = start_at; cur_idx < num_cards; cur_idx++) {
    if ((value & card_mask) == card_idx) {
      return cur_idx;
    }
    value >>= bits_per_card;
  }
  return num_cards;
}

inline bool HGCCardSetInlinePtr::contains(uint card_idx, uint bits_per_card) {
  uint num_cards = num_cards_in(_value);
  if (num_cards == 0) {
    return false;
  }
  uint cur_idx = find(card_idx, bits_per_card, 0, num_cards);
  return cur_idx < num_cards;
}

template <class CardVisitor>
inline void HGCCardSetInlinePtr::iterate(CardVisitor& found, uint bits_per_card) {
  uint const num_cards = num_cards_in(_value);
  uintptr_t const card_mask = (1 << bits_per_card) - 1;

  uintptr_t value = ((uintptr_t)_value) >> card_pos_for(0, bits_per_card);
  for (uint cur_idx = 0; cur_idx < num_cards; cur_idx++) {
    found(value & card_mask);
    value >>= bits_per_card;
  }
}

inline bool HGCCardSetContainer::try_increment_refcount() {
  uintptr_t old_value = refcount();
  while (true) {
    if (old_value < 3 || (old_value & 0x1) == 0) {
      return false;
    }

    uintptr_t new_value = old_value + 2;
    uintptr_t ref_count = Atomic::cmpxchg(&_ref_count, old_value, new_value);
    if (ref_count == old_value) {
      return true;
    }
    old_value = ref_count;
  }
}

inline uintptr_t HGCCardSetContainer::decrement_refcount() {
  uintptr_t old_value = refcount();
  assert((old_value & 0x1) != 0 && old_value >= 3, "precondition");
  return Atomic::sub(&_ref_count, 2u);
}

inline HGCCardSetArray::HGCCardSetArray(uint card_in_region, EntryCountType num_cards) :
  HGCCardSetContainer(),
  _size(num_cards),
  _num_entries(1) {
  assert(_size > 0, "CardSetArray of size 0 not supported.");
  assert(_size < LockBitMask, "Only support CardSetArray of size %u or smaller.", LockBitMask - 1);
  *entry_addr(0) = checked_cast<EntryDataType>(card_in_region);
}

inline HGCCardSetArray::HGCCardSetArrayLocker::HGCCardSetArrayLocker(EntryCountType volatile* num_entries_addr) :
  _num_entries_addr(num_entries_addr) {
  SpinYield s;
  EntryCountType num_entries = Atomic::load(_num_entries_addr) & EntryMask;
  while (true) {
    EntryCountType old_value = Atomic::cmpxchg(_num_entries_addr,
                                               num_entries,
                                               (EntryCountType)(num_entries | LockBitMask));
    if (old_value == num_entries) {
      _local_num_entries = num_entries;
      break;
    }
    num_entries = old_value & EntryMask;
    s.wait();
  }
}

inline HGCCardSetArray::EntryDataType const* HGCCardSetArray::base_addr() const {
  const void* ptr = reinterpret_cast<const char*>(this) + header_size_in_bytes();
  return reinterpret_cast<EntryDataType const*>(ptr);
}

inline HGCCardSetArray::EntryDataType const* HGCCardSetArray::entry_addr(EntryCountType index) const {
  assert(index < _num_entries, "precondition");
  return base_addr() + index;
}

inline HGCCardSetArray::EntryDataType* HGCCardSetArray::entry_addr(EntryCountType index) {
  return const_cast<EntryDataType*>(const_cast<const HGCCardSetArray*>(this)->entry_addr(index));
}

inline HGCCardSetArray::EntryDataType HGCCardSetArray::at(EntryCountType index) const {
  return *entry_addr(index);
}

inline HGCAddCardResult HGCCardSetArray::add(uint card_idx) {
  assert(card_idx < (1u << (sizeof(EntryDataType) * BitsPerByte)),
         "Card index %u does not fit allowed card value range.", card_idx);
  EntryCountType num_entries = Atomic::load_acquire(&_num_entries) & EntryMask;
  EntryCountType idx = 0;
  for (; idx < num_entries; idx++) {
    if (at(idx) == card_idx) {
      return Found;
    }
  }

  HGCCardSetArrayLocker x(&_num_entries);

  num_entries = x.num_entries();
  for (; idx < num_entries; idx++) {
    if (at(idx) == card_idx) {
      return Found;
    }
  }

  if (num_entries == _size) {
    return Overflow;
  }

  *entry_addr(num_entries) = checked_cast<EntryDataType>(card_idx);

  x.inc_num_entries();

  return Added;
}

inline bool HGCCardSetArray::contains(uint card_idx) {
  EntryCountType num_entries = Atomic::load_acquire(&_num_entries) & EntryMask;

  for (EntryCountType idx = 0; idx < num_entries; idx++) {
    if (at(idx) == card_idx) {
      return true;
    }
  }
  return false;
}

template <class CardVisitor>
void HGCCardSetArray::iterate(CardVisitor& found) {
  EntryCountType num_entries = Atomic::load_acquire(&_num_entries) & EntryMask;
  for (EntryCountType idx = 0; idx < num_entries; idx++) {
    found(at(idx));
  }
}

inline size_t HGCCardSetArray::header_size_in_bytes() {
  return offset_of(HGCCardSetArray, _data);
}

inline HGCCardSetBitMap::HGCCardSetBitMap(uint card_in_region, uint size_in_bits) :
  HGCCardSetContainer(), _num_bits_set(1) {
  assert(size_in_bits % (sizeof(_bits[0]) * BitsPerByte) == 0,
         "Size %u should be aligned to bitmap word size.", size_in_bits);
  BitMapView bm(_bits, size_in_bits);
  bm.clear();
  bm.set_bit(card_in_region);
}

inline HGCAddCardResult HGCCardSetBitMap::add(uint card_idx, size_t threshold, size_t size_in_bits) {
  BitMapView bm(_bits, size_in_bits);
  if (_num_bits_set >= threshold) {
    return bm.at(card_idx) ? Found : Overflow;
  }
  if (bm.par_set_bit(card_idx)) {
    Atomic::inc(&_num_bits_set, memory_order_relaxed);
    return Added;
  }
  return Found;
}

template <class CardVisitor>
inline void HGCCardSetBitMap::iterate(CardVisitor& found, size_t size_in_bits, uint offset) {
  BitMapView bm(_bits, size_in_bits);
  bm.iterate([&](BitMap::idx_t idx) { found(offset | (uint)idx); });
}

inline size_t HGCCardSetBitMap::header_size_in_bytes() {
  return offset_of(HGCCardSetBitMap, _bits);
}

inline HGCCardSetHowl::ContainerPtr const* HGCCardSetHowl::container_addr(EntryCountType index) const {
  assert(index < _num_entries, "precondition");
  return buckets() + index;
}

inline HGCCardSetHowl::ContainerPtr* HGCCardSetHowl::container_addr(EntryCountType index) {
  return const_cast<ContainerPtr*>(const_cast<const HGCCardSetHowl*>(this)->container_addr(index));
}

inline HGCCardSetHowl::ContainerPtr HGCCardSetHowl::at(EntryCountType index) const {
  return *container_addr(index);
}

inline HGCCardSetHowl::ContainerPtr const* HGCCardSetHowl::buckets() const {
  const void* ptr = reinterpret_cast<const char*>(this) + header_size_in_bytes();
  return reinterpret_cast<ContainerPtr const*>(ptr);
}

inline HGCCardSetHowl::HGCCardSetHowl(EntryCountType card_in_region, HGCCardSetConfiguration* config) :
  HGCCardSetContainer(),
  _num_entries((config->max_cards_in_array() + 1)) {
  EntryCountType num_buckets = config->num_buckets_in_howl();
  EntryCountType bucket = config->howl_bucket_index(card_in_region);
  for (uint i = 0; i < num_buckets; ++i) {
    *container_addr(i) = HGCCardSetInlinePtr();
    if (i == bucket) {
      HGCCardSetInlinePtr value(container_addr(i), at(i));
      value.add(card_in_region, config->inline_ptr_bits_per_card(), config->max_cards_in_inline_ptr());
    }
  }
}

inline bool HGCCardSetHowl::contains(uint card_idx, HGCCardSetConfiguration* config) {
  EntryCountType bucket = config->howl_bucket_index(card_idx);
  ContainerPtr* array_entry = container_addr(bucket);
  ContainerPtr container = Atomic::load_acquire(array_entry);

  switch (HGCCardSet::container_type(container)) {
    case HGCCardSet::ContainerArrayOfCards: {
      return HGCCardSet::container_ptr<HGCCardSetArray>(container)->contains(card_idx);
    }
    case HGCCardSet::ContainerBitMap: {
      uint card_offset = config->howl_bitmap_offset(card_idx);
      return HGCCardSet::container_ptr<HGCCardSetBitMap>(container)->contains(card_offset, config->max_cards_in_howl_bitmap());
    }
    case HGCCardSet::ContainerInlinePtr: {
      HGCCardSetInlinePtr ptr(container);
      return ptr.contains(card_idx, config->inline_ptr_bits_per_card());
    }
    case HGCCardSet::ContainerHowl: {
      assert(container == HGCCardSet::FullCardSet, "Must be");
      return true;
    }
  }
  return false;
}

template <class CardOrRangeVisitor>
inline void HGCCardSetHowl::iterate(CardOrRangeVisitor& found, HGCCardSetConfiguration* config) {
  for (uint i = 0; i < config->num_buckets_in_howl(); ++i) {
    iterate_cardset(at(i), i, found, config);
  }
}

template <class ContainerPtrVisitor>
inline void HGCCardSetHowl::iterate(ContainerPtrVisitor& found, uint num_card_sets) {
  for (uint i = 0; i < num_card_sets; ++i) {
    found(container_addr(i));
  }
}

template <class CardOrRangeVisitor>
inline void HGCCardSetHowl::iterate_cardset(ContainerPtr const container, uint index, CardOrRangeVisitor& found, HGCCardSetConfiguration* config) {
  switch (HGCCardSet::container_type(container)) {
    case HGCCardSet::ContainerInlinePtr: {
      if (found.start_iterate(HowlInline)) {
        HGCCardSetInlinePtr ptr(container);
        ptr.iterate(found, config->inline_ptr_bits_per_card());
      }
      return;
    }
    case HGCCardSet::ContainerArrayOfCards: {
      if (found.start_iterate(HowlArrayOfCards)) {
        HGCCardSet::container_ptr<HGCCardSetArray>(container)->iterate(found);
      }
      return;
    }
    case HGCCardSet::ContainerBitMap: {
      if (found.start_iterate(HowlBitmap)) {
        uint offset = index << config->log2_max_cards_in_howl_bitmap();
        HGCCardSet::container_ptr<HGCCardSetBitMap>(container)->iterate(found, config->max_cards_in_howl_bitmap(), offset);
      }
      return;
    }
    case HGCCardSet::ContainerHowl: {
      assert(container == HGCCardSet::FullCardSet, "Must be");
      if (found.start_iterate(HowlFull)) {
        uint offset = index << config->log2_max_cards_in_howl_bitmap();
        found(offset, config->max_cards_in_howl_bitmap());
      }
      return;
    }
  }
}

inline HGCCardSetHowl::EntryCountType HGCCardSetHowl::num_buckets(size_t size_in_bits, size_t max_cards_in_array, size_t max_num_buckets) {
  size_t size_bitmap_bytes = BitMap::calc_size_in_words(size_in_bits) * BytesPerWord;
  size_t max_size_arrays_bytes = size_bitmap_bytes / 2;
  size_t size_array_bytes = max_cards_in_array * sizeof(HGCCardSetArray::EntryDataType);
  size_t num_arrays = max_size_arrays_bytes / size_array_bytes;
  num_arrays = round_down_power_of_2(MAX2((size_t)1, MIN2(num_arrays, max_num_buckets)));
  return (EntryCountType)num_arrays;
}

inline size_t HGCCardSetHowl::header_size_in_bytes() {
  return offset_of(HGCCardSetHowl, _buckets);
}

#endif
