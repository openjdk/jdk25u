#ifndef SHARE_GC_H_HCARDTABLE_HPP
#define SHARE_GC_H_HCARDTABLE_HPP

#include "gc/shared/cardTable.hpp"
#include "memory/allocation.hpp"

class HGCCardTable : public CardTable {
public:
  enum HGCCardValues {
    young_card       = CT_MR_BS_last_reserved,
    mixed_young_card = CT_MR_BS_last_reserved + 1,
    mixed_clean_card = CT_MR_BS_last_reserved + 2,
    mixed_dirty_card = CT_MR_BS_last_reserved + 3,
  };

  explicit HGCCardTable(MemRegion whole_heap);

  static CardValue young_card_val()       { return young_card; }
  static CardValue mixed_young_card_val() { return mixed_young_card; }
  static CardValue mixed_clean_card_val() { return mixed_clean_card; }
  static CardValue mixed_dirty_card_val() { return mixed_dirty_card; }

  static bool is_young_card_value(CardValue v) {
    return (v == young_card) || (v == mixed_young_card);
  }
  static bool is_clean_card_value(CardValue v) {
    return (v == clean_card_val()) || (v == mixed_clean_card);
  }
  static bool is_dirty_card_value(CardValue v) {
    return (v == dirty_card_val()) || (v == mixed_dirty_card);
  }

  void mark_as_young(const MemRegion& mr);

  virtual bool is_in_young(const void* p) const override;
};

#endif
