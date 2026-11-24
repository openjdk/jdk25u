#ifndef SHARE_GC_H_HMARKSWEEP_HPP
#define SHARE_GC_H_HMARKSWEEP_HPP

#include "memory/allocation.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "gc/shared/workerThread.hpp"
#include "runtime/vmOperation.hpp"

class HGCRegion;
class HGCMarkClosure;
class HGCCollectedHeap;

class HGCMarkSweep : public VM_GC_Sync_Operation {
  bool _full;

  void mark_live();
  void sweep();

public:
  HGCMarkSweep(bool full) : _full(full) {}

  virtual VMOp_Type type() const override { return VMOp_HGCCollect; }
  virtual void doit() override;

  static void process_roots(bool scan_remsets = true);
  static void rebuild_rem_sets();
  static void scan_rem_sets(bool live_only = false, bool at_safepoint = true);
  static void refine_dirty_cards();
  static void prepare_class_unloading();
  static void unload_classes_concurrently();
};

void hgc_initialize_class_unloading();

class HGCInitialMark : public VM_Operation {
public:
  virtual VMOp_Type type() const override { return VMOp_HGCCollect; }
  virtual void doit() override;
};

class HGCRemark : public VM_GC_Sync_Operation {
public:
  virtual VMOp_Type type() const override { return VMOp_HGCCollect; }
  virtual void doit() override;
};

class HGCConcurrentRebuildTask : public WorkerTask {
  HGCCollectedHeap* _heap;
  volatile size_t _next;

public:
  explicit HGCConcurrentRebuildTask(HGCCollectedHeap* heap);
  virtual void work(uint worker_id) override;
};

class HGCRelease : public VM_Operation {
public:
  virtual VMOp_Type type() const override { return VMOp_HGCRelease; }
  virtual void doit() override;
};

#endif
