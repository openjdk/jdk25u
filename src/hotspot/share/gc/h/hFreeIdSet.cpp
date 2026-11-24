#include "gc/h/hFreeIdSet.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/atomic.hpp"
#include "utilities/checkedCast.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/powerOfTwo.hpp"

HGCFreeIdSet::HGCFreeIdSet(uint start, uint size) :
  _sem(size),
  _next(nullptr),
  _start(start),
  _size(size),
  _head_index_mask(0),
  _head(0)
{
  assert(size != 0, "precondition");
  assert(start <= (UINT_MAX - size),
         "start (%u) + size (%u) overflow: ", start, size);
  uint shift = log2i(size) + 1;
  assert(shift <= (BitsPerWord / 2), "excessive size %u", size);
  _head_index_mask = (uintx(1) << shift) - 1;
  assert(size <= _head_index_mask, "invariant");
  _next = NEW_C_HEAP_ARRAY(uint, size, mtGC);
  for (uint i = 0; i < size; ++i) {
    _next[i] = i + 1;
  }
}

HGCFreeIdSet::~HGCFreeIdSet() {
  FREE_C_HEAP_ARRAY(uint, _next);
}

uint HGCFreeIdSet::head_index(uintx head) const {
  return checked_cast<uint>(head & _head_index_mask);
}

uintx HGCFreeIdSet::make_head(uint index, uintx old_head) const {
  return index | ((old_head & ~_head_index_mask) + 1 + _head_index_mask);
}

#ifdef ASSERT
const uint Claimed = UINT_MAX;
#endif

uint HGCFreeIdSet::claim_par_id() {
  _sem.wait();
  uintx old_head = Atomic::load(&_head);
  uint index;
  while (true) {
    index = head_index(old_head);
    assert(index < _size, "invariant");
    uintx new_head = make_head(_next[index], old_head);
    new_head = Atomic::cmpxchg(&_head, old_head, new_head);
    if (new_head == old_head) break;
    old_head = new_head;
  }
  DEBUG_ONLY(_next[index] = Claimed;)
  return _start + index;
}

void HGCFreeIdSet::release_par_id(uint id) {
  uint index = id - _start;
  assert(index < _size, "invalid id %u", id);
  assert(_next[index] == Claimed, "precondition");
  uintx old_head = Atomic::load(&_head);
  while (true) {
    _next[index] = head_index(old_head);
    uintx new_head = make_head(index, old_head);
    new_head = Atomic::cmpxchg(&_head, old_head, new_head);
    if (new_head == old_head) break;
    old_head = new_head;
  }
  _sem.signal();
}
