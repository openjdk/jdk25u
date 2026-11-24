#ifndef SHARE_GC_H_HSATBMARKQUEUESET_HPP
#define SHARE_GC_H_HSATBMARKQUEUESET_HPP

#include "gc/shared/bufferNode.hpp"
#include "gc/shared/satbMarkQueue.hpp"
#include "runtime/javaThread.hpp"

class HGCSATBMarkQueueSet : public SATBMarkQueueSet {
public:
  HGCSATBMarkQueueSet(BufferNode::Allocator* allocator);

  virtual SATBMarkQueue& satb_queue_for_thread(Thread* const t) const;
  virtual void filter(SATBMarkQueue& queue);
};

#endif
