#include "gc/h/hCollectionSetPolicy.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "utilities/ticks.hpp"

size_t HGCCollectionSetPolicy::default_limit() {
  return (size_t)os::active_processor_count() * HGCCSetCPUMultiplier;
}

HGCCollectionSetPolicy::HGCCollectionSetPolicy() :
    _base_limit(default_limit()),
    _min_limit(MAX2((size_t)1, _base_limit / 2)),
    _max_limit(_base_limit * 4),
    _limit(_base_limit),
    _active_processors((size_t)os::active_processor_count()),
    _last_cycle_end_ns(0),
    _cycle_start_ns(0),
    _remark_start_ns(0),
    _last_cset_regions(0),
    _fill_ewma(0.0),
    _interval_ewma_s(0.0),
    _remark_ewma_ms(0.0),
    _adaptive(HGCAdaptiveCSet) {
}

size_t HGCCollectionSetPolicy::limit() const {
  return Atomic::load(&_limit);
}

void HGCCollectionSetPolicy::cycle_started(size_t cset_regions) {
  const jlong now = Ticks::now().value();
  if (_last_cycle_end_ns != 0 && cset_regions > 0) {
    const double interval_s = (double)(now - _last_cycle_end_ns) / 1.0e9;
    if (interval_s > 0.001) {
      const double rate = (double)cset_regions / interval_s;
      _fill_ewma = (_fill_ewma == 0.0) ? rate : (0.75 * _fill_ewma + 0.25 * rate);
      _interval_ewma_s = (_interval_ewma_s == 0.0) ? interval_s : (0.75 * _interval_ewma_s + 0.25 * interval_s);
    }
  }
  _cycle_start_ns = now;
  _last_cset_regions = cset_regions;
}

void HGCCollectionSetPolicy::remark_started() {
  _remark_start_ns = Ticks::now().value();
}

void HGCCollectionSetPolicy::remark_finished() {
  if (_remark_start_ns != 0) {
    const double ms = (double)(Ticks::now().value() - _remark_start_ns) / 1.0e6;
    _remark_ewma_ms = (_remark_ewma_ms == 0.0) ? ms : (0.75 * _remark_ewma_ms + 0.25 * ms);
  }
}

void HGCCollectionSetPolicy::adjust() {
  if (!_adaptive || _interval_ewma_s <= 0.0) {
    return;
  }

  const double low_interval_s = 0.25;
  const double high_interval_s = 1.0;
  size_t next = _limit;
  const size_t step = MAX2((size_t)1, _base_limit / 8);

  if (_interval_ewma_s < low_interval_s) {
    next = _limit + step;
  } else if (_interval_ewma_s > high_interval_s) {
    next = _limit > step ? _limit - step : 0;
  }

  const size_t heap_regions = HGCCollectedHeap::heap()->count_of_regions();
  const size_t dynamic_max = heap_regions > 1 ? heap_regions / 2 : _max_limit;
  const size_t max_limit = MAX2(_min_limit, MIN2(_max_limit, dynamic_max));
  next = MAX2(_min_limit, MIN2(max_limit, next));

  if (next != _limit) {
    Atomic::store(&_limit, next);
    log_debug(gc, cset)("HGC cset adaptive: limit=%zu base=%zu min=%zu max=%zu"
                        " interval=%.3fs fill=%.3f regions/s remark=%.3fms",
                        next, _base_limit, _min_limit, max_limit,
                        _interval_ewma_s, _fill_ewma, _remark_ewma_ms);
  }
}

void HGCCollectionSetPolicy::cycle_finished(size_t ) {
  _last_cycle_end_ns = Ticks::now().value();
  adjust();
}
