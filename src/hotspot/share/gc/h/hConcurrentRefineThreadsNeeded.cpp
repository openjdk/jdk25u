#include "gc/h/hAnalytics.hpp"
#include "gc/h/hConcurrentRefineThreadsNeeded.hpp"
#include "gc/h/hPolicy.hpp"
#include "gc/shared/gc_globals.hpp"
#include "utilities/globalDefinitions.hpp"

#include <math.h>

HGCConcurrentRefineThreadsNeeded::HGCConcurrentRefineThreadsNeeded(HGCPolicy* policy,
                                                                   double update_period_ms) :
  _policy(policy),
  _update_period_ms(update_period_ms),
  _predicted_time_until_next_gc_ms(0.0),
  _predicted_cards_at_next_gc(0),
  _threads_needed(0)
{}

void HGCConcurrentRefineThreadsNeeded::update(uint active_threads,
                                              size_t available_bytes,
                                              size_t num_cards,
                                              size_t target_num_cards) {
  const HGCAnalytics* analytics = _policy->analytics();

  double alloc_region_rate = analytics->predict_alloc_rate_ms();
  double alloc_bytes_rate = alloc_region_rate * HGCRegionSize;
  if (alloc_bytes_rate == 0.0) {
    _predicted_time_until_next_gc_ms = 0.0;
  } else {
    const double one_hour_ms = 60.0 * 60.0 * MILLIUNITS;
    double raw_time_ms = available_bytes / alloc_bytes_rate;
    _predicted_time_until_next_gc_ms = MIN2(raw_time_ms, one_hour_ms);
  }

  size_t incoming_cards = 0;
  if (_predicted_time_until_next_gc_ms > _update_period_ms) {
    double incoming_rate = analytics->predict_dirtied_cards_rate_ms();
    double raw_cards = incoming_rate * _predicted_time_until_next_gc_ms;
    incoming_cards = static_cast<size_t>(raw_cards);
  }
  size_t total_cards = num_cards + incoming_cards;
  _predicted_cards_at_next_gc = total_cards;

  if (total_cards <= target_num_cards) {
    _threads_needed = 0;
    return;
  }

  if (_predicted_time_until_next_gc_ms <= _update_period_ms) {
    _threads_needed = MAX2(active_threads, 1u);
    return;
  }

  size_t cards_needed = total_cards - target_num_cards;

  double refine_rate = analytics->predict_concurrent_refine_rate_ms();
  if (refine_rate == 0.0) {
    _threads_needed = 1;
    return;
  }

  double thread_capacity = refine_rate * _predicted_time_until_next_gc_ms;
  double nthreads = cards_needed / thread_capacity;

  if (_predicted_time_until_next_gc_ms <= _update_period_ms * 5.0) {
    nthreads = ::ceil(nthreads);
  } else {
    nthreads = ::round(nthreads);
  }

  _threads_needed = static_cast<uint>(MIN2<size_t>(nthreads, UINT_MAX));
}
