#ifndef SHARE_GC_H_HCARDSET_HPP
#define SHARE_GC_H_HCARDSET_HPP

#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"

class HGCCardSetAllocOptions;
class HGCCardSetHashTable;
class HGCCardSetHashTableValue;
class HGCCardSetMemoryManager;
class HGCMonotonicArenaFreePool;
class HGCMonotonicArenaMemoryStats;
class Mutex;
class outputStream;

enum HGCAddCardResult : u1 {
  Overflow,
  Found,
  Added
};

enum HGCContainerType : u1 {
  HowlInline,
  HowlArrayOfCards,
  HowlBitmap,
  HowlFull,
  MergedInline,
  MergedArrayOfCards,
  MergedFull,
  MergedHowl
};

class HGCCardSetConfiguration : public CHeapObj<mtGC> {
  uint _inline_ptr_bits_per_card;

  uint _max_cards_in_array;
  uint _num_buckets_in_howl;
  uint _max_cards_in_card_set;
  uint _cards_in_howl_threshold;
  uint _max_cards_in_howl_bitmap;
  uint _cards_in_howl_bitmap_threshold;
  uint _log2_max_cards_in_howl_bitmap;
  uint _bitmap_hash_mask;
  uint _log2_card_regions_per_heap_region;
  uint _log2_cards_per_card_region;

  HGCCardSetAllocOptions* _card_set_alloc_options;

  HGCCardSetConfiguration(uint inline_ptr_bits_per_card,
                          uint max_cards_in_array,
                          double cards_in_bitmap_threshold_percent,
                          uint num_buckets_in_howl,
                          double cards_in_howl_threshold_percent,
                          uint max_cards_in_card_set,
                          uint log2_card_regions_per_heap_region);
  void init_card_set_alloc_options();

  void log_configuration();

public:
  HGCCardSetConfiguration();
  HGCCardSetConfiguration(uint max_cards_in_array,
                          double cards_in_bitmap_threshold_percent,
                          uint max_buckets_in_howl,
                          double cards_in_howl_threshold_percent,
                          uint max_cards_in_cardset,
                          uint log2_card_region_per_region);

  ~HGCCardSetConfiguration();

  uint inline_ptr_bits_per_card() const { return _inline_ptr_bits_per_card; }
  uint max_cards_in_inline_ptr() const;
  static uint max_cards_in_inline_ptr(uint bits_per_card);

  uint max_cards_in_array() const { return _max_cards_in_array; }

  uint max_cards_in_howl_bitmap() const { return _max_cards_in_howl_bitmap; }
  uint cards_in_howl_bitmap_threshold() const { return _cards_in_howl_bitmap_threshold; }
  uint log2_max_cards_in_howl_bitmap() const { return _log2_max_cards_in_howl_bitmap; }

  uint num_buckets_in_howl() const { return _num_buckets_in_howl; }
  uint cards_in_howl_threshold() const { return _cards_in_howl_threshold; }
  uint howl_bitmap_offset(uint card_idx) const { return card_idx & _bitmap_hash_mask; }
  uint howl_bucket_index(uint card_idx) { return card_idx >> _log2_max_cards_in_howl_bitmap; }

  uint max_cards_in_region() const { return _max_cards_in_card_set; }

  uint log2_card_regions_per_heap_region() const { return _log2_card_regions_per_heap_region; }
  uint log2_cards_per_card_region() const { return _log2_cards_per_card_region; }

  static constexpr uint num_mem_object_types() { return 4; }
  const HGCCardSetAllocOptions* mem_object_alloc_options(uint idx);

  static const char* mem_object_type_name_str(uint index);
};

class HGCCardSetCoarsenStats {
public:
  static constexpr size_t NumCoarsenCategories = 7;
  static constexpr size_t CoarsenHowlOffset = 4;

private:
  size_t _coarsen_from[NumCoarsenCategories];
  size_t _coarsen_collision[NumCoarsenCategories];

public:
  HGCCardSetCoarsenStats() { reset(); }

  void reset();

  void set(HGCCardSetCoarsenStats& other);

  void subtract_from(HGCCardSetCoarsenStats& other);

  void record_coarsening(uint tag, bool collision);

  void print_on(outputStream* out);
};

class HGCCardSet : public CHeapObj<mtGC> {
  friend class HGCTransferCard;
  friend class HGCReleaseCardsets;

  static uint _split_card_shift;
  static size_t _split_card_mask;

  static HGCCardSetCoarsenStats _coarsen_stats;
  static HGCCardSetCoarsenStats _last_coarsen_stats;

public:
  static const uintptr_t ContainerPtrHeaderSize = 2;

  using ContainerPtr = void*;
  static const uintptr_t ContainerInlinePtr      = 0x0;
  static const uintptr_t ContainerArrayOfCards   = 0x1;
  static const uintptr_t ContainerBitMap         = 0x2;
  static const uintptr_t ContainerHowl           = 0x3;

  static constexpr ContainerPtr FreeCardSet = nullptr;
  static ContainerPtr FullCardSet;

  static const uintptr_t ContainerPtrTypeMask = ((uintptr_t)1 << ContainerPtrHeaderSize) - 1;

  static ContainerPtr strip_container_type(ContainerPtr ptr) { return (ContainerPtr)((uintptr_t)ptr & ~ContainerPtrTypeMask); }

  static uint container_type(ContainerPtr ptr) { return (uintptr_t)ptr & ContainerPtrTypeMask; }

  template <class T>
  static T* container_ptr(ContainerPtr ptr);

private:

  HGCCardSetMemoryManager* _mm;
  HGCCardSetConfiguration* _config;

  HGCCardSetHashTable* _table;

  size_t _num_occupied;

  ContainerPtr make_container_ptr(void* value, uintptr_t type);

  ContainerPtr acquire_container(ContainerPtr volatile* container_addr);
  bool release_container(ContainerPtr container);
  void release_and_maybe_free_container(ContainerPtr container);
  void release_and_must_free_container(ContainerPtr container);

  bool coarsen_container(ContainerPtr volatile* container_addr,
                         ContainerPtr cur_container,
                         uint card_in_region, bool within_howl = false);

  ContainerPtr create_coarsened_array_of_cards(uint card_in_region, bool within_howl);

  void transfer_cards(HGCCardSetHashTableValue* table_entry, ContainerPtr source_container, uint card_region);
  void transfer_cards_in_howl(ContainerPtr parent_container, ContainerPtr source_container, uint card_region);

  HGCAddCardResult add_to_container(ContainerPtr volatile* container_addr, ContainerPtr container, uint card_region, uint card, bool increment_total = true);

  HGCAddCardResult add_to_inline_ptr(ContainerPtr volatile* container_addr, ContainerPtr container, uint card_in_region);
  HGCAddCardResult add_to_array(ContainerPtr container, uint card_in_region);
  HGCAddCardResult add_to_bitmap(ContainerPtr container, uint card_in_region);
  HGCAddCardResult add_to_howl(ContainerPtr parent_container, uint card_region, uint card_in_region, bool increment_total = true);

  HGCCardSetHashTableValue* get_or_add_container(uint card_region, bool* should_grow_table);
  HGCCardSetHashTableValue* get_container(uint card_region);

  template <class CardVisitor>
  void iterate_cards_during_transfer(ContainerPtr const container, CardVisitor& vl);

  uint container_type_to_mem_object_type(uintptr_t type) const;
  uint8_t* allocate_mem_object(uintptr_t type);
  void free_mem_object(ContainerPtr container);

  void split_card(uintptr_t card, uint& card_region, uint& card_within_region) const;

  HGCAddCardResult add_card(uint card_region, uint card_in_region, bool increment_total = true);

  bool contains_card(uint card_region, uint card_in_region);

  class CardClosure {
  public:
    virtual void do_card(uint region_idx, uint card_idx) = 0;
  };

  void iterate_cards(CardClosure& cl);

public:
  HGCCardSetConfiguration* config() const { return _config; }

  HGCCardSet(HGCCardSetConfiguration* config, HGCMonotonicArenaFreePool* free_list_pool);
  virtual ~HGCCardSet();

  static void initialize(MemRegion reserved);

  HGCAddCardResult add_card(uintptr_t card);

  bool contains_card(uintptr_t card);

  void print_info(outputStream* st, uintptr_t card);

  bool occupancy_less_or_equal_to(size_t limit) const;

  bool is_empty() const;

  size_t occupied() const;

  size_t num_containers();

  static HGCCardSetCoarsenStats coarsen_stats();
  static void print_coarsen_stats(outputStream* out);

  size_t mem_size() const;
  size_t unused_mem_size() const;
  HGCMonotonicArenaMemoryStats memory_stats() const;
  static size_t static_mem_size();

  void clear();

  void reset_table_scanner();

  void reset_table_scanner_for_groups();

  template <class CardOrRangeVisitor>
  void iterate_cards_or_ranges_in_container(ContainerPtr const container, CardOrRangeVisitor& cl);

  class ContainerPtrClosure {
  public:
    virtual void do_containerptr(uint card_region_idx, size_t num_occupied, ContainerPtr container) = 0;
  };

  void iterate_containers(ContainerPtrClosure* cl, bool safepoint = false);
};

class HGCCardSetHashTableValue {
public:
  using ContainerPtr = HGCCardSet::ContainerPtr;

  const uint _region_idx;
  uint volatile _num_occupied;
  ContainerPtr volatile _container;

  HGCCardSetHashTableValue(uint region_idx, ContainerPtr container) : _region_idx(region_idx), _num_occupied(0), _container(container) { }
};

#endif
