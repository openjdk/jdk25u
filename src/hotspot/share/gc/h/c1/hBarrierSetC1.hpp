#ifndef SHARE_GC_H_C1_HBARRIERSETC1_HPP
#define SHARE_GC_H_C1_HBARRIERSETC1_HPP

#include "c1/c1_CodeStubs.hpp"
#include "c1/c1_Compilation.hpp"
#include "gc/shared/c1/cardTableBarrierSetC1.hpp"

class HGCPreBarrierStub : public CodeStub {
  bool _do_load;
  LIR_Opr _addr;
  LIR_Opr _pre_val;
  LIR_PatchCode _patch_code;
  CodeEmitInfo* _info;

public:
  HGCPreBarrierStub(LIR_Opr addr, LIR_Opr pre_val,
                    LIR_PatchCode patch_code, CodeEmitInfo* info) :
    _do_load(true), _addr(addr), _pre_val(pre_val),
    _patch_code(patch_code), _info(info) {
    assert(_pre_val->is_register(), "should be a temporary register");
    assert(_addr->is_address(), "should be a field address");
    FrameMap* f = Compilation::current()->frame_map();
    f->update_reserved_argument_area_size(2 * BytesPerWord);
  }

  HGCPreBarrierStub(LIR_Opr pre_val) :
    _do_load(false), _addr(LIR_OprFact::illegalOpr), _pre_val(pre_val),
    _patch_code(lir_patch_none), _info(nullptr) {
    assert(_pre_val->is_register(), "should be a register");
    FrameMap* f = Compilation::current()->frame_map();
    f->update_reserved_argument_area_size(2 * BytesPerWord);
  }

  LIR_Opr addr() const { return _addr; }
  LIR_Opr pre_val() const { return _pre_val; }
  LIR_PatchCode patch_code() const { return _patch_code; }
  CodeEmitInfo* info() const { return _info; }
  bool do_load() const { return _do_load; }

  void emit_code(LIR_Assembler* ce) override;
  void visit(LIR_OpVisitState* visitor) override {
    if (_do_load) {
      if (_info != nullptr) {
        visitor->do_slow_case(_info);
      } else {
        visitor->do_slow_case();
      }
      visitor->do_input(_addr);
      visitor->do_temp(_pre_val);
    } else {
      visitor->do_slow_case();
      visitor->do_input(_pre_val);
    }
  }
#ifndef PRODUCT
  void print_name(outputStream* out) const override { out->print("HGCPreBarrierStub"); }
#endif
};

class HGCBarrierSetC1 : public CardTableBarrierSetC1 {
  CodeBlob* _pre_barrier_c1_runtime_code_blob;

protected:
  void pre_barrier(LIRAccess& access, LIR_Opr addr_opr,
                   LIR_Opr pre_val, CodeEmitInfo* info) override;
  void post_barrier(LIRAccess& access, LIR_Opr addr, LIR_Opr new_val) override;
  void store_at_resolved(LIRAccess& access, LIR_Opr value) override;

public:
  HGCBarrierSetC1() : _pre_barrier_c1_runtime_code_blob(nullptr) {}

  CodeBlob* pre_barrier_c1_runtime_code_blob() const {
    return _pre_barrier_c1_runtime_code_blob;
  }

  bool generate_c1_runtime_stubs(BufferBlob* buffer_blob) override;
};

#endif
