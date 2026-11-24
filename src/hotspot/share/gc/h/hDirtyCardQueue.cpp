#include "gc/h/hBarrierSet.hpp"
#include "gc/h/hCardTable.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/hConcurrentRefine.hpp"
#include "gc/h/hConcurrentRefineStats.hpp"
#include "gc/h/hDirtyCardQueue.hpp"
#include "gc/h/hFreeIdSet.hpp"
#include "gc/h/hRedirtyCardsQueue.hpp"
#include "gc/h/hRemSet.hpp"
#include "gc/h/hRegionRemSet.hpp"
#include "gc/h/hThreadLocalData.hpp"
#include "gc/shared/bufferNode.hpp"
#include "gc/shared/bufferNodeList.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "memory/iterator.hpp"
#include "runtime/atomic.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/threads.hpp"
#include "runtime/threadSMR.hpp"
#include "utilities/globalCounter.inline.hpp"
#include "utilities/nonblockingQueue.inline.hpp"
#include "utilities/pair.hpp"
#include "utilities/quickSort.hpp"
#include "utilities/ticks.hpp"

HGCDirtyCardQueue::HGCDirtyCardQueue(HGCDirtyCardQueueSet* qset) :
  PtrQueue(qset),
  _refinement_stats(new HGCConcurrentRefineStats())
{ }

HGCDirtyCardQueue::~HGCDirtyCardQueue() {
  delete _refinement_stats;
}

static uint par_ids_start() { return 0; }

HGCDirtyCardQueueSet::HGCDirtyCardQueueSet(BufferNode::Allocator* allocator) :
  PtrQueueSet(allocator),
  _num_cards(0),
  _mutator_refinement_threshold(SIZE_MAX),
  _completed(),
  _paused(),
  _free_ids(par_ids_start(), num_par_ids()),
  _refinement(nullptr),
  _detached_refinement_stats()
{}

HGCDirtyCardQueueSet::~HGCDirtyCardQueueSet() {
  abandon_completed_buffers();
}

uint HGCDirtyCardQueueSet::num_par_ids() {
  return (uint)os::initial_active_processor_count();
}

void HGCDirtyCardQueueSet::flush_queue(HGCDirtyCardQueue& queue) {
  if (queue.buffer() != nullptr) {
    HGCConcurrentRefineStats* stats = queue.refinement_stats();
    stats->inc_dirtied_cards(queue.size());
  }
  PtrQueueSet::flush_queue(queue);
}

void HGCDirtyCardQueueSet::enqueue(HGCDirtyCardQueue& queue,
                                   volatile CardValue* card_ptr) {
  CardValue* value = const_cast<CardValue*>(card_ptr);
  if (!try_enqueue(queue, value)) {
    handle_zero_index(queue);
    retry_enqueue(queue, value);
  }
}

void HGCDirtyCardQueueSet::handle_zero_index(HGCDirtyCardQueue& queue) {
  assert(queue.index() == 0, "precondition");
  BufferNode* old_node = exchange_buffer_with_new(queue);
  if (old_node != nullptr) {
    assert(old_node->index() == 0, "invariant");
    HGCConcurrentRefineStats* stats = queue.refinement_stats();
    stats->inc_dirtied_cards(old_node->capacity());
    handle_completed_buffer(old_node, stats);
  }
}

void HGCDirtyCardQueueSet::handle_zero_index_for_thread(Thread* t) {
  HGCDirtyCardQueue& queue = HGCThreadLocalData::dirty_card_queue(t);
  HGCBarrierSet::dirty_card_queue_set().handle_zero_index(queue);
}

size_t HGCDirtyCardQueueSet::num_cards() const {
  return Atomic::load(&_num_cards);
}

void HGCDirtyCardQueueSet::enqueue_completed_buffer(BufferNode* cbn) {
  assert(cbn != nullptr, "precondition");
  Atomic::add(&_num_cards, cbn->size());
  GlobalCounter::CriticalSection cs(Thread::current());
  _completed.push(*cbn);
}

BufferNode* HGCDirtyCardQueueSet::dequeue_completed_buffer() {
  Thread* current_thread = Thread::current();
  BufferNode* result = nullptr;
  while (true) {
    GlobalCounter::CriticalSection cs(current_thread);
    if (_completed.try_pop(&result)) return result;
  }
}

BufferNode* HGCDirtyCardQueueSet::get_completed_buffer() {
  BufferNode* result = dequeue_completed_buffer();
  if (result == nullptr) {
    enqueue_previous_paused_buffers();
    result = dequeue_completed_buffer();
    if (result == nullptr) return nullptr;
  }
  Atomic::sub(&_num_cards, result->size());
  return result;
}

#ifdef ASSERT
void HGCDirtyCardQueueSet::verify_num_cards() const {
  size_t actual = 0;
  for (BufferNode* cur = _completed.first();
       !_completed.is_end(cur);
       cur = cur->next()) {
    actual += cur->size();
  }
  assert(actual == Atomic::load(&_num_cards),
         "Num entries in completed buffers should be %zu but are %zu",
         Atomic::load(&_num_cards), actual);
}
#endif

HGCDirtyCardQueueSet::PausedBuffers::PausedList::PausedList() :
  _head(nullptr), _tail(nullptr),
  _safepoint_id(SafepointSynchronize::safepoint_id())
{}

#ifdef ASSERT
HGCDirtyCardQueueSet::PausedBuffers::PausedList::~PausedList() {
  assert(Atomic::load(&_head) == nullptr, "precondition");
  assert(_tail == nullptr, "precondition");
}
#endif

bool HGCDirtyCardQueueSet::PausedBuffers::PausedList::is_next() const {
  assert_not_at_safepoint();
  return _safepoint_id == SafepointSynchronize::safepoint_id();
}

void HGCDirtyCardQueueSet::PausedBuffers::PausedList::add(BufferNode* node) {
  assert_not_at_safepoint();
  assert(is_next(), "precondition");
  BufferNode* old_head = Atomic::xchg(&_head, node);
  if (old_head == nullptr) {
    assert(_tail == nullptr, "invariant");
    _tail = node;
  } else {
    node->set_next(old_head);
  }
}

HGCDirtyCardQueueSet::HeadTail HGCDirtyCardQueueSet::PausedBuffers::PausedList::take() {
  BufferNode* head = Atomic::load(&_head);
  BufferNode* tail = _tail;
  Atomic::store(&_head, (BufferNode*)nullptr);
  _tail = nullptr;
  return HeadTail(head, tail);
}

HGCDirtyCardQueueSet::PausedBuffers::PausedBuffers() : _plist(nullptr) {}

#ifdef ASSERT
HGCDirtyCardQueueSet::PausedBuffers::~PausedBuffers() {
  assert(Atomic::load(&_plist) == nullptr, "invariant");
}
#endif

void HGCDirtyCardQueueSet::PausedBuffers::add(BufferNode* node) {
  assert_not_at_safepoint();
  PausedList* plist = Atomic::load_acquire(&_plist);
  if (plist == nullptr) {
    plist = new PausedList();
    PausedList* old_plist = Atomic::cmpxchg(&_plist, (PausedList*)nullptr, plist);
    if (old_plist != nullptr) {
      delete plist;
      plist = old_plist;
    }
  }
  assert(plist->is_next(), "invariant");
  plist->add(node);
}

HGCDirtyCardQueueSet::HeadTail HGCDirtyCardQueueSet::PausedBuffers::take_previous() {
  assert_not_at_safepoint();
  PausedList* previous;
  {
    GlobalCounter::CriticalSection cs(Thread::current());
    previous = Atomic::load_acquire(&_plist);
    if ((previous == nullptr) ||
        previous->is_next() ||
        (Atomic::cmpxchg(&_plist, previous, (PausedList*)nullptr) != previous)) {
      return HeadTail();
    }
  }
  HeadTail result = previous->take();
  GlobalCounter::write_synchronize();
  delete previous;
  return result;
}

HGCDirtyCardQueueSet::HeadTail HGCDirtyCardQueueSet::PausedBuffers::take_all() {
  assert_at_safepoint();
  HeadTail result;
  PausedList* plist = Atomic::load(&_plist);
  if (plist != nullptr) {
    Atomic::store(&_plist, (PausedList*)nullptr);
    result = plist->take();
    delete plist;
  }
  return result;
}

void HGCDirtyCardQueueSet::record_paused_buffer(BufferNode* node) {
  assert(node->next() == nullptr, "precondition");
  if (SafepointSynchronize::is_at_safepoint()) {

    enqueue_completed_buffer(node);
    return;
  }
  enqueue_previous_paused_buffers();
  Atomic::add(&_num_cards, node->size());
  _paused.add(node);
}

void HGCDirtyCardQueueSet::enqueue_paused_buffers_aux(const HeadTail& paused) {
  if (paused._head != nullptr) {
    assert(paused._tail != nullptr, "invariant");
    _completed.append(*paused._head, *paused._tail);
  }
}

void HGCDirtyCardQueueSet::enqueue_previous_paused_buffers() {
  assert_not_at_safepoint();
  enqueue_paused_buffers_aux(_paused.take_previous());
}

void HGCDirtyCardQueueSet::enqueue_all_paused_buffers() {
  assert_at_safepoint();
  enqueue_paused_buffers_aux(_paused.take_all());
}

void HGCDirtyCardQueueSet::abandon_completed_buffers() {
  BufferNodeList list = take_all_completed_buffers();
  BufferNode* buffers_to_delete = list._head;
  while (buffers_to_delete != nullptr) {
    BufferNode* bn = buffers_to_delete;
    buffers_to_delete = bn->next();
    bn->set_next(nullptr);
    deallocate_buffer(bn);
  }
}

void HGCDirtyCardQueueSet::merge_bufferlists(HGCRedirtyCardsQueueSet* src) {
  assert(allocator() == src->allocator(), "precondition");
  const BufferNodeList from = src->take_all_completed_buffers();
  if (from._head != nullptr) {
    Atomic::add(&_num_cards, from._entry_count);
    _completed.append(*from._head, *from._tail);
  }
}

BufferNodeList HGCDirtyCardQueueSet::take_all_completed_buffers() {
  enqueue_all_paused_buffers();
  verify_num_cards();
  Pair<BufferNode*, BufferNode*> pair = _completed.take_all();
  size_t num_cards = Atomic::load(&_num_cards);
  Atomic::store(&_num_cards, size_t(0));
  return BufferNodeList(pair.first, pair.second, num_cards);
}

class HGCRefineBufferedCards : public StackObj {
  BufferNode* const _node;
  CardTable::CardValue** const _node_buffer;
  const size_t _node_buffer_capacity;
  const uint _worker_id;
  HGCConcurrentRefineStats* _stats;
  HGCRemSet* const _rs;
  const bool _allow_yield;

  static inline ptrdiff_t compare_cards(const CardTable::CardValue* p1,
                                        const CardTable::CardValue* p2) {
    return p2 - p1;
  }

  void sort_cards(size_t start_index) {
    QuickSort::sort(&_node_buffer[start_index],
                    _node_buffer_capacity - start_index,
                    compare_cards);
  }

  size_t clean_cards() {
    const size_t start = _node->index();
    assert(start <= _node_buffer_capacity, "invariant");

    CardTable::CardValue** src = &_node_buffer[start];
    CardTable::CardValue** dst = &_node_buffer[_node_buffer_capacity];
    assert(src <= dst, "invariant");
    for ( ; src < dst; ++src) {
      if (_rs->clean_card_before_refine(src)) {
        while (src < --dst) {
          if (!_rs->clean_card_before_refine(dst)) {
            *dst = *src;
            break;
          }
        }
      }
    }

    const size_t first_clean = dst - _node_buffer;
    assert(first_clean >= start && first_clean <= _node_buffer_capacity, "invariant");
    _stats->inc_refined_cards(first_clean - start);
    _stats->inc_precleaned_cards(first_clean - start);
    return first_clean;
  }

  bool refine_cleaned_cards(size_t start_index) {
    bool result = true;
    size_t i = start_index;
    for ( ; i < _node_buffer_capacity; ++i) {
      if (_allow_yield && SuspendibleThreadSet::should_yield()) {
        redirty_unrefined_cards(i);
        result = false;
        break;
      }

      if (!HGCCardTable::is_clean_card_value(*_node_buffer[i])) {
        continue;
      }
      _rs->refine_card_concurrently(_node_buffer[i], _worker_id);
    }
    _node->set_index(i);
    _stats->inc_refined_cards(i - start_index);
    return result;
  }

  void redirty_unrefined_cards(size_t start) {
    for ( ; start < _node_buffer_capacity; ++start) {
      HGCBarrierSet::requeue_dirty_card(_node_buffer[start]);
    }
  }

public:
  HGCRefineBufferedCards(BufferNode* node,
                         uint worker_id,
                         HGCConcurrentRefineStats* stats,
                         bool allow_yield) :
    _node(node),
    _node_buffer(reinterpret_cast<CardTable::CardValue**>(BufferNode::make_buffer_from_node(node))),
    _node_buffer_capacity(node->capacity()),
    _worker_id(worker_id),
    _stats(stats),
    _rs(HGCCollectedHeap::heap()->rem_set()),
    _allow_yield(allow_yield) {}

  bool refine() {
    size_t first_clean_index = clean_cards();
    if (first_clean_index == _node_buffer_capacity) {
      _node->set_index(first_clean_index);
      return true;
    }
    OrderAccess::fence();
    sort_cards(first_clean_index);
    return refine_cleaned_cards(first_clean_index);
  }
};

bool HGCDirtyCardQueueSet::refine_buffer(BufferNode* node,
                                         uint worker_id,
                                         HGCConcurrentRefineStats* stats,
                                         bool allow_yield) {
  Ticks start_time = Ticks::now();
  HGCRefineBufferedCards buffered_cards(node, worker_id, stats, allow_yield);
  bool result = buffered_cards.refine();
  stats->inc_refinement_time(Ticks::now() - start_time);
  return result;
}

void HGCDirtyCardQueueSet::handle_refined_buffer(BufferNode* node,
                                                 bool fully_processed) {
  if (fully_processed) {
    assert(node->is_empty(), "Buffer not fully consumed: index: %zu, size: %zu",
           node->index(), node->capacity());
    deallocate_buffer(node);
  } else {
    assert(!node->is_empty(), "Buffer fully consumed.");
    record_paused_buffer(node);
  }
}

void HGCDirtyCardQueueSet::handle_completed_buffer(BufferNode* new_node,
                                                   HGCConcurrentRefineStats* stats) {
  enqueue_completed_buffer(new_node);

  if (_refinement != nullptr) {

    _refinement->activate_primary();
    return;
  }

  if (Atomic::load(&_num_cards) <= Atomic::load(&_mutator_refinement_threshold)) {
    return;
  }

  if (SuspendibleThreadSet::should_yield()) {
    return;
  }

  if (!Thread::current()->is_Java_thread()) {
    return;
  }

  BufferNode* node = get_completed_buffer();
  if (node == nullptr) return;

  uint worker_id = _free_ids.claim_par_id();
  bool fully_processed = refine_buffer(node, worker_id, stats, true);
  _free_ids.release_par_id(worker_id);

  handle_refined_buffer(node, fully_processed);
}

bool HGCDirtyCardQueueSet::refine_completed_buffer_no_paused(uint worker_id,
                                                             HGCConcurrentRefineStats* stats) {
  if (Atomic::load(&_num_cards) == 0) {
    return false;
  }

  BufferNode* node = dequeue_completed_buffer();
  if (node == nullptr) {
    return false;
  }
  Atomic::sub(&_num_cards, node->size());

  bool fully_processed = refine_buffer(node, worker_id, stats, false );
  handle_refined_buffer(node, fully_processed);
  return true;
}

bool HGCDirtyCardQueueSet::refine_completed_buffer_concurrently(uint worker_id,
                                                               size_t stop_at,
                                                               HGCConcurrentRefineStats* stats,
                                                               bool allow_yield) {
  if (Atomic::load(&_num_cards) <= stop_at) return false;

  BufferNode* node = get_completed_buffer();
  if (node == nullptr) return false;

  bool fully_processed = refine_buffer(node, worker_id, stats, allow_yield);
  handle_refined_buffer(node, fully_processed);
  return true;
}

void HGCDirtyCardQueueSet::abandon_logs_and_stats() {
  assert_at_safepoint();

  set_mutator_refinement_threshold(SIZE_MAX);

  struct AbandonThreadLogClosure : public ThreadClosure {
    HGCDirtyCardQueueSet& _qset;
    AbandonThreadLogClosure(HGCDirtyCardQueueSet& qset) : _qset(qset) {}
    virtual void do_thread(Thread* t) {
      HGCDirtyCardQueue& queue = HGCThreadLocalData::dirty_card_queue(t);
      _qset.reset_queue(queue);
      queue.refinement_stats()->reset();
    }
  } closure(*this);
  Threads::threads_do(&closure);

  enqueue_all_paused_buffers();
  abandon_completed_buffers();

  MutexLocker ml(HGCCollectedHeap::detached_refinement_stats_lock(), Mutex::_no_safepoint_check_flag);
  _detached_refinement_stats.reset();
}

void HGCDirtyCardQueueSet::update_refinement_stats(HGCConcurrentRefineStats& stats) {
  assert_at_safepoint();

  _concatenated_refinement_stats = stats;

  enqueue_all_paused_buffers();
  verify_num_cards();

  MutexLocker ml(HGCCollectedHeap::detached_refinement_stats_lock(), Mutex::_no_safepoint_check_flag);
  _concatenated_refinement_stats += _detached_refinement_stats;
  _detached_refinement_stats.reset();
}

HGCConcurrentRefineStats HGCDirtyCardQueueSet::concatenate_log_and_stats(Thread* thread) {
  assert_at_safepoint();

  HGCDirtyCardQueue& queue = HGCThreadLocalData::dirty_card_queue(thread);
  if (!queue.is_empty()) {
    flush_queue(queue);
  }

  HGCConcurrentRefineStats result = *queue.refinement_stats();
  queue.refinement_stats()->reset();
  return result;
}

HGCConcurrentRefineStats HGCDirtyCardQueueSet::concatenated_refinement_stats() const {
  assert_at_safepoint();
  return _concatenated_refinement_stats;
}

void HGCDirtyCardQueueSet::record_detached_refinement_stats(HGCConcurrentRefineStats* stats) {
  MutexLocker ml(HGCCollectedHeap::detached_refinement_stats_lock(), Mutex::_no_safepoint_check_flag);
  _detached_refinement_stats += *stats;
  stats->reset();
}

size_t HGCDirtyCardQueueSet::mutator_refinement_threshold() const {
  return Atomic::load(&_mutator_refinement_threshold);
}

void HGCDirtyCardQueueSet::set_mutator_refinement_threshold(size_t value) {
  Atomic::store(&_mutator_refinement_threshold, value);
}
