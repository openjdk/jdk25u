#ifndef SHARE_GC_H_HFROMCARDCACHE_HPP
#define SHARE_GC_H_HFROMCARDCACHE_HPP

#include "memory/allStatic.hpp"
#include "utilities/ostream.hpp"

class HGCFromCardCache : public AllStatic {
private:
  static uintptr_t** _cache;
  static uint _max_reserved_regions;
  static size_t _static_mem_size;
#ifdef ASSERT
  static uint _max_workers;

  static void check_bounds(uint worker_id, uint region_idx) {
    assert(worker_id < _max_workers, "Worker_id %u is larger than maximum %u", worker_id, _max_workers);
    assert(region_idx < _max_reserved_regions, "Region_idx %u is larger than maximum %u", region_idx, _max_reserved_regions);
  }
#endif

  static const uintptr_t InvalidCard = 0;

  static uint num_par_rem_sets();

public:
  static void clear(uint region_idx);

  static bool contains(uint worker_id, uint region_idx, uintptr_t card) {
    return at(worker_id, region_idx) == card;
  }

  static bool contains_or_replace(uint worker_id, uint region_idx, uintptr_t card) {
    uintptr_t card_in_cache = at(worker_id, region_idx);
    if (card_in_cache == card) {
      return true;
    } else {
      set(worker_id, region_idx, card);
      return false;
    }
  }

  static uintptr_t at(uint worker_id, uint region_idx) {
    DEBUG_ONLY(check_bounds(worker_id, region_idx);)
    return _cache[region_idx][worker_id];
  }

  static void set(uint worker_id, uint region_idx, uintptr_t val) {
    DEBUG_ONLY(check_bounds(worker_id, region_idx);)
    _cache[region_idx][worker_id] = val;
  }

  static void initialize(uint max_reserved_regions);

  static void invalidate(uint start_idx, size_t num_regions);

  static void print(outputStream* out = tty) PRODUCT_RETURN;

  static size_t static_mem_size() {
    return _static_mem_size;
  }
};

#endif
