#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hRegionRemSet.inline.hpp"
#include "utilities/globalDefinitions.hpp"

extern volatile size_t hgc_refine_cards;
extern volatile size_t hgc_refine_oops;
extern volatile size_t hgc_refine_same;
extern volatile size_t hgc_refine_added;

HeapWord* HGCHeapRegionRemSet::_heap_base_address = nullptr;
const char* HGCHeapRegionRemSet::_state_strings[] =  {"Untracked", "Updating", "Complete"};
const char* HGCHeapRegionRemSet::_short_state_strings[] =  {"UNTRA", "UPDAT", "CMPLT"};

void HGCHeapRegionRemSet::initialize(MemRegion reserved) {
  HGCCardSet::initialize(reserved);
  _heap_base_address = reserved.start();
}

void HGCHeapRegionRemSet::uninstall_card_set() {
  delete _card_set;
  _card_set = nullptr;
}

HGCHeapRegionRemSet::HGCHeapRegionRemSet(HGCRegion* hr) :
  _card_set(nullptr),
  _hr(hr),
  _state(Untracked),
  _lock(0) { }

HGCHeapRegionRemSet::~HGCHeapRegionRemSet() {
  assert(!has_card_set(), "Still assigned to a card set");
}

uint HGCHeapRegionRemSet::region_index() const {
  return HGCCollectedHeap::heap()->region_index(_hr->start());
}

void HGCHeapRegionRemSet::clear_fcc() {
  HGCFromCardCache::clear(region_index());
}

void HGCHeapRegionRemSet::clear(bool only_cardset, bool keep_tracked) {
  clear_fcc();

  if (has_card_set()) {
    card_set()->clear();
    assert(card_set()->occupied() == 0, "Should be clear.");
  }

  if (!keep_tracked) {
    set_state_untracked();
  } else {
    assert(is_tracked(), "must be");
  }
}

void HGCHeapRegionRemSet::reset_table_scanner() {
  if (has_card_set()) {
    card_set()->reset_table_scanner();
  }
}

HGCMonotonicArenaMemoryStats HGCHeapRegionRemSet::card_set_memory_stats() const {
  assert(has_card_set(), "pre-condition");
  return card_set()->memory_stats();
}

void HGCHeapRegionRemSet::print_static_mem_size(outputStream* out) {
  out->print_cr("  Static structures = %zu", HGCHeapRegionRemSet::static_mem_size());
}
