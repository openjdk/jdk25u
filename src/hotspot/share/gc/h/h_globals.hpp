#ifndef SHARE_GC_H_H_GLOBALS_HPP
#define SHARE_GC_H_H_GLOBALS_HPP

#define MAXREGIONSIZE (512 * 1024 * 1024)

#define GC_H_FLAGS(develop,                         \
                   develop_pd,                       \
                   product,                           \
                   product_pd,                         \
                   range,                               \
                   constraint)                           \
                                                          \
  product(size_t, HGCMinArenaSize, 4 * K, EXPERIMENTAL,    \
          "Size of an HGC region, must be a power of two.") \
          range(4 * K, 128 * M)                              \
                                                              \
  product(size_t, HGCMaxArenaSize, 0, EXPERIMENTAL,            \
          "Size of an HGC region, must be a power of two.")     \
          range(0, MAXREGIONSIZE)                                \
                                                                  \
  product(size_t, HGCRegionSize, 2 * M, EXPERIMENTAL,              \
          "Size of an HGC cluster, must be a power of two. "        \
          "determined ergonomically.")                               \
          range(4 * K, MAXREGIONSIZE)                                \
                                                                     \
  product(uint, HGCCSetCPUMultiplier, 3, EXPERIMENTAL,               \
          "Collection-set target as a multiple of active processors " \
          "(4 when the ergonomic region target hits the 4M floor).")   \
          range(1, 64)                                                \
                                                                      \
  product(bool, HGCAdaptiveCSet, false, EXPERIMENTAL,                  \
          "Adapt collection-set limit to the inter-cycle interval.")   \
                                                                     \
  product(bool, HGCDumpHeapMap, false, EXPERIMENTAL,                 \
          "Dump full HGC heap map as JSON after each STW collection") \
                                                                      \
  product(uint, HGCRemSetArrayOfCardsEntries, 16, EXPERIMENTAL,       \
          "Maximum number of entries per Array of Cards card set container.") \
          range(1, 65536)                                             \
                                                                      \
  product(uint, HGCRemSetHowlNumBuckets, 8, EXPERIMENTAL,             \
          "Number of buckets per Howl card set container. Must be a power of two.") \
          range(1, 1024)                                              \
                                                                      \
  product(uint, HGCRemSetCoarsenHowlBitmapToHowlFullPercent, 90, EXPERIMENTAL, \
          "Percentage at which to coarsen a Howl bitmap to Howl full card set container.") \
          range(1, 100)                                               \
                                                                      \
  product(uint, HGCRemSetCoarsenHowlToFullPercent, 90, EXPERIMENTAL,  \
          "Percentage at which to coarsen a Howl card set to Full card set container.") \
          range(1, 100)                                               \
                                                                      \
  product(uint, HGCConcRefinementThreads, 1, EXPERIMENTAL,            \
          "The number of parallel remembered set update threads.")    \
          range(0, (max_jint-1)/wordSize)                              \
                                                                       \
  product(bool, HGCForceSTW, false, EXPERIMENTAL,                      \
          "Force full STW collections instead of concurrent cycles.")  \
                                                                       \
  product(bool, HGCUsePreBarrier, true, EXPERIMENTAL,                  \
          "Emit and execute HGC SATB pre-barriers.")                   \
                                                                       \
  product(bool, HGCUsePostBarrier, true, EXPERIMENTAL,                 \
          "Emit and execute HGC card post-barriers.")                  \
                                                                       \
  product(bool, HGCUseConcurrentRefinement, true, EXPERIMENTAL,        \
          "Run concurrent dirty-card refinement threads.")

#endif
