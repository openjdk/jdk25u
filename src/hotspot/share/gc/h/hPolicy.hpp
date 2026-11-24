#ifndef SHARE_GC_H_HPOLICY_HPP
#define SHARE_GC_H_HPOLICY_HPP

#include "gc/h/hPredictions.hpp"
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class HGCAnalytics;

class HGCPolicy : public CHeapObj<mtGC> {
  HGCPredictions _predictor;
  HGCAnalytics* _analytics;

public:
  HGCPolicy();
  ~HGCPolicy();

  const HGCAnalytics* analytics() const { return _analytics; }

  bool use_adaptive_young_list_length() const { return false; }

  void revise_young_list_target_length(size_t card_rs_length,
                                       size_t sampled_code_root_rs_length) {}

  size_t estimate_used_young_bytes_locked() const;

  size_t young_list_target_length() const;
};

#endif
