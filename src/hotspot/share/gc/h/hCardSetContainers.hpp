#ifndef SHARE_GC_H_HCARDSETCONTAINERS_HPP
#define SHARE_GC_H_HCARDSETCONTAINERS_HPP

#include "gc/h/hCardSet.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/globalDefinitions.hpp"

class HGCCardSetInlinePtr : public StackObj {
  using ContainerPtr = HGCCardSet::ContainerPtr;

  ContainerPtr volatile * _value_addr;
  ContainerPtr _value;

  static const uint SizeFieldLen = 3;
  static const uint SizeFieldPos = 2;
  static const uint HeaderSize = HGCCardSet::ContainerPtrHeaderSize + SizeFieldLen;

  static const uint BitsInValue = sizeof(ContainerPtr) * BitsPerByte;

  static const uintptr_t SizeFieldMask = (((uint)1 << SizeFieldLen) - 1) << SizeFieldPos;

  static uint card_pos_for(uint const idx, uint const bits_per_card) {
    return (idx * bits_per_card + HeaderSize);
  }

  static ContainerPtr merge(ContainerPtr orig_value, uint card_in_region, uint idx, uint bits_per_card);

  uint find(uint const card_idx, uint const bits_per_card, uint start_at, uint num_cards);

  static ContainerPtr empty_card_set() {
    static_assert(HGCCardSet::ContainerInlinePtr == 0, "unnecessary warning dodge");
    auto value = HGCCardSet::ContainerInlinePtr;
    return reinterpret_cast<ContainerPtr>(value);
  }

public:
  HGCCardSetInlinePtr() : HGCCardSetInlinePtr(empty_card_set()) {}

  explicit HGCCardSetInlinePtr(ContainerPtr value) :
    HGCCardSetInlinePtr(nullptr, value) {}

  HGCCardSetInlinePtr(ContainerPtr volatile* value_addr, ContainerPtr value) : _value_addr(value_addr), _value(value) {
    assert(HGCCardSet::container_type(_value) == HGCCardSet::ContainerInlinePtr, "Value " PTR_FORMAT " is not a valid HGCCardSetInlinePtr.", p2i(_value));
  }

  HGCAddCardResult add(uint const card_idx, uint const bits_per_card, uint const max_cards_in_inline_ptr);

  bool contains(uint const card_idx, uint const bits_per_card);

  template <class CardVisitor>
  void iterate(CardVisitor& found, uint const bits_per_card);

  operator ContainerPtr () { return _value; }

  static uint max_cards_in_inline_ptr(uint bits_per_card) {
    return (BitsInValue - HeaderSize) / bits_per_card;
  }

  static uint num_cards_in(ContainerPtr value) {
    return ((uintptr_t)value & SizeFieldMask) >> SizeFieldPos;
  }
};

class HGCCardSetContainer {
  uintptr_t _ref_count;
protected:
  ~HGCCardSetContainer() = default;
public:
  HGCCardSetContainer() : _ref_count(3) { }

  uintptr_t refcount() const { return Atomic::load_acquire(&_ref_count); }

  bool try_increment_refcount();

  uintptr_t decrement_refcount();

  static uint LogCardsPerRegionLimit;

  static uint cards_per_region_limit() { return 1u << LogCardsPerRegionLimit; }
};

class HGCCardSetArray : public HGCCardSetContainer {
public:
  typedef uint16_t EntryDataType;
  typedef uint EntryCountType;
  using ContainerPtr = HGCCardSet::ContainerPtr;
private:
  EntryCountType _size;
  EntryCountType volatile _num_entries;
  EntryDataType _data[1];

  static const EntryCountType LockBitMask = (EntryCountType)1 << (sizeof(EntryCountType) * BitsPerByte - 1);
  static const EntryCountType EntryMask = LockBitMask - 1;

  class HGCCardSetArrayLocker : public StackObj {
    EntryCountType volatile* _num_entries_addr;
    EntryCountType _local_num_entries;
  public:
    HGCCardSetArrayLocker(EntryCountType volatile* value);

    EntryCountType num_entries() const { return _local_num_entries; }
    void inc_num_entries() {
      assert(((_local_num_entries + 1) & EntryMask) == (EntryCountType)(_local_num_entries + 1), "no overflow" );
      _local_num_entries++;
    }

    ~HGCCardSetArrayLocker() {
      Atomic::release_store(_num_entries_addr, _local_num_entries);
    }
  };

  EntryDataType const* base_addr() const;
  EntryDataType const* entry_addr(EntryCountType index) const;
  EntryDataType* entry_addr(EntryCountType index);
  EntryDataType at(EntryCountType index) const;
public:
  HGCCardSetArray(uint const card_in_region, EntryCountType num_cards);

  HGCAddCardResult add(uint card_idx);
  bool contains(uint card_idx);

  template <class CardVisitor>
  void iterate(CardVisitor& found);

  size_t num_entries() const { return _num_entries & EntryMask; }

  static size_t header_size_in_bytes();
  static size_t size_in_bytes(size_t num_cards) {
    return header_size_in_bytes() + sizeof(EntryDataType) * num_cards;
  }
};

class HGCCardSetBitMap : public HGCCardSetContainer {
  size_t _num_bits_set;
  BitMap::bm_word_t _bits[1];

public:
  HGCCardSetBitMap(uint const card_in_region, uint const size_in_bits);

  HGCAddCardResult add(uint card_idx, size_t threshold, size_t size_in_bits);

  bool contains(uint card_idx, size_t size_in_bits) {
    BitMapView bm(_bits, size_in_bits);
    return bm.at(card_idx);
  }

  uint num_bits_set() const { return (uint)_num_bits_set; }

  template <class CardVisitor>
  void iterate(CardVisitor& found, size_t const size_in_bits, uint offset);

  uint next(uint const idx, size_t const size_in_bits) {
    BitMapView bm(_bits, size_in_bits);
    return static_cast<uint>(bm.find_first_set_bit(idx));
  }

  static size_t header_size_in_bytes();
  static size_t size_in_bytes(size_t size_in_bits) { return header_size_in_bytes() + BitMap::calc_size_in_words(size_in_bits) * BytesPerWord; }
};

class HGCCardSetHowl : public HGCCardSetContainer {
public:
  typedef uint EntryCountType;
  using ContainerPtr = HGCCardSet::ContainerPtr;
  EntryCountType volatile _num_entries;
private:
  ContainerPtr _buckets[1];

  template <class CardOrRangeVisitor>
  void iterate_cardset(ContainerPtr const container, uint index, CardOrRangeVisitor& found, HGCCardSetConfiguration* config);

  ContainerPtr at(EntryCountType index) const;
  ContainerPtr const* buckets() const;

public:
  HGCCardSetHowl(EntryCountType card_in_region, HGCCardSetConfiguration* config);

  ContainerPtr const* container_addr(EntryCountType index) const;
  ContainerPtr* container_addr(EntryCountType index);

  bool contains(uint card_idx, HGCCardSetConfiguration* config);

  template <class CardOrRangeVisitor>
  void iterate(CardOrRangeVisitor& found, HGCCardSetConfiguration* config);

  template <class ContainerPtrVisitor>
  void iterate(ContainerPtrVisitor& found, uint num_card_sets);

  static EntryCountType num_buckets(size_t size_in_bits, size_t num_cards_in_array, size_t max_buckets);

  static EntryCountType bitmap_size(size_t size_in_bits, uint num_buckets) {
    EntryCountType num_cards = (EntryCountType)size_in_bits / num_buckets;
    return round_up_power_of_2(num_cards);
  }

  static size_t header_size_in_bytes();
  static size_t size_in_bytes(size_t num_arrays) {
    return header_size_in_bytes() + sizeof(ContainerPtr) * num_arrays;
  }
};

#endif
