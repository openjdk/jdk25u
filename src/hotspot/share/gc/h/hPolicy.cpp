#include "gc/h/hAnalytics.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hPolicy.hpp"
#include "gc/shared/gc_globals.hpp"

HGCPolicy::HGCPolicy() :
  _predictor(0.5),
  _analytics(new HGCAnalytics(&_predictor))
{}

HGCPolicy::~HGCPolicy() {
  delete _analytics;
}

size_t HGCPolicy::estimate_used_young_bytes_locked() const {
  return HGCCollectedHeap::heap()->used();
}

size_t HGCPolicy::young_list_target_length() const {
  return HGCCollectedHeap::heap()->cset_limit();
}
