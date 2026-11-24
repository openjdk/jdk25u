#include "c1/c1_CodeStubs.hpp"
#include "c1/c1_Compilation.hpp"
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_LIRGenerator.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "c1/c1_Runtime1.hpp"
#include "gc/h/c1/hBarrierSetC1.hpp"
#include "gc/h/hBarrierSet.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/h/hBarrierSetAssembler.hpp"
#include "gc/h/hBarrierSetRuntime.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "gc/shared/cardTable.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "utilities/macros.hpp"

#ifdef ASSERT
#define __ gen->lir(__FILE__, __LINE__)->
#else
#define __ gen->lir()->
#endif

void HGCPreBarrierStub::emit_code(LIR_Assembler* ce) {
  HGCBarrierSetAssembler* bs =
      (HGCBarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
  bs->gen_pre_barrier_stub(ce, this);
}

void HGCBarrierSetC1::pre_barrier(LIRAccess& access, LIR_Opr addr_opr,
                                  LIR_Opr pre_val, CodeEmitInfo* info) {
  if (!HGCUsePreBarrier) return;
  DecoratorSet decorators = access.decorators();
  if ((decorators & (IS_DEST_UNINITIALIZED | AS_NO_KEEPALIVE)) != 0) {
    return;
  }

  LIRGenerator* gen = access.gen();
  const BasicType flag_type =
      in_bytes(SATBMarkQueue::byte_width_of_active()) == 4 ? T_INT : T_BOOLEAN;
  LIR_Opr active = gen->new_register(T_INT);
  LIR_Address* active_addr =
      new LIR_Address(gen->getThreadPointer(),
                      in_bytes(HGCThreadLocalData::satb_mark_queue_active_offset()),
                      flag_type);
  __ load(active_addr, active);
  __ cmp(lir_cond_notEqual, active, LIR_OprFact::intConst(0));

  bool do_load = pre_val == LIR_OprFact::illegalOpr;
  CodeStub* stub;
  if (do_load) {
    assert(addr_opr != LIR_OprFact::illegalOpr, "pre-barrier address is required");
    LIR_Opr loaded = gen->new_register(T_OBJECT);
    LIR_PatchCode patch_code = (decorators & C1_NEEDS_PATCHING) != 0
                                   ? lir_patch_normal
                                   : lir_patch_none;
    if (!addr_opr->is_address()) {
      addr_opr = LIR_OprFact::address(new LIR_Address(addr_opr, T_OBJECT));
    }
    stub = new HGCPreBarrierStub(addr_opr, loaded, patch_code, info);
    pre_val = loaded;
  } else {
    assert(addr_opr == LIR_OprFact::illegalOpr, "sanity");
    assert(pre_val->is_register(), "pre-value must be a register");
    assert(pre_val->type() == T_OBJECT, "pre-value must be an object");
    stub = new HGCPreBarrierStub(pre_val);
  }

  __ branch(lir_cond_notEqual, stub);
  __ branch_destination(stub->continuation());
}

void HGCBarrierSetC1::store_at_resolved(LIRAccess& access, LIR_Opr value) {
  BarrierSetC1::store_at_resolved(access, value);
  if (!access.is_oop()) {
    return;
  }

  LIR_Opr post_addr = access.resolved_addr();
  if ((access.decorators() & C1_NEEDS_PATCHING) != 0 ||
      (post_addr->is_address() &&
       post_addr->as_address_ptr()->disp() == max_jint)) {
    post_addr = access.base().opr();
  }
  post_barrier(access, post_addr, value);
}

void HGCBarrierSetC1::post_barrier(LIRAccess& access, LIR_Opr addr, LIR_Opr new_val) {
  if (!HGCUsePostBarrier) return;
  DecoratorSet decorators = access.decorators();
  if ((decorators & IN_HEAP) == 0) {
    return;
  }
  if (new_val->is_constant() &&
      new_val->as_constant_ptr()->as_jobject() == nullptr) {
    return;
  }

  LIRGenerator* gen = access.gen();
  if (addr->is_address()) {
    LIR_Address* address = addr->as_address_ptr();
    LIR_Opr ptr = gen->new_pointer_register();
    if (!address->index()->is_valid() && address->disp() == 0) {
      __ move(address->base(), ptr);
    } else {
      assert(address->disp() != max_jint, "lea doesn't support patched addresses!");
      __ leal(addr, ptr);
    }
    addr = ptr;
  }
  assert(addr->is_register(), "must be a register at this point");

  BasicTypeList signature;
  signature.append(T_ADDRESS);
  LIR_OprList args;
  args.append(addr);
  gen->call_runtime(&signature, &args,
                    CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_post_addr_entry),
                    voidType, nullptr);
}

class C1HGCPreBarrierCodeGenClosure : public StubAssemblerCodeGenClosure {
  OopMapSet* generate_code(StubAssembler* sasm) override {
    HGCBarrierSetAssembler* bs =
        (HGCBarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
    bs->generate_c1_pre_barrier_runtime_stub(sasm);
    return nullptr;
  }
};

bool HGCBarrierSetC1::generate_c1_runtime_stubs(BufferBlob* buffer_blob) {
  C1HGCPreBarrierCodeGenClosure pre_code_gen;
  _pre_barrier_c1_runtime_code_blob =
      Runtime1::generate_blob(buffer_blob, C1StubId::NO_STUBID, "hgc_pre_barrier_slow",
                              false, &pre_code_gen);
  return _pre_barrier_c1_runtime_code_blob != nullptr;
}
