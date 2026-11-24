#include "gc/h/hCardTable.hpp"

#include "gc/shared/memset_with_concurrent_readers.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"

#include <cstring>

HGCCardTable::HGCCardTable(MemRegion whole_heap) :
    CardTable(whole_heap) {
  initialize(whole_heap.start(), whole_heap.end());
  _covered[0] = _whole_heap;
  _covered[1] = MemRegion(_whole_heap.end(), (size_t)0);
  os::commit_memory_or_exit((char*)_byte_map, _byte_map_size, false, "HGC card table");
  memset(_byte_map, clean_card_val(), _byte_map_size);
}

void HGCCardTable::mark_as_young(const MemRegion& mr) {
  CardValue* const first = byte_for(mr.start());
  CardValue* const last = byte_after(mr.last());

  for (CardValue* c = first; c < last; ++c) {
    *c = young_card;
  }
}

bool HGCCardTable::is_in_young(const void* p) const {
  return is_young_card_value(*byte_for(p));
}
