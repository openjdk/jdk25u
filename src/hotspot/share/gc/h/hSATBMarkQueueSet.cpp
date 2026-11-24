#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hSATBMarkQueueSet.hpp"
#include "gc/h/hThreadLocalData.hpp"

HGCSATBMarkQueueSet::HGCSATBMarkQueueSet(BufferNode::Allocator* allocator) :
  SATBMarkQueueSet(allocator) {}

SATBMarkQueue& HGCSATBMarkQueueSet::satb_queue_for_thread(Thread* const t) const {
  return HGCThreadLocalData::satb_mark_queue(t);
}

void HGCSATBMarkQueueSet::filter(SATBMarkQueue& queue) {
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  apply_filter([heap](const void* entry) {
    oop obj = cast_to_oop(entry);
    HeapWord* addr = cast_from_oop<HeapWord*>(obj);
    HGCRegion* region = heap->region(addr);
    if (region == nullptr) {

      return true;
    }
    if (region->state() != HGCRegion::Reclaiming) {
      return true;
    }
    if (region->is_large()) {

      return region->large_owner()->is_large_marked();
    }
    HGCArena* arena = region->arena(addr);
    assert(arena != nullptr, "object must be in an arena");
    return arena->is_gray(addr);
  }, queue);
}
