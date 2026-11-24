#ifndef SHARE_GC_H_HCOLLECTEDHEAP_HPP
#define SHARE_GC_H_HCOLLECTEDHEAP_HPP

#include "code/nmethod.hpp"
#include "gc/h/hArena.hpp"
#include "gc/h/hLargeRunTree.hpp"
#include "gc/h/hRegion.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/referenceDiscoverer.hpp"
#include "gc/shared/gcCause.hpp"
#include "memory/iterator.hpp"
#include "oops/stackChunkOop.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/thread.hpp"
#include "runtime/vmStructs.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"
#include "utilities/powerOfTwo.hpp"
#include <cstddef>

class HGCMarkQueueSet;
class WorkerThreads;
class HGCCardTable;
class HGCConcurrentMarkThread;
class HGCUncommitThread;
class HGCConcurrentRefine;
class HGCPolicy;
class HGCCollectionSetPolicy;
class HGCRemSet;
class HGCCardSetConfiguration;
class HGCMonotonicArenaFreePool;
class ReferenceProcessor;
class Mutex;

class HGCRefDiscoverer : public ReferenceDiscoverer {
  ReferenceProcessor* _rp;
  GrowableArray<oop> _discovered;
  Mutex* _lock;

public:
  HGCRefDiscoverer();
  ~HGCRefDiscoverer();
  void set_ref_processor(ReferenceProcessor* rp) { _rp = rp; }
  void start_discovery();
  bool discover_reference(oop obj, ReferenceType rt) override;
};

class HGCCollectedHeap : public CollectedHeap {
  friend VMStructs;
protected:
  virtual HeapWord* allocate_new_tlab(size_t min_size,
                                      size_t requested_size,
                                      size_t* actual_size) override;

  virtual HeapWord* mem_allocate(size_t size,
      bool* gc_overhead_limit_was_exceeded) override;

private:
  HeapWord* _start;
  size_t _num_of_k;
  volatile size_t _current_count_of_regions;

  volatile size_t _committed_regions;
  volatile size_t _real_memory_bytes;
  size_t _count_of_regions;
  GrowableArray<size_t>* _free_region_indices;
  static constexpr size_t max_num_of_k = 19;
  HGCRegionDoubleList _kregions[max_num_of_k];
  WorkerThreads* _workers;
  HGCMarkQueueSet* _mark_queue_set;
  HGCCardTable* _card_table;
  HGCRemSet* _rem_set;
  HGCCardSetConfiguration* _card_set_config;
  HGCMonotonicArenaFreePool* _card_set_free_pool;
  HGCConcurrentMarkThread* _cm_thread;
  HGCUncommitThread* _uncommit_thread;
  HGCConcurrentRefine* _refinement;
  HGCPolicy* _policy;
  HGCCollectionSetPolicy* _cset_policy;
  AlwaysTrueClosure _always_true;
  HGCRefDiscoverer _ref_discoverer;
  ReferenceProcessor* _ref_processor;
  bool _clear_all_soft_refs;

  HGCVolatileRegionList _cset;
  HGCVolatileRegionList _snapshot_of_cset;

  HGCVolatileRegionList _empty_regions;
  GrowableArray<HGCRegion*>* _uncommit_queue;
  GrowableArray<HGCRegion*>* _uncommitted_regions;

  HGCVolatileRegionList _large_owners;

  Mutex* _cset_lock;

  HGCLargeRunTree _large_runs;
  Mutex* _region_space_lock;
  Mutex* _kregions_lock;
  Monitor* _uncommit_lock;

  static Mutex* _detached_refinement_stats_lock;

  Monitor* _cycle_lock;

  Monitor* _cycle_end_lock;

  volatile bool _cycle_active;

  volatile bool _concurrent_marking;
  bool _stopped;
  HGCRegion* _regions[];

  virtual void initialize_serviceability() override;

public:
  void* operator new(size_t size);
  HGCCollectedHeap();
  ~HGCCollectedHeap();
  static HGCCollectedHeap* heap();

  HGCRegion* region(HeapWord* ptr) const;
  HGCArena* arena(HeapWord* ptr) const;

  address c2_region_table_start_addr() const { return (address) &_start; }
  address c2_region_table_count_addr() const { return (address) &_count_of_regions; }
  address c2_region_table_base_addr() const  { return (address) &_regions[0]; }

  WorkerThreads* workers() const { return _workers; }
  HGCMarkQueueSet* mark_queue_set() const { return _mark_queue_set; }
  HGCCardTable* card_table() const { return _card_table; }
  HGCRemSet* rem_set() const { return _rem_set; }

  static Mutex* detached_refinement_stats_lock() { return _detached_refinement_stats_lock; }
  HGCConcurrentMarkThread* cm_thread() const { return _cm_thread; }
  HGCConcurrentRefine* refinement() const { return _refinement; }
  ReferenceProcessor* ref_processor() const;
  HGCRefDiscoverer* ref_discoverer();
  bool clear_all_soft_refs() const { return _clear_all_soft_refs; }

  size_t count_of_regions() const;
  size_t cset_limit() const;
  void cset_remark_started();
  void cset_remark_finished();
  HGCRegion* region_at(size_t index) const { return _regions[index]; }
  uint region_index(HeapWord* ptr) const;

  HGCVolatileRegionList& snapshot_of_cset() { return _snapshot_of_cset; }

  bool is_cycle_active() const { return _cycle_active; }
  bool is_concurrent_marking() const { return _concurrent_marking; }
  void set_concurrent_marking(bool value);
  void mark_object_concurrently(oop obj);
  Monitor* cycle_lock() const { return _cycle_lock; }
  void start_cycle();
  void end_cycle();
  void wait_for_cycle_end();

  void maybe_start_cycle();
  bool is_young_card(HeapWord* p) const;
  void start_current_cset();
  void fill_cset_all();
  HGCRegion* pop_cset();

  void release_region(HGCRegion* region);
  void add_region(HGCRegion* region);
  void detach_empty_regions();
  void uncommit_pending_regions();
  void reclaim_uncommitted_regions();
  Monitor* uncommit_lock() const { return _uncommit_lock; }
  void add_region_to_cset(HGCRegion* region);
  void add_large_owner(HGCRegion* region) { _large_owners.push(region); }
  void free_large_run(HGCRegion* owner);
  void clear_thread_local_arenas();

  virtual Name kind() const override;
  virtual const char* name() const override;

  virtual jint initialize() override;

  virtual size_t capacity() const override;
  virtual size_t used() const override;
  virtual size_t max_capacity() const override;

  size_t real_memory_used() const;
  virtual bool is_in(const void* p) const override;

  virtual size_t tlab_capacity(Thread* thr) const override;
  virtual size_t tlab_used(Thread* thr) const override;
  virtual size_t unsafe_max_tlab_alloc(Thread* thr) const override;
  virtual size_t max_tlab_size() const override;

  virtual void collect(GCCause::Cause cause) override;
  virtual void do_full_collection(bool clear_all_soft_refs) override;
  virtual void stop() override;
  virtual void safepoint_synchronize_begin() override;
  virtual void safepoint_synchronize_end() override;

  virtual bool requires_barriers(stackChunkOop obj) const override;

  virtual GrowableArray<GCMemoryManager*> memory_managers() override;
  virtual GrowableArray<MemoryPool*> memory_pools() override;

  virtual void object_iterate(ObjectClosure* cl) override;

  virtual void prepare_for_verify() override;

  virtual void print_heap_on(outputStream* st) const override;
  virtual void print_gc_on(outputStream* st) const override;
  virtual bool print_location(outputStream* st, void* addr) const override;
  virtual void gc_threads_do(ThreadClosure* tc) const override;
  virtual void print_tracing_info() const override;

  void dump_heap_map();

  virtual void register_nmethod(nmethod* nm) override;
  virtual void unregister_nmethod(nmethod* nm) override;
  virtual void verify_nmethod(nmethod* nm) override;

  virtual void verify(VerifyOption option) override;

  void verify_structural();

  virtual void pin_object(JavaThread* thread, oop obj) override;
  virtual void unpin_object(JavaThread* thread, oop obj) override;
private:
  class InAligns {
  public:
    static HeapWord* allocate(HGCCollectedHeap* self,
        HGCArena::Alignments min_size,
        HGCArena::Alignments requested_size,
        HGCArena::Alignments* actual_size,
        bool allow_large);

    static HeapWord* mem_allocate(HGCCollectedHeap* self,
        HGCArena::Alignments size,
        bool* gc_overhead_limit_was_exceeded);
  };
  friend InAligns;

  HGCRegion* allocate_new_region(size_t k);
  void publish_region(HGCRegion* region);

  bool is_large_request(HGCArena::Alignments size) const;
  HeapWord* allocate_large(HGCArena::Alignments size, HGCArena::Alignments* actual_size);
  void add_large_run(size_t start, size_t regions);

  static size_t k_by_requested_size(HGCArena::Alignments size_in_alignments);
  static size_t min_k();
  static size_t max_k();
};

#define heap_locking_asserts_params(_extra_message_)                          \
  "%s : Heap_lock locked: %s, at safepoint: %s, is VM thread: %s",            \
  (_extra_message_),                                                          \
  BOOL_TO_STR(Heap_lock->owned_by_self()),                                    \
  BOOL_TO_STR(SafepointSynchronize::is_at_safepoint()),                       \
  BOOL_TO_STR(Thread::current()->is_VM_thread())

#define assert_heap_locked()                                                  \
  do {                                                                        \
    assert(Heap_lock->owned_by_self(),                                        \
           heap_locking_asserts_params("should be holding the Heap_lock"));   \
  } while (0)

#define assert_heap_locked_or_at_safepoint(_should_be_vm_thread_)             \
  do {                                                                        \
    assert(Heap_lock->owned_by_self() ||                                      \
           (SafepointSynchronize::is_at_safepoint() &&                        \
             ((_should_be_vm_thread_) == Thread::current()->is_VM_thread())), \
           heap_locking_asserts_params("should be holding the Heap_lock or "  \
                                        "should be at a safepoint"));         \
  } while (0)

#define assert_heap_locked_and_not_at_safepoint()                             \
  do {                                                                        \
    assert(Heap_lock->owned_by_self() &&                                      \
                                    !SafepointSynchronize::is_at_safepoint(), \
          heap_locking_asserts_params("should be holding the Heap_lock and "  \
                                       "should not be at a safepoint"));      \
  } while (0)

#define assert_heap_not_locked()                                              \
  do {                                                                        \
    assert(!Heap_lock->owned_by_self(),                                       \
        heap_locking_asserts_params("should not be holding the Heap_lock"));  \
  } while (0)

#define assert_heap_not_locked_and_not_at_safepoint()                         \
  do {                                                                        \
    assert(!Heap_lock->owned_by_self() &&                                     \
                                    !SafepointSynchronize::is_at_safepoint(), \
      heap_locking_asserts_params("should not be holding the Heap_lock and "  \
                                   "should not be at a safepoint"));          \
  } while (0)

#define assert_at_safepoint_on_vm_thread()                                        \
  do {                                                                            \
    assert_at_safepoint();                                                        \
    assert(Thread::current_or_null() != nullptr, "no current thread");            \
    assert(Thread::current()->is_VM_thread(), "current thread is not VM thread"); \
  } while (0)

#ifdef ASSERT
#define assert_used_and_recalculate_used_equal(g1h)                           \
  do {                                                                        \
    size_t cur_used_bytes = g1h->used();                                      \
    size_t recal_used_bytes = g1h->recalculate_used();                        \
    assert(cur_used_bytes == recal_used_bytes, "Used(%zu) is not" \
           " same as recalculated used(%zu).",                    \
           cur_used_bytes, recal_used_bytes);                                 \
  } while (0)
#else
#define assert_used_and_recalculate_used_equal(g1h) do {} while(0)
#endif

#endif
