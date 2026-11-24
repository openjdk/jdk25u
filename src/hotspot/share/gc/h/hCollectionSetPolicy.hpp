#ifndef SHARE_GC_H_HCOLLECTIONSETPOLICY_HPP
#define SHARE_GC_H_HCOLLECTIONSETPOLICY_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class HGCCollectionSetPolicy : public CHeapObj<mtGC> {
  static size_t default_limit();

  const size_t _base_limit;
  const size_t _min_limit;
  const size_t _max_limit;
  size_t _limit;
  size_t _active_processors;

  jlong _last_cycle_end_ns;
  jlong _cycle_start_ns;
  jlong _remark_start_ns;
  size_t _last_cset_regions;
  double _fill_ewma;
  double _interval_ewma_s;
  double _remark_ewma_ms;
  bool _adaptive;

  void adjust();

public:
  HGCCollectionSetPolicy();

  size_t limit() const;

  void cycle_started(size_t cset_regions);
  void remark_started();
  void remark_finished();
  void cycle_finished(size_t );
};

#endif
