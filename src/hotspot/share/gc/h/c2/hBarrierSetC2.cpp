#include "gc/h/c2/hBarrierSetC2.hpp"

#include "gc/h/hBarrierSetRuntime.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "opto/castnode.hpp"
#include "opto/compile.hpp"
#include "opto/connode.hpp"
#include "opto/graphKit.hpp"
#include "opto/idealKit.hpp"
#include "opto/memnode.hpp"
#include "opto/narrowptrnode.hpp"
#include "opto/type.hpp"

const TypeFunc* HGCBarrierSetC2::pre_type() {
  const Type** fields = TypeTuple::fields(2);
  fields[TypeFunc::Parms + 0] = TypeRawPtr::BOTTOM;
  fields[TypeFunc::Parms + 1] = TypeRawPtr::BOTTOM;
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 2, fields);

  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}

const TypeFunc* HGCBarrierSetC2::pre_val_type() {
  const Type** fields = TypeTuple::fields(2);
  fields[TypeFunc::Parms + 0] = TypeOopPtr::BOTTOM;
  fields[TypeFunc::Parms + 1] = TypeRawPtr::BOTTOM;
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 2, fields);
  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}

void HGCBarrierSetC2::pre_barrier(GraphKit* kit, Node* adr, uint adr_idx,
                                  const TypeOopPtr* val_type, bool use_precise) const {
  if (!HGCUsePreBarrier) {
    return;
  }
  (void)use_precise;
  assert(adr != nullptr, "pre-barrier address is required");
  assert(val_type != nullptr, "old value type is required");

  IdealKit ideal(kit, true);
#define __ ideal.
  Node* tls = __ thread();
  Node* no_base = __ top();
  Node* zero = __ ConI(0);
  Node* zeroX = __ ConX(0);
  float unlikely = PROB_UNLIKELY(0.999);

  const int active_offset = in_bytes(HGCThreadLocalData::satb_mark_queue_active_offset());
  const int index_offset = in_bytes(HGCThreadLocalData::satb_mark_queue_index_offset());
  const int buffer_offset = in_bytes(HGCThreadLocalData::satb_mark_queue_buffer_offset());

  BasicType flag_type = in_bytes(SATBMarkQueue::byte_width_of_active()) == 4 ? T_INT : T_BYTE;
  Node* active_addr = __ AddP(no_base, tls, __ ConX(active_offset));
  Node* active = __ load(__ ctrl(), active_addr, TypeInt::BYTE, flag_type, Compile::AliasIdxRaw);
  __ if_then(active, BoolTest::ne, zero, unlikely); {
    Node* index_addr = __ AddP(no_base, tls, __ ConX(index_offset));
    BasicType index_bt = TypeX_X->basic_type();
    Node* index = __ load(__ ctrl(), index_addr, TypeX_X, index_bt, Compile::AliasIdxRaw);
    __ if_then(index, BoolTest::ne, zeroX, PROB_LIKELY(0.999)); {
      Node* pre_val = __ load(__ ctrl(), adr, val_type, T_OBJECT, adr_idx);
      __ if_then(pre_val, BoolTest::ne, kit->null()); {
        Node* next_index = kit->gvn().transform(new SubXNode(index, __ ConX(sizeof(intptr_t))));
        Node* buffer_addr = __ AddP(no_base, tls, __ ConX(buffer_offset));
        Node* buffer = __ load(__ ctrl(), buffer_addr, TypeRawPtr::NOTNULL, T_ADDRESS, Compile::AliasIdxRaw);
        Node* log_addr = __ AddP(no_base, buffer, next_index);
        __ store(__ ctrl(), log_addr, pre_val, T_OBJECT, Compile::AliasIdxRaw, MemNode::unordered);
        __ store(__ ctrl(), index_addr, next_index, index_bt, Compile::AliasIdxRaw, MemNode::unordered);
      } __ end_if();
    } __ else_(); {
      __ make_leaf_call(pre_type(),
                        CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_pre_addr_entry),
                        "hgc_pre_barrier", adr, tls);
    } __ end_if();
  } __ end_if();
#undef __
  kit->final_sync(ideal);
}

const TypeFunc* HGCBarrierSetC2::post_type() {
  const Type** fields = TypeTuple::fields(1);
  fields[TypeFunc::Parms + 0] = TypeRawPtr::BOTTOM;
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 1, fields);
  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}

static void emit_post_barrier_call(IdealKit& ideal, const TypeFunc* post_ty, Node* addr) {
  ideal.make_leaf_call(post_ty,
                       CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_post_addr_entry),
                       "hgc_post_barrier", addr);
}

static void emit_young_card_check(IdealKit& ideal, GraphKit* kit,
                                  IdealVariable& call_runtime,
                                  Node* ptr_x, Node* next_x) {
  HGCCollectedHeap* heap = HGCCollectedHeap::heap();
  assert(heap != nullptr, "the heap is initialized before C2 compiles");

  Node* zero = ideal.ConX(0);
  Node* start_addr = kit->makecon(TypeRawPtr::make(heap->c2_region_table_start_addr()));
  Node* count_addr = kit->makecon(TypeRawPtr::make(heap->c2_region_table_count_addr()));
  Node* regions_addr = kit->makecon(TypeRawPtr::make(heap->c2_region_table_base_addr()));

  Node* start = ideal.load(ideal.ctrl(), start_addr, TypeX_X, T_LONG, Compile::AliasIdxRaw);
  Node* count = ideal.load(ideal.ctrl(), count_addr, TypeX_X, T_LONG, Compile::AliasIdxRaw);
  Node* diff = kit->gvn().transform(new SubXNode(ptr_x, start));
  Node* index = ideal.URShiftX(diff, ideal.ConI(log2i(HGCRegionSize)));

  ideal.if_then(index, BoolTest::lt, count, PROB_LIKELY(0.999)); {
    Node* byte_offset = kit->gvn().transform(new LShiftXNode(index, ideal.ConI(LogBytesPerWord)));
    Node* region = ideal.load(ideal.ctrl(),
                              ideal.AddP(ideal.top(), regions_addr, byte_offset),
                              TypeX_X, T_LONG, Compile::AliasIdxRaw);
    ideal.if_then(region, BoolTest::ne, zero); {
      Node* region_ptr = kit->gvn().transform(new CastX2PNode(region));
      Node* large_owner = ideal.load(ideal.ctrl(),
                                     ideal.AddP(ideal.top(), region_ptr,
                                                ideal.ConX(HGCRegion::c2_large_owner_offset_in_bytes())),
                                     TypeX_X, T_LONG, Compile::AliasIdxRaw);
      Node* state = ideal.load(ideal.ctrl(),
                               ideal.AddP(ideal.top(), region_ptr,
                                          ideal.ConX(HGCRegion::c2_state_offset_in_bytes())),
                               TypeInt::BYTE, T_BYTE, Compile::AliasIdxRaw);
      ideal.if_then(large_owner, BoolTest::ne, zero, PROB_UNLIKELY(0.999)); {
        ideal.set(call_runtime, ideal.ConI(1));
      } ideal.end_if();
      ideal.if_then(state, BoolTest::eq, ideal.ConI((jint)HGCRegion::Reclaiming),
                    PROB_UNLIKELY(0.999)); {
        ideal.set(call_runtime, ideal.ConI(1));
      } ideal.end_if();
      if (next_x != nullptr) {
        emit_young_card_check(ideal, kit, call_runtime, next_x, nullptr);
      }
    } ideal.else_(); {
      ideal.set(call_runtime, ideal.ConI(1));
    } ideal.end_if();
  } ideal.else_(); {
    ideal.set(call_runtime, ideal.ConI(1));
  } ideal.end_if();
}

void HGCBarrierSetC2::post_barrier(GraphKit* kit,
                                   Node* obj,
                                   Node* adr,
                                   Node* val,
                                   bool use_precise) const {
  (void)obj;
  (void)use_precise;
  if (!HGCUsePostBarrier) {
    return;
  }

  if (val != nullptr && val->is_Con()) {
    const Type* t = val->bottom_type();
    if (t == TypePtr::NULL_PTR || t == Type::TOP) {
      return;
    }
  }

  Node* addr = adr;
  assert(addr != nullptr, "post-barrier address is required");

  Node* new_val = val;
  if (new_val != nullptr && UseCompressedOops &&
      new_val->bottom_type()->isa_narrowoop()) {
    new_val = kit->gvn().transform(new DecodeNNode(new_val, TypePtr::BOTTOM));
  }

  const TypeFunc* post_ty = post_type();
  IdealKit ideal(kit, true, true);
  IdealVariable call_runtime(ideal);
#define __ ideal.
  __ declarations_done();
  __ set(call_runtime, __ ConI(0));

  Node* cast = __ CastPX(__ ctrl(), addr);
  if (new_val == nullptr) {

    __ set(call_runtime, __ ConI(1));
  } else {
    Node* new_val_cast = __ CastPX(__ ctrl(), new_val);
    Node* xor_addr = __ XorX(cast, new_val_cast);
    Node* region_shift = __ URShiftX(xor_addr, __ ConI(log2i(HGCRegionSize)));

    __ if_then(region_shift, BoolTest::ne, __ ConX(0), PROB_LIKELY(0.9)); {
      Node* card_offset = __ URShiftX(cast, __ ConI(CardTable::card_shift()));
      Node* card_addr = __ AddP(__ top(), byte_map_base_node(kit), card_offset);
      Node* card_val = __ load(__ ctrl(), card_addr, TypeInt::BYTE, T_BYTE, Compile::AliasIdxRaw);
      Node* dirty = __ ConI(HGCCardTable::dirty_card_val());
      Node* mixed_dirty = __ ConI(HGCCardTable::mixed_dirty_card_val());
      Node* young = __ ConI(HGCCardTable::young_card_val());

      __ if_then(card_val, BoolTest::eq, dirty, PROB_LIKELY(0.99)); {
      } __ else_(); {
        __ if_then(card_val, BoolTest::eq, mixed_dirty, PROB_LIKELY(0.99)); {
        } __ else_(); {
          __ if_then(card_val, BoolTest::eq, young, PROB_LIKELY(0.9)); {

            emit_young_card_check(ideal, kit, call_runtime, cast, new_val_cast);
          } __ else_(); {

            __ set(call_runtime, __ ConI(1));
          } __ end_if();
        } __ end_if();
      } __ end_if();
    } __ end_if();
  }

  __ if_then(__ value(call_runtime), BoolTest::ne, __ ConI(0), PROB_UNLIKELY(0.999)); {
    emit_post_barrier_call(ideal, post_ty, addr);
  } __ end_if();
#undef __
  kit->final_sync(ideal);
}

Node* HGCBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  DecoratorSet decorators = access.decorators();
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  bool is_array = (decorators & IS_ARRAY) != 0;
  bool tightly_coupled = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;
  bool skip_pre = (decorators & (IS_DEST_UNINITIALIZED | AS_NO_KEEPALIVE)) != 0;

  if (access.is_oop() && !tightly_coupled && (in_heap || anonymous) && !skip_pre &&
      HGCUsePreBarrier) {
    assert(access.is_parse_access(), "entry not supported at optimization time");
    C2ParseAccess& parse_access = static_cast<C2ParseAccess&>(access);
    GraphKit* kit = parse_access.kit();
    uint adr_idx = kit->C->get_alias_index(access.addr().type());
    const TypeOopPtr* val_type = static_cast<const TypeOopPtr*>(val.type());
    pre_barrier(kit, access.addr().node(), adr_idx, val_type, is_array || anonymous);
  }

  return ModRefBarrierSetC2::store_at_resolved(access, val);
}
