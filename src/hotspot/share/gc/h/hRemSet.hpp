#ifndef SHARE_GC_H_HREMSET_HPP
#define SHARE_GC_H_HREMSET_HPP

#include "gc/h/hCardTable.hpp"
#include "memory/allocation.hpp"

class HGCCollectedHeap;

class HGCRemSet : public CHeapObj<mtGC> {
public:
  typedef CardTable::CardValue CardValue;

private:
  HGCCollectedHeap* _heap;
  HGCCardTable* _ct;

public:
  HGCRemSet(HGCCollectedHeap* heap, HGCCardTable* ct);
  ~HGCRemSet();

  void initialize(uint max_reserved_regions);

  bool clean_card_before_refine(CardValue** const card_ptr_addr);
  void refine_card_concurrently(CardValue* const card_ptr, const uint worker_id);
};

#endif
