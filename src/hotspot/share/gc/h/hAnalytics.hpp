#ifndef SHARE_GC_H_HANALYTICS_HPP
#define SHARE_GC_H_HANALYTICS_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/numberSeq.hpp"

class HGCPredictions;

class HGCAnalytics : public CHeapObj<mtGC> {
  static const int TruncatedSeqLength = 10;
  const HGCPredictions* _predictor;

  TruncatedSeq _alloc_rate_ms_seq;
  TruncatedSeq _concurrent_refine_rate_ms_seq;
  TruncatedSeq _dirtied_cards_rate_ms_seq;

  double predict_zero_bounded(TruncatedSeq const* seq) const;
  size_t predict_size(TruncatedSeq const* seq) const;

public:
  HGCAnalytics(const HGCPredictions* predictor);

  static bool enough_samples_available(TruncatedSeq const* seq);

  void report_alloc_rate_ms(double alloc_rate);
  void report_concurrent_refine_rate_ms(double cards_per_ms);
  void report_dirtied_cards_rate_ms(double cards_per_ms);

  double predict_alloc_rate_ms() const;
  double predict_concurrent_refine_rate_ms() const;
  double predict_dirtied_cards_rate_ms() const;
};

#endif
