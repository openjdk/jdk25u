#ifndef CPU_X86_GC_H_HBARRIERSETASSEMBLER_X86_HPP
#define CPU_X86_GC_H_HBARRIERSETASSEMBLER_X86_HPP

#include "asm/macroAssembler.hpp"
#include "gc/shared/cardTableBarrierSetAssembler.hpp"

class LIR_Assembler;
class HGCPreBarrierStub;
class StubAssembler;

class HGCBarrierSetAssembler: public CardTableBarrierSetAssembler {
  void write_barrier_pre(MacroAssembler* masm,
                         Register obj,
                         Register pre_val,
                         Register tmp,
                         bool tosca_live,
                         bool expand_call);

  void write_barrier_post(MacroAssembler* masm,
                          Register store_addr,
                          Register tmp,
                          Register tmp2);

protected:
  virtual void gen_write_ref_array_pre_barrier(MacroAssembler* masm, DecoratorSet decorators, Register addr, Register count) override;

  virtual void gen_write_ref_array_post_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                Register addr, Register count, Register tmp) override;

  virtual void oop_store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                            Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) override;

public:
  virtual void load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                       Register dst, Address src, Register tmp1) override;

  void gen_pre_barrier_stub(LIR_Assembler* ce, HGCPreBarrierStub* stub);
  void generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm);
};

#endif
