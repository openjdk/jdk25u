#ifndef SHARE_GC_H_HTHREADLOCALDATA_HPP
#define SHARE_GC_H_HTHREADLOCALDATA_HPP

#include "gc/h/hArena.hpp"
#include "gc/h/hDirtyCardQueue.hpp"
#include "gc/h/hSATBMarkQueueSet.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/satbMarkQueue.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"

class HGCRegion;

class HGCThreadLocalData {
private:
  HGCArena* _arena;
  size_t _tlab_size;
  SATBMarkQueue _satb_mark_queue;
  HGCDirtyCardQueue _dirty_card_queue;

  HGCThreadLocalData(HGCSATBMarkQueueSet* queue_set,
                     HGCDirtyCardQueueSet* dirty_card_queue_set) :
    _arena(nullptr),
    _tlab_size(0),
    _satb_mark_queue(queue_set),
    _dirty_card_queue(dirty_card_queue_set) {}

  static HGCThreadLocalData* data(Thread* thread) {
    assert(UseHGC, "Sanity");
    return thread->gc_data<HGCThreadLocalData>();
  }

  static ByteSize satb_mark_queue_offset() {
    return Thread::gc_data_offset() + byte_offset_of(HGCThreadLocalData, _satb_mark_queue);
  }

  static ByteSize dirty_card_queue_offset() {
    return Thread::gc_data_offset() + byte_offset_of(HGCThreadLocalData, _dirty_card_queue);
  }

public:
  static void create(Thread* thread, HGCSATBMarkQueueSet* queue_set,
                     HGCDirtyCardQueueSet* dirty_card_queue_set) {
    new (data(thread)) HGCThreadLocalData(queue_set, dirty_card_queue_set);
  }

  static void destroy(Thread* thread) {
    data(thread)->~HGCThreadLocalData();
  }

  static HGCArena* get_arena(Thread* t) {
    return data(t)->_arena;
  }

  static void set_arena(Thread* t, HGCArena* a) {
    data(t)->_arena = a;
  }

  static size_t get_tlab_size(Thread* t) {
    return data(t)->_tlab_size;
  }

  static void set_tlab_size(Thread* t, size_t size) {
    data(t)->_tlab_size = size;
  }

  static SATBMarkQueue& satb_mark_queue(Thread* thread) {
    return data(thread)->_satb_mark_queue;
  }

  static ByteSize satb_mark_queue_active_offset() {
    return satb_mark_queue_offset() + SATBMarkQueue::byte_offset_of_active();
  }

  static ByteSize satb_mark_queue_index_offset() {
    return satb_mark_queue_offset() + SATBMarkQueue::byte_offset_of_index();
  }

  static ByteSize satb_mark_queue_buffer_offset() {
    return satb_mark_queue_offset() + SATBMarkQueue::byte_offset_of_buf();
  }

  static HGCDirtyCardQueue& dirty_card_queue(Thread* thread) {
    return data(thread)->_dirty_card_queue;
  }

  static ByteSize dirty_card_queue_index_offset() {
    return dirty_card_queue_offset() + HGCDirtyCardQueue::byte_offset_of_index();
  }

  static ByteSize dirty_card_queue_buffer_offset() {
    return dirty_card_queue_offset() + HGCDirtyCardQueue::byte_offset_of_buf();
  }
};

#endif
