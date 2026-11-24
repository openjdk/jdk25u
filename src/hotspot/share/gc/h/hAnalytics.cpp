#include "gc/h/hAnalytics.hpp"
#include "gc/h/hPredictions.hpp"
#include "gc/shared/gc_globals.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/numberSeq.hpp"

static double cost_per_logged_card_ms_defaults[] = {
  0.01, 0.005, 0.005, 0.003, 0.003, 0.002, 0.002, 0.0015
};

HGCAnalytics::HGCAnalytics(const HGCPredictions* predictor) :
  _predictor(predictor),
  _alloc_rate_ms_seq(TruncatedSeqLength),
  _concurrent_refine_rate_ms_seq(TruncatedSeqLength),
  _dirtied_cards_rate_ms_seq(TruncatedSeqLength) {
  _concurrent_refine_rate_ms_seq.add(1/cost_per_logged_card_ms_defaults[0]);
  _dirtied_cards_rate_ms_seq.add(0.0);
}

bool HGCAnalytics::enough_samples_available(TruncatedSeq const* seq) {
  return seq->num() >= 3;
}

double HGCAnalytics::predict_zero_bounded(TruncatedSeq const* seq) const {
  return _predictor->predict_zero_bounded(seq);
}

size_t HGCAnalytics::predict_size(TruncatedSeq const* seq) const {
  return (size_t)predict_zero_bounded(seq);
}

void HGCAnalytics::report_alloc_rate_ms(double alloc_rate) {
  _alloc_rate_ms_seq.add(alloc_rate);
}

void HGCAnalytics::report_concurrent_refine_rate_ms(double cards_per_ms) {
  _concurrent_refine_rate_ms_seq.add(cards_per_ms);
}

void HGCAnalytics::report_dirtied_cards_rate_ms(double cards_per_ms) {
  _dirtied_cards_rate_ms_seq.add(cards_per_ms);
}

double HGCAnalytics::predict_alloc_rate_ms() const {
  if (enough_samples_available(&_alloc_rate_ms_seq)) {
    return predict_zero_bounded(&_alloc_rate_ms_seq);
  } else {
    return 0.0;
  }
}

double HGCAnalytics::predict_concurrent_refine_rate_ms() const {
  return predict_zero_bounded(&_concurrent_refine_rate_ms_seq);
}

double HGCAnalytics::predict_dirtied_cards_rate_ms() const {
  return predict_zero_bounded(&_dirtied_cards_rate_ms_seq);
}
