#include "gc/h/hArguments.hpp"
#include "gc/h/hArena.hpp"
#include "gc/h/hCollectedHeap.hpp"
#include "gc/h/h_globals.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/cardTable.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "hCollectedHeap.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/powerOfTwo.hpp"

size_t HGCArguments::conservative_max_heap_alignment() {
  return MAXREGIONSIZE;
}

void HGCArguments::initialize() {
  GCArguments::initialize();

  assert(UseHGC, "Sanity");

  const size_t target_region = MaxHeapSize / 128;
  if (FLAG_IS_DEFAULT(HGCRegionSize)) {
    size_t region = round_down_power_of_2(target_region);
    region = MAX2(region, (size_t)4 * M);
    region = MIN2(region, (size_t)32 * M);
    FLAG_SET_ERGO(HGCRegionSize, region);
  }

  if (FLAG_IS_DEFAULT(HGCCSetCPUMultiplier)) {
    const size_t multiplier = target_region <= (size_t)4 * M ? 4 : 3;
    FLAG_SET_ERGO(HGCCSetCPUMultiplier, multiplier);
  }

  if (ClassUnloading) {

    FLAG_SET_DEFAULT(VerifyBeforeExit, false);
  }

  FLAG_SET_DEFAULT(ParallelGCThreads, WorkerPolicy::parallel_worker_threads());
  if (ParallelGCThreads == 0) {
    assert(!FLAG_IS_DEFAULT(ParallelGCThreads), "The default value for ParallelGCThreads should not be 0.");
    vm_exit_during_initialization("The flag -XX:+UseHGC can not be combined with -XX:ParallelGCThreads=0", nullptr);
  }

  if (FLAG_IS_DEFAULT(ConcGCThreads) || ConcGCThreads == 0) {
    FLAG_SET_ERGO(ConcGCThreads, MAX2(ParallelGCThreads / 4, 1U));
  }

  if (!is_power_of_2(HGCMinArenaSize)) {
    vm_exit_during_initialization("HGCMinRegionSize must be a power of two", nullptr);
  }

  if (!is_power_of_2(HGCRegionSize)) {
    vm_exit_during_initialization("HGCClusterSize must be a power of two", nullptr);
  }

  if (MinObjAlignmentInBytes == -1) vm_exit_during_initialization("MinObjAlignmentInBytes == -1", nullptr);

  size_t max_arena_size = ((size_t)1 << (8 * sizeof(HGCArena::Offset)))
      * MinObjAlignmentInBytes;

  if (HGCMaxArenaSize == 0) {
    size_t arena = MAX2(HGCMinArenaSize, HGCRegionSize / 4);
    HGCMaxArenaSize = MIN2(max_arena_size, arena);
  }

  if (!is_power_of_2(HGCMaxArenaSize)) {
    vm_exit_during_initialization("HGCMaxArenaSize must be a power of two", nullptr);
  }

  if (HGCMaxArenaSize > max_arena_size)
    vm_exit_during_initialization("HGCMaxArenaSize is so big", nullptr);

  if (HGCMinArenaSize > HGCMaxArenaSize) {
    vm_exit_during_initialization("HGCMinArenaSize > HGCMaxArenaSize", nullptr);
  }

  if (HGCMaxArenaSize > HGCRegionSize) {
    vm_exit_during_initialization("HGCMaxArenaSize > HGCRegionSize", nullptr);
  }
}

static size_t calculate_heap_alignment(size_t space_alignment) {
  size_t card_table_alignment = CardTable::ct_max_alignment_constraint();
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  return MAX3(card_table_alignment, space_alignment, page_size);
}

void HGCArguments::initialize_alignments() {
  CardTable::initialize_card_size();

  SpaceAlignment = os::vm_page_size();
  HeapAlignment  = calculate_heap_alignment(SpaceAlignment);
}

CollectedHeap* HGCArguments::create_heap() {
  return new HGCCollectedHeap();
}

bool HGCArguments::is_supported() const {
  return true;
}
