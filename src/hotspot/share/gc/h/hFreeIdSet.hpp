#ifndef SHARE_GC_H_HFREEIDSET_HPP
#define SHARE_GC_H_HFREEIDSET_HPP

#include "runtime/semaphore.hpp"
#include "utilities/globalDefinitions.hpp"

class HGCFreeIdSet {
  Semaphore _sem;
  uint* _next;
  uint _start;
  uint _size;
  uintx _head_index_mask;
  volatile uintx _head;

  uint head_index(uintx head) const;
  uintx make_head(uint index, uintx old_head) const;

  NONCOPYABLE(HGCFreeIdSet);

public:
  HGCFreeIdSet(uint start, uint size);
  ~HGCFreeIdSet();

  uint claim_par_id();

  void release_par_id(uint id);

  struct TestSupport;
};

#endif
