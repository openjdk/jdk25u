#include "gc/h/hConcurrentRefineStats.hpp"

HGCConcurrentRefineStats::HGCConcurrentRefineStats() :
  _refinement_time(),
  _refined_cards(0),
  _precleaned_cards(0),
  _dirtied_cards(0)
{}

double HGCConcurrentRefineStats::refinement_rate_ms() const {
  double secs = refinement_time().seconds();
  return (secs > 0) ? (refined_cards() / (secs * MILLIUNITS)) : 0.0;
}

HGCConcurrentRefineStats&
HGCConcurrentRefineStats::operator+=(const HGCConcurrentRefineStats& other) {
  _refinement_time += other._refinement_time;
  _refined_cards += other._refined_cards;
  _precleaned_cards += other._precleaned_cards;
  _dirtied_cards += other._dirtied_cards;
  return *this;
}

template<typename T>
static T clipped_sub(T x, T y) {
  return (x < y) ? T() : (x - y);
}

HGCConcurrentRefineStats&
HGCConcurrentRefineStats::operator-=(const HGCConcurrentRefineStats& other) {
  _refinement_time = clipped_sub(_refinement_time, other._refinement_time);
  _refined_cards = clipped_sub(_refined_cards, other._refined_cards);
  _precleaned_cards = clipped_sub(_precleaned_cards, other._precleaned_cards);
  _dirtied_cards = clipped_sub(_dirtied_cards, other._dirtied_cards);
  return *this;
}

void HGCConcurrentRefineStats::reset() {
  *this = HGCConcurrentRefineStats();
}
