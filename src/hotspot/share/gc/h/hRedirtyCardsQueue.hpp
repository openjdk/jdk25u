#ifndef SHARE_GC_H_HREDIRTYCARDSQUEUE_HPP
#define SHARE_GC_H_HREDIRTYCARDSQUEUE_HPP

#include "gc/shared/bufferNode.hpp"
#include "gc/shared/bufferNodeList.hpp"
#include "gc/shared/ptrQueue.hpp"
#include "memory/padded.hpp"
#include "utilities/macros.hpp"

class HGCRedirtyCardsQueueSet;

class HGCRedirtyCardsLocalQueueSet : private PtrQueueSet {
  class Queue : public PtrQueue {
  public:
    Queue(HGCRedirtyCardsLocalQueueSet* qset);
    ~Queue() NOT_DEBUG(= default);
  };

  HGCRedirtyCardsQueueSet* _shared_qset;
  BufferNodeList _buffers;
  Queue _queue;

  virtual void enqueue_completed_buffer(BufferNode* node);

public:
  HGCRedirtyCardsLocalQueueSet(HGCRedirtyCardsQueueSet* shared_qset);
  ~HGCRedirtyCardsLocalQueueSet() NOT_DEBUG(= default);

  void enqueue(void* value);

  BufferNodeList flush();
};

class HGCRedirtyCardsQueueSet : public PtrQueueSet {
  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_PADDING_SIZE, 0);
  BufferNode::Stack _list;
  DEFINE_PAD_MINUS_SIZE(2, DEFAULT_PADDING_SIZE, sizeof(size_t));
  volatile size_t _entry_count;
  DEFINE_PAD_MINUS_SIZE(3, DEFAULT_PADDING_SIZE, sizeof(BufferNode*));
  BufferNode* _tail;
  DEBUG_ONLY(mutable bool _collecting;)

  void update_tail(BufferNode* node);

public:
  HGCRedirtyCardsQueueSet(BufferNode::Allocator* allocator);
  ~HGCRedirtyCardsQueueSet();

  void verify_empty() const NOT_DEBUG_RETURN;

  virtual void enqueue_completed_buffer(BufferNode* node);
  void add_bufferlist(const BufferNodeList& buffers);

  BufferNode* all_completed_buffers() const;
  BufferNodeList take_all_completed_buffers();
};

#endif
