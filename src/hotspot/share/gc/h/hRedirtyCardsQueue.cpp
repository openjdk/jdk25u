#include "gc/h/hRedirtyCardsQueue.hpp"
#include "gc/shared/bufferNode.hpp"
#include "runtime/atomic.hpp"
#include "utilities/macros.hpp"

HGCRedirtyCardsLocalQueueSet::HGCRedirtyCardsLocalQueueSet(HGCRedirtyCardsQueueSet* shared_qset) :
  PtrQueueSet(shared_qset->allocator()),
  _shared_qset(shared_qset),
  _buffers(),
  _queue(this)
{}

#ifdef ASSERT
HGCRedirtyCardsLocalQueueSet::~HGCRedirtyCardsLocalQueueSet() {
  assert(_buffers._head == nullptr, "unflushed qset");
  assert(_buffers._tail == nullptr, "invariant");
  assert(_buffers._entry_count == 0, "invariant");
}
#endif

void HGCRedirtyCardsLocalQueueSet::enqueue_completed_buffer(BufferNode* node) {
  _buffers._entry_count += node->size();
  node->set_next(_buffers._head);
  _buffers._head = node;
  if (_buffers._tail == nullptr) {
    _buffers._tail = node;
  }
}

void HGCRedirtyCardsLocalQueueSet::enqueue(void* value) {
  if (!try_enqueue(_queue, value)) {
    BufferNode* old_node = exchange_buffer_with_new(_queue);
    if (old_node != nullptr) {
      enqueue_completed_buffer(old_node);
    }
    retry_enqueue(_queue, value);
  }
}

BufferNodeList HGCRedirtyCardsLocalQueueSet::flush() {
  flush_queue(_queue);
  BufferNodeList cur_buffers = _buffers;
  _shared_qset->add_bufferlist(_buffers);
  _buffers = BufferNodeList();
  return cur_buffers;
}

HGCRedirtyCardsLocalQueueSet::Queue::Queue(HGCRedirtyCardsLocalQueueSet* qset) :
  PtrQueue(qset)
{}

#ifdef ASSERT
HGCRedirtyCardsLocalQueueSet::Queue::~Queue() {
  assert(buffer() == nullptr, "unflushed queue");
}
#endif

HGCRedirtyCardsQueueSet::HGCRedirtyCardsQueueSet(BufferNode::Allocator* allocator) :
  PtrQueueSet(allocator),
  _list(),
  _entry_count(0),
  _tail(nullptr)
  DEBUG_ONLY(COMMA _collecting(true))
{}

HGCRedirtyCardsQueueSet::~HGCRedirtyCardsQueueSet() {
  verify_empty();
}

#ifdef ASSERT
#endif

BufferNode* HGCRedirtyCardsQueueSet::all_completed_buffers() const {
  DEBUG_ONLY(_collecting = false;)
  return _list.top();
}

BufferNodeList HGCRedirtyCardsQueueSet::take_all_completed_buffers() {
  DEBUG_ONLY(_collecting = false;)
  BufferNodeList result(_list.pop_all(), _tail, _entry_count);
  _tail = nullptr;
  _entry_count = 0;
  DEBUG_ONLY(_collecting = true;)
  return result;
}

void HGCRedirtyCardsQueueSet::update_tail(BufferNode* node) {
  if (node->next() == nullptr) {
    assert(_tail == nullptr, "invariant");
    _tail = node;
  }
}

void HGCRedirtyCardsQueueSet::enqueue_completed_buffer(BufferNode* node) {
  assert(_collecting, "precondition");
  Atomic::add(&_entry_count, node->size());
  _list.push(*node);
  update_tail(node);
}

void HGCRedirtyCardsQueueSet::add_bufferlist(const BufferNodeList& buffers) {
  assert(_collecting, "precondition");
  if (buffers._head != nullptr) {
    assert(buffers._tail != nullptr, "invariant");
    Atomic::add(&_entry_count, buffers._entry_count);
    _list.prepend(*buffers._head, *buffers._tail);
    update_tail(buffers._tail);
  }
}

void HGCRedirtyCardsQueueSet::verify_empty() const {
  assert(_list.empty(), "precondition");
  assert(_tail == nullptr, "invariant");
  assert(_entry_count == 0, "invariant");
}
