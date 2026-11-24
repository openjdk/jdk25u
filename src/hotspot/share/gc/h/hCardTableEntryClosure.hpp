#ifndef SHARE_GC_H_HCARDTABLEENTRYCLOSURE_HPP
#define SHARE_GC_H_HCARDTABLEENTRYCLOSURE_HPP

#include "gc/shared/bufferNode.hpp"
#include "gc/shared/cardTable.hpp"
#include "memory/allocation.hpp"

class HGCCardTableEntryClosure : public CHeapObj<mtGC> {
public:
  typedef CardTable::CardValue CardValue;

  virtual void do_card_ptr(CardValue* card_ptr) = 0;

  void apply_to_buffer(BufferNode* node, uint worker_id) {
    void** buffer = BufferNode::make_buffer_from_node(node);
    size_t capacity = node->capacity();
    for (size_t i = node->index(); i < capacity; ++i) {
      CardValue* card_ptr = static_cast<CardValue*>(buffer[i]);
      do_card_ptr(card_ptr);
    }
  }
};

#endif
