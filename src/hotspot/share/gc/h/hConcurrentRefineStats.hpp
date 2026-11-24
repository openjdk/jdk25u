#ifndef SHARE_GC_H_HCONCURRENTREFINESTATS_HPP
#define SHARE_GC_H_HCONCURRENTREFINESTATS_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ticks.hpp"

class HGCConcurrentRefineStats : public CHeapObj<mtGC> {
  Tickspan _refinement_time;
  size_t _refined_cards;
  size_t _precleaned_cards;
  size_t _dirtied_cards;

public:
  HGCConcurrentRefineStats();

  Tickspan refinement_time() const { return _refinement_time; }

  size_t refined_cards() const { return _refined_cards; }

  double refinement_rate_ms() const;

  size_t precleaned_cards() const { return _precleaned_cards; }

  size_t dirtied_cards() const { return _dirtied_cards; }

  void inc_refinement_time(Tickspan t) { _refinement_time += t; }
  void inc_refined_cards(size_t cards) { _refined_cards += cards; }
  void inc_precleaned_cards(size_t cards) { _precleaned_cards += cards; }
  void inc_dirtied_cards(size_t cards) { _dirtied_cards += cards; }

  HGCConcurrentRefineStats& operator+=(const HGCConcurrentRefineStats& other);
  HGCConcurrentRefineStats& operator-=(const HGCConcurrentRefineStats& other);

  friend HGCConcurrentRefineStats operator+(HGCConcurrentRefineStats x,
                                            const HGCConcurrentRefineStats& y) {
    return x += y;
  }

  friend HGCConcurrentRefineStats operator-(HGCConcurrentRefineStats x,
                                            const HGCConcurrentRefineStats& y) {
    return x -= y;
  }

  void reset();
};

#endif
