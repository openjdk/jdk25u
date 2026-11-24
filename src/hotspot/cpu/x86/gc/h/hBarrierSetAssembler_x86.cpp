#include "asm/macroAssembler.inline.hpp"
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/h/c1/hBarrierSetC1.hpp"
#include "gc/h/hBarrierSet.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/h/hBarrierSetAssembler.hpp"
#include "gc/h/hBarrierSetRuntime.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/debug.hpp"
#include "utilities/macros.hpp"

#define __ masm->

#define TIMES_OOP (UseCompressedOops ? Address::times_4 : Address::times_8)

void HGCBarrierSetAssembler::gen_write_ref_array_pre_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                             Register addr, Register count) {
  bool dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;

  if (!dest_uninitialized) {
    Register thread = r15_thread;

    Label filtered;
    Address in_progress(thread, in_bytes(HGCThreadLocalData::satb_mark_queue_active_offset()));

    if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
      __ cmpl(in_progress, 0);
    } else {
      assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
      __ cmpb(in_progress, 0);
    }

    __ jcc(Assembler::equal, filtered);

    __ push_call_clobbered_registers(false );
    if (count == c_rarg0) {
      if (addr == c_rarg1) {

        __ xchgptr(c_rarg1, c_rarg0);
      } else {
        __ movptr(c_rarg1, count);
        __ movptr(c_rarg0, addr);
      }
    } else {
      __ movptr(c_rarg0, addr);
      __ movptr(c_rarg1, count);
    }
    if (UseCompressedOops) {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_array_pre_narrow_oop_entry), 2);
    } else {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_array_pre_oop_entry), 2);
    }
    __ pop_call_clobbered_registers(false );

    __ bind(filtered);
  }
}

static void generate_queue_insertion(MacroAssembler* masm, ByteSize index_offset, ByteSize buffer_offset, Label& runtime,
                                     const Register thread, const Register value, const Register temp) {

  STATIC_ASSERT(in_bytes(SATBMarkQueue::byte_width_of_index()) == sizeof(intptr_t));

  __ movptr(temp, Address(thread, in_bytes(index_offset)));
  __ testptr(temp, temp);
  __ jcc(Assembler::zero, runtime);

  __ subptr(temp, wordSize);
  __ movptr(Address(thread, in_bytes(index_offset)), temp);
  __ addptr(temp, Address(thread, in_bytes(buffer_offset)));
  __ movptr(Address(temp, 0), value);
}

static void generate_pre_barrier_fast_path(MacroAssembler* masm,
                                           const Register thread) {
  Address in_progress(thread, in_bytes(HGCThreadLocalData::satb_mark_queue_active_offset()));

  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ cmpl(in_progress, 0);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ cmpb(in_progress, 0);
  }
}

static void generate_pre_barrier_slow_path(MacroAssembler* masm,
                                           const Register obj,
                                           const Register pre_val,
                                           const Register thread,
                                           const Register tmp,
                                           Label& done,
                                           Label& runtime) {

  if (obj != noreg) {
    __ load_heap_oop(pre_val, Address(obj, 0), noreg, AS_RAW);
  }

  __ cmpptr(pre_val, NULL_WORD);
  __ jcc(Assembler::equal, done);
  generate_queue_insertion(masm,
                           HGCThreadLocalData::satb_mark_queue_index_offset(),
                           HGCThreadLocalData::satb_mark_queue_buffer_offset(),
                           runtime,
                           thread, pre_val, tmp);
  __ jmp(done);
}

void HGCBarrierSetAssembler::write_barrier_pre(MacroAssembler* masm,
                                               Register obj,
                                               Register pre_val,
                                               Register tmp,
                                               bool tosca_live,
                                               bool expand_call) {
  if (!HGCUsePreBarrier) {
    return;
  }

  const Register thread = r15_thread;

  Label done;
  Label runtime;

  assert(pre_val != noreg, "check this code");

  if (obj != noreg) {
    assert_different_registers(obj, pre_val, tmp);
    assert(pre_val != rax, "check this code");
  }

  generate_pre_barrier_fast_path(masm, thread);

  __ jcc(Assembler::equal, done);
  generate_pre_barrier_slow_path(masm, obj, pre_val, thread, tmp, done, runtime);

  __ bind(runtime);

  __ push_call_clobbered_registers();

  if (expand_call) {
    assert(pre_val != c_rarg1, "smashed arg");
    if (c_rarg1 != thread) {
      __ mov(c_rarg1, thread);
    }
    if (c_rarg0 != pre_val) {
      __ mov(c_rarg0, pre_val);
    }
    __ MacroAssembler::call_VM_leaf_base(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_pre_entry), 2);
  } else {
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
  }

  __ pop_call_clobbered_registers();

  __ bind(done);
}

void HGCBarrierSetAssembler::write_barrier_post(MacroAssembler* masm,
                                                Register store_addr,
                                                Register tmp,
                                                Register tmp2) {
  if (!HGCUsePostBarrier) {
    return;
  }
  BarrierSet* bs = BarrierSet::barrier_set();
  CardTableBarrierSet* ctbs = barrier_set_cast<CardTableBarrierSet>(bs);
  CardTable* ct = ctbs->card_table();

  Label done;
  Label young;

  __ movptr(tmp, store_addr);
  __ shrptr(tmp, CardTable::card_shift());
  __ mov64(tmp2, (int64_t)(intptr_t)ct->byte_map_base());
  __ addptr(tmp, tmp2);

  __ cmpb(Address(tmp, 0), HGCCardTable::young_card_val());
  __ jcc(Assembler::equal, young);
  __ cmpb(Address(tmp, 0), HGCCardTable::mixed_young_card_val());
  __ jcc(Assembler::equal, young);

  __ cmpb(Address(tmp, 0), HGCCardTable::dirty_card_val());
  __ jcc(Assembler::equal, done);
  __ cmpb(Address(tmp, 0), HGCCardTable::mixed_dirty_card_val());
  __ jcc(Assembler::equal, done);

  __ push_call_clobbered_registers(false );
  if (c_rarg0 != tmp) {
    __ movptr(c_rarg0, tmp);
  }
  if (c_rarg1 != r15_thread) {
    __ movptr(c_rarg1, r15_thread);
  }
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_post_entry), 2);
  __ pop_call_clobbered_registers(false );
  __ jmp(done);

  __ bind(young);
  __ push_call_clobbered_registers(false );
  if (c_rarg0 != store_addr) {
    __ movptr(c_rarg0, store_addr);
  }
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_post_addr_entry), 1);
  __ pop_call_clobbered_registers(false );

  __ bind(done);
}

void HGCBarrierSetAssembler::gen_write_ref_array_post_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                              Register addr, Register count, Register tmp) {
  Label done;

  __ testl(count, count);
  __ jcc(Assembler::zero, done);

  __ push_call_clobbered_registers(false );
  __ movptr(c_rarg0, addr);
  __ movptr(c_rarg1, count);
  __ movptr(c_rarg2, r15_thread);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_array_post_entry), 3);
  __ pop_call_clobbered_registers(false );

  __ bind(done);
}

void HGCBarrierSetAssembler::oop_store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                          Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) {
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool as_normal = (decorators & AS_NORMAL) != 0;

  bool needs_pre_barrier = as_normal;
  bool needs_post_barrier = (val != noreg) && in_heap;

  if (dst.index() == noreg && dst.disp() == 0) {
    if (dst.base() != tmp1) {
      __ movptr(tmp1, dst.base());
    }
  } else {
    __ lea(tmp1, dst);
  }

  if (needs_pre_barrier) {
    write_barrier_pre(masm ,
                      tmp1 ,
                      tmp2 ,
                      tmp3 ,
                      val != noreg ,
                      false );
  }

  BarrierSetAssembler::store_at(masm, decorators, type, Address(tmp1, 0), val, noreg, noreg, noreg);

  if (needs_post_barrier) {
    write_barrier_post(masm ,
                       tmp1 ,
                       tmp2 ,
                       tmp3 );
  }
}

void HGCBarrierSetAssembler::gen_pre_barrier_stub(LIR_Assembler* ce, HGCPreBarrierStub* stub) {
  HGCBarrierSetC1* bs = (HGCBarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  C1_MacroAssembler* masm = ce->masm();

  masm->bind(*stub->entry());

  if (stub->do_load()) {
    ce->mem2reg(stub->addr(), stub->pre_val(), T_OBJECT, stub->patch_code(), stub->info(), false);
  }

  Register pre_val = stub->pre_val()->as_register();
  masm->cmpptr(pre_val, NULL_WORD);
  masm->jcc(Assembler::equal, *stub->continuation());
  ce->store_parameter(pre_val, 0);
  masm->call(RuntimeAddress(bs->pre_barrier_c1_runtime_code_blob()->code_begin()));
  masm->jmp(*stub->continuation());
}

void HGCBarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  StubAssembler* masm = sasm;

  STATIC_ASSERT(in_bytes(SATBMarkQueue::byte_width_of_index()) == sizeof(intptr_t));

  __ prologue("hgc_pre_barrier", false);

  __ push(rax);
  __ push(rdx);

  const Register pre_val = rax;
  const Register thread = r15_thread;
  const Register tmp = rdx;

  Address queue_active(thread, in_bytes(HGCThreadLocalData::satb_mark_queue_active_offset()));
  Address queue_index(thread, in_bytes(HGCThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(HGCThreadLocalData::satb_mark_queue_buffer_offset()));

  Label done;
  Label runtime;

  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ cmpl(queue_active, 0);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ cmpb(queue_active, 0);
  }
  __ jcc(Assembler::equal, done);

  __ movptr(tmp, queue_index);
  __ testptr(tmp, tmp);
  __ jcc(Assembler::zero, runtime);
  __ subptr(tmp, wordSize);
  __ movptr(queue_index, tmp);
  __ addptr(tmp, buffer);

  __ load_parameter(0, pre_val);
  __ movptr(Address(tmp, 0), pre_val);
  __ jmp(done);

  __ bind(runtime);
  __ push_call_clobbered_registers();
  __ load_parameter(0, rcx);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, HGCBarrierSetRuntime::write_ref_field_pre_entry), rcx, thread);
  __ pop_call_clobbered_registers();

  __ bind(done);
  __ pop(rdx);
  __ pop(rax);
  __ epilogue();
}

void HGCBarrierSetAssembler::load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                     Register dst, Address src, Register tmp1) {
  bool on_oop = is_reference_type(type);
  bool on_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool on_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool on_reference = on_weak || on_phantom;
  CardTableBarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1);
  if (on_oop && on_reference) {
    write_barrier_pre(masm ,
                      noreg ,
                      dst ,
                      tmp1 ,
                      true ,
                      true );
  }
}
