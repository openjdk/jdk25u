#ifndef SHARE_GC_H_HARGUMENTS_HPP
#define SHARE_GC_H_HARGUMENTS_HPP

#include "gc/shared/gcArguments.hpp"

class CollectedHeap;

class HGCArguments : public GCArguments {
private:
  virtual void initialize_alignments();
  virtual void initialize();
  virtual size_t conservative_max_heap_alignment();
  virtual CollectedHeap* create_heap();
  virtual bool is_supported() const;
};

#endif
