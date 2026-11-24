#ifndef SHARE_GC_H_HPREDICTIONS_HPP
#define SHARE_GC_H_HPREDICTIONS_HPP

#include "utilities/numberSeq.hpp"

class HGCPredictions {
private:
  double _stddev_scale;

  double stddev_estimate(TruncatedSeq const* seq) const {
    double estimate = seq->dsd();
    int const samples = seq->num();
    if (samples < 5) {
      estimate = MAX2(seq->davg() * (5 - samples) / 2.0, estimate);
    }
    return estimate;
  }

public:
  HGCPredictions(double stddev_scale) : _stddev_scale(stddev_scale) {
    assert(stddev_scale >= 0.0, "must be");
  }

  double predict(TruncatedSeq const* seq) const {
    return seq->davg() + _stddev_scale * stddev_estimate(seq);
  }

  double predict_in_unit_interval(TruncatedSeq const* seq) const {
    return clamp(predict(seq), 0.0, 1.0);
  }

  double predict_zero_bounded(TruncatedSeq const* seq) const {
    return MAX2(predict(seq), 0.0);
  }
};

#endif
