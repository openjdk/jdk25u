#include "gc/h/hBarrierSet.inline.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/h/hBarrierSetRuntime.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/macros.hpp"

void HGCBarrierSetRuntime::write_ref_array_pre_oop_entry(oop* dst, size_t length) {
  HGCBarrierSet* bs = barrier_set_cast<HGCBarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_array_pre(dst, length, false);
}

void HGCBarrierSetRuntime::write_ref_array_pre_narrow_oop_entry(narrowOop* dst, size_t length) {
  HGCBarrierSet* bs = barrier_set_cast<HGCBarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_array_pre(dst, length, false);
}

#if defined(__GNUC__) && defined(__x86_64__)
#define HGC_PRESERVE_CALLER_SAVED \
  __attribute__((no_caller_saved_registers, target("general-regs-only")))
#else
#define HGC_PRESERVE_CALLER_SAVED
#endif

JRT_LEAF(void, HGCBarrierSetRuntime::write_ref_field_pre_entry(oopDesc* orig, JavaThread* thread))
  assert(thread == JavaThread::current(), "pre-condition");
  if (!HGCUsePreBarrier) return;
  if (orig == nullptr) return;
  if (!HGCBarrierSet::satb_mark_queue_set().is_active()) return;
  SATBMarkQueue& queue = HGCThreadLocalData::satb_mark_queue(thread);
  HGCBarrierSet::satb_mark_queue_set().enqueue_known_active(queue, orig);
JRT_END

HGC_PRESERVE_CALLER_SAVED
JRT_LEAF(void, HGCBarrierSetRuntime::write_ref_field_pre_addr_entry(void* addr, JavaThread* thread))
  assert(thread == JavaThread::current(), "pre-condition");
  if (!HGCUsePreBarrier) return;
  SATBMarkQueue& queue = HGCThreadLocalData::satb_mark_queue(thread);
  if (!queue.is_active()) return;
  if (!HGCBarrierSet::satb_mark_queue_set().is_active()) return;

  oop pre_val = nullptr;
  if (UseCompressedOops) {
    narrowOop value = RawAccess<MO_RELAXED>::oop_load(reinterpret_cast<narrowOop*>(addr));
    if (value != narrowOop::null) {
      pre_val = CompressedOops::decode_not_null(value);
    }
  } else {
    pre_val = RawAccess<MO_RELAXED>::oop_load(reinterpret_cast<oop*>(addr));
  }
  if (pre_val == nullptr) return;
  HGCBarrierSet::satb_mark_queue_set().enqueue_known_active(queue, pre_val);
JRT_END

HGC_PRESERVE_CALLER_SAVED
JRT_LEAF(void, HGCBarrierSetRuntime::write_ref_field_post_addr_entry(void* addr))
  HGCBarrierSet::post_barrier(addr);
JRT_END

JRT_LEAF(void, HGCBarrierSetRuntime::write_ref_field_post_entry(volatile CardTable::CardValue* card, JavaThread* thread))
  assert(thread == JavaThread::current(), "pre-condition");
  OrderAccess::storeload();
  HGCBarrierSet::dirty_card(card);
JRT_END

JRT_LEAF(void, HGCBarrierSetRuntime::write_ref_array_post_entry(HeapWord* dst,
                                                                size_t length,
                                                                JavaThread* thread))
  assert(thread == JavaThread::current(), "pre-condition");
  OrderAccess::release();
  HGCBarrierSet* bs = barrier_set_cast<HGCBarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_array(dst, length);
JRT_END
