#ifndef SHARE_GC_H_HOOPCLOSURES_INLINE_HPP
#define SHARE_GC_H_HOOPCLOSURES_INLINE_HPP

#include "gc/h/hOopClosures.hpp"

#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/h/hRegionRemSet.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/globals.hpp"

template <class T>
inline bool HGCInSameRegion(T* p, oop obj) {
  return (((uintptr_t)p ^ cast_from_oop<uintptr_t>(obj)) >> log2i(HGCRegionSize)) == 0;
}

template <class T>
inline void HGCConcurrentRefineOopClosure::do_oop_work(T* p) {
  T o = RawAccess<MO_RELAXED>::oop_load(p);
  if (CompressedOops::is_null(o)) {
    return;
  }

  oop obj = CompressedOops::decode_raw_not_null(o);
  Atomic::inc(&hgc_refine_oops);
  if (p == hgc_probe_field) {
    Atomic::inc(&hgc_probe_hits);
  }

  HGCRegion* to = _heap->region(cast_from_oop<HeapWord*>(obj));
  if (to == nullptr) {

    return;
  }
  if (to->state() == HGCRegion::Reclaiming) {

    _heap->mark_object_concurrently(obj);
  }
  if (HGCInSameRegion(p, obj)) {
    Atomic::inc(&hgc_refine_same);
    return;
  }
  HGCRegion* target_region = to->is_large() ? to->large_owner() : to;
  HGCHeapRegionRemSet* to_rem_set = target_region != nullptr ? target_region->rem_set() : nullptr;
  if (to_rem_set == nullptr || !to_rem_set->is_tracked()) {
    return;
  }
  Atomic::inc(&hgc_refine_added);
  to_rem_set->add_reference(p, _worker_id);
}

template <class T>
inline void HGCRebuildRemSetClosure::do_oop_work(T* p) {
  oop const obj = RawAccess<MO_RELAXED>::oop_load(p);
  if (obj == nullptr) {
    return;
  }

  if (HGCInSameRegion(p, obj)) {
    return;
  }

  HGCRegion* to = _heap->region(cast_from_oop<HeapWord*>(obj));
  if (to == nullptr) {
    return;
  }
  HGCHeapRegionRemSet* rem_set = to->rem_set();
  if (rem_set != nullptr && rem_set->is_tracked()) {
    rem_set->add_reference(p, _worker_id);
  }
}

#endif
