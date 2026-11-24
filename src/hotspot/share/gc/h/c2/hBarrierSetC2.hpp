#ifndef SHARE_GC_H_C2_HBARRIERSETC2_HPP
#define SHARE_GC_H_C2_HBARRIERSETC2_HPP

#include "gc/shared/c2/cardTableBarrierSetC2.hpp"

class TypeFunc;

class HGCBarrierSetC2 : public CardTableBarrierSetC2 {
protected:
  Node* store_at_resolved(C2Access& access, C2AccessValue& val) const override;
  void post_barrier(GraphKit* kit,
                    Node* obj,
                    Node* adr,
                    Node* val,
                    bool use_precise) const override;

private:
  void pre_barrier(GraphKit* kit, Node* adr, uint adr_idx,
                   const TypeOopPtr* val_type, bool use_precise) const;
  static const TypeFunc* pre_type();
  static const TypeFunc* pre_val_type();
  static const TypeFunc* post_type();
};

#endif
