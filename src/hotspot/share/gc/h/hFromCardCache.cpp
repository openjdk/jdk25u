#include "gc/h/hDirtyCardQueue.hpp"
#include "gc/h/hFromCardCache.hpp"
#include "gc/shared/gc_globals.hpp"
#include "memory/padded.inline.hpp"
#include "runtime/globals.hpp"
#include "utilities/debug.hpp"

uintptr_t** HGCFromCardCache::_cache = nullptr;
uint        HGCFromCardCache::_max_reserved_regions = 0;
size_t      HGCFromCardCache::_static_mem_size = 0;
#ifdef ASSERT
uint   HGCFromCardCache::_max_workers = 0;
#endif

void HGCFromCardCache::initialize(uint max_reserved_regions) {
  guarantee(max_reserved_regions > 0, "Heap size must be valid");
  guarantee(_cache == nullptr, "Should not call this multiple times");

  _max_reserved_regions = max_reserved_regions;
#ifdef ASSERT
  _max_workers = num_par_rem_sets();
#endif
  _cache = Padded2DArray<uintptr_t, mtGC>::create_unfreeable(_max_reserved_regions,
                                                             num_par_rem_sets(),
                                                             &_static_mem_size);

  if (AlwaysPreTouch) {
    invalidate(0, _max_reserved_regions);
  }
}

void HGCFromCardCache::invalidate(uint start_idx, size_t new_num_regions) {
  guarantee((size_t)start_idx + new_num_regions <= max_uintx,
            "Trying to invalidate beyond maximum region, from %u size %zu",
            start_idx, new_num_regions);
  uint end_idx = (start_idx + (uint)new_num_regions);
  assert(end_idx <= _max_reserved_regions, "Must be within max.");

  for (uint i = 0; i < num_par_rem_sets(); i++) {
    for (uint j = start_idx; j < end_idx; j++) {
      set(i, j, InvalidCard);
    }
  }
}

#ifndef PRODUCT
#endif

uint HGCFromCardCache::num_par_rem_sets() {
  return HGCDirtyCardQueueSet::num_par_ids() + HGCConcRefinementThreads + MAX2(ConcGCThreads, ParallelGCThreads);
}

void HGCFromCardCache::clear(uint region_idx) {
  uint num_par_remsets = num_par_rem_sets();
  for (uint i = 0; i < num_par_remsets; i++) {
    set(i, region_idx, InvalidCard);
  }
}

void HGCFromCardCache::print(outputStream* out) {
  for (uint i = 0; i < num_par_rem_sets(); i++) {
    for (uint j = 0; j < _max_reserved_regions; j++) {
      out->print_cr("_from_card_cache[%u][%u] = %zu.",
                    i, j, at(i, j));
    }
  }
}
