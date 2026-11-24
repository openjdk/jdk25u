#ifndef SHARE_GC_H_HDIRTYCARDQUEUE_HPP
#define SHARE_GC_H_HDIRTYCARDQUEUE_HPP

#include "gc/h/hCardTable.hpp"
#include "gc/h/hConcurrentRefineStats.hpp"
#include "gc/h/hFreeIdSet.hpp"
#include "gc/shared/bufferNode.hpp"
#include "gc/shared/bufferNodeList.hpp"
#include "gc/shared/ptrQueue.hpp"
#include "memory/allocation.hpp"
#include "memory/padded.hpp"
#include "utilities/nonblockingQueue.hpp"

class HGCConcurrentRefine;
class HGCConcurrentRefineThread;
class HGCDirtyCardQueueSet;
class HGCRedirtyCardsQueueSet;
class Thread;

class HGCDirtyCardQueue : public PtrQueue {
  HGCConcurrentRefineStats* _refinement_stats;

public:
  HGCDirtyCardQueue(HGCDirtyCardQueueSet* qset);

  ~HGCDirtyCardQueue();

  HGCConcurrentRefineStats* refinement_stats() const {
    return _refinement_stats;
  }

  static ByteSize byte_offset_of_index() {
    return PtrQueue::byte_offset_of_index<HGCDirtyCardQueue>();
  }
  using PtrQueue::byte_width_of_index;

  static ByteSize byte_offset_of_buf() {
    return PtrQueue::byte_offset_of_buf<HGCDirtyCardQueue>();
  }
  using PtrQueue::byte_width_of_buf;
};

class HGCDirtyCardQueueSet : public PtrQueueSet {
  struct HeadTail {
    BufferNode* _head;
    BufferNode* _tail;
    HeadTail() : _head(nullptr), _tail(nullptr) {}
    HeadTail(BufferNode* head, BufferNode* tail) : _head(head), _tail(tail) {}
  };

  class PausedBuffers {
    class PausedList : public CHeapObj<mtGC> {
      BufferNode* volatile _head;
      BufferNode* _tail;
      size_t _safepoint_id;

      NONCOPYABLE(PausedList);

    public:
      PausedList();
      DEBUG_ONLY(~PausedList();)

      bool is_next() const;

      void add(BufferNode* node);

      HeadTail take();
    };

    PausedList* volatile _plist;
    DEFINE_PAD_MINUS_SIZE(1, DEFAULT_PADDING_SIZE, sizeof(PausedList*));

    NONCOPYABLE(PausedBuffers);

  public:
    PausedBuffers();
    DEBUG_ONLY(~PausedBuffers();)

    void add(BufferNode* node);

    HeadTail take_previous();

    HeadTail take_all();
  };

  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_PADDING_SIZE, 0);
  volatile size_t _num_cards;
  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_PADDING_SIZE, sizeof(size_t));
  volatile size_t _mutator_refinement_threshold;
  DEFINE_PAD_MINUS_SIZE(2, DEFAULT_PADDING_SIZE, sizeof(size_t));
  NonblockingQueue<BufferNode, &BufferNode::next_ptr> _completed;
  DEFINE_PAD_MINUS_SIZE(3, DEFAULT_PADDING_SIZE, sizeof(BufferNode*));
  PausedBuffers _paused;

  HGCFreeIdSet _free_ids;

  HGCConcurrentRefine* _refinement;

  HGCConcurrentRefineStats _concatenated_refinement_stats;
  HGCConcurrentRefineStats _detached_refinement_stats;

  void verify_num_cards() const NOT_DEBUG_RETURN;

  void record_paused_buffer(BufferNode* node);
  void enqueue_paused_buffers_aux(const HeadTail& paused);
  void enqueue_previous_paused_buffers();

  void abandon_completed_buffers();

  bool refine_buffer(BufferNode* node,
                     uint worker_id,
                     HGCConcurrentRefineStats* stats,
                     bool allow_yield);

  void handle_refined_buffer(BufferNode* node, bool fully_processed);

  BufferNode* dequeue_completed_buffer();
  BufferNode* get_completed_buffer();

  void handle_zero_index(HGCDirtyCardQueue& queue);

  void handle_completed_buffer(BufferNode* node, HGCConcurrentRefineStats* stats);

public:
  HGCDirtyCardQueueSet(BufferNode::Allocator* allocator);
  ~HGCDirtyCardQueueSet();

  void set_refinement(HGCConcurrentRefine* refinement) { _refinement = refinement; }

  void enqueue_all_paused_buffers();

  static uint num_par_ids();

  static void handle_zero_index_for_thread(Thread* t);

  virtual void enqueue_completed_buffer(BufferNode* node);

  size_t num_cards() const;

  void merge_bufferlists(HGCRedirtyCardsQueueSet* src);

  BufferNodeList take_all_completed_buffers();

  void flush_queue(HGCDirtyCardQueue& queue);

  using CardValue = HGCCardTable::CardValue;
  void enqueue(HGCDirtyCardQueue& queue, volatile CardValue* card_ptr);

  bool refine_completed_buffer_concurrently(uint worker_id,
                                            size_t stop_at,
                                            HGCConcurrentRefineStats* stats,
                                            bool allow_yield = true);

  bool refine_completed_buffer_no_paused(uint worker_id,
                                         HGCConcurrentRefineStats* stats);

  void abandon_logs_and_stats();

  void update_refinement_stats(HGCConcurrentRefineStats& stats);
  HGCConcurrentRefineStats concatenate_log_and_stats(Thread* thread);

  HGCConcurrentRefineStats concatenated_refinement_stats() const;

  void record_detached_refinement_stats(HGCConcurrentRefineStats* stats);

  size_t mutator_refinement_threshold() const;

  void set_mutator_refinement_threshold(size_t value);
};

#endif
