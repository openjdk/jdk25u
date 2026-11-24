#ifndef SHARE_GC_H_HCONCURRENTREFINETHREADSNEEDED_HPP
#define SHARE_GC_H_HCONCURRENTREFINETHREADSNEEDED_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class HGCAnalytics;
class HGCPolicy;

class HGCConcurrentRefineThreadsNeeded : public CHeapObj<mtGC> {
  HGCPolicy* _policy;
  double _update_period_ms;
  double _predicted_time_until_next_gc_ms;
  size_t _predicted_cards_at_next_gc;
  uint _threads_needed;

public:
  HGCConcurrentRefineThreadsNeeded(HGCPolicy* policy, double update_period_ms);

  void update(uint active_threads,
              size_t available_bytes,
              size_t num_cards,
              size_t target_num_cards);

  uint threads_needed() const { return _threads_needed; }

  double predicted_time_until_next_gc_ms() const {
    return _predicted_time_until_next_gc_ms;
  }

  size_t predicted_cards_at_next_gc() const {
    return _predicted_cards_at_next_gc;
  }
};

#endif
