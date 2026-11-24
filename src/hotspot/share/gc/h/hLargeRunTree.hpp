#ifndef SHARE_GC_H_HLARGERUNTREE_HPP
#define SHARE_GC_H_HLARGERUNTREE_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class HGCLargeRunTree : public CHeapObj<mtGC> {
  struct SizeNode : public CHeapObj<mtGC> {
    size_t start;
    size_t regions;
    uint32_t prio;
    SizeNode* left;
    SizeNode* right;

    SizeNode(size_t s, size_t r, uint32_t p) :
        start(s), regions(r), prio(p), left(nullptr), right(nullptr) {}
  };

  struct AddrNode : public CHeapObj<mtGC> {
    size_t start;
    size_t regions;
    uint32_t prio;
    AddrNode* left;
    AddrNode* right;

    AddrNode(size_t s, size_t r, uint32_t p) :
        start(s), regions(r), prio(p), left(nullptr), right(nullptr) {}
  };

  SizeNode* _by_size;
  AddrNode* _by_addr;
  uint32_t _seed;

  uint32_t next_prio() {
    _seed = _seed * 1664525u + 1013904223u;
    return _seed;
  }

  static SizeNode* merge_size(SizeNode* a, SizeNode* b);
  static void split_size(SizeNode* t, size_t regions, size_t start,
                         SizeNode** left, SizeNode** right);
  static SizeNode* erase_size(SizeNode* t, size_t regions, size_t start);

  static AddrNode* merge_addr(AddrNode* a, AddrNode* b);
  static void split_addr(AddrNode* t, size_t start,
                         AddrNode** left, AddrNode** right);
  static AddrNode* erase_addr(AddrNode* t, size_t start);

  static void clear_size(SizeNode* t);
  static void clear_addr(AddrNode* t);

public:
  HGCLargeRunTree();
  ~HGCLargeRunTree();

  NONCOPYABLE(HGCLargeRunTree);

  bool alloc_fit(size_t needed, size_t* start, size_t* regions);

  void add(size_t start, size_t regions);

  bool is_empty() const { return _by_addr == nullptr; }
};

#endif
