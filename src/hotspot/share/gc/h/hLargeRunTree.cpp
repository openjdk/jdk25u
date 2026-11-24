#include "gc/h/hLargeRunTree.hpp"

HGCLargeRunTree::HGCLargeRunTree() :
    _by_size(nullptr),
    _by_addr(nullptr),
    _seed(0x9e3779b9u) {
}

HGCLargeRunTree::~HGCLargeRunTree() {
  clear_size(_by_size);
  clear_addr(_by_addr);
}

void HGCLargeRunTree::clear_size(SizeNode* t) {
  if (t == nullptr) {
    return;
  }
  clear_size(t->left);
  clear_size(t->right);
  delete t;
}

void HGCLargeRunTree::clear_addr(AddrNode* t) {
  if (t == nullptr) {
    return;
  }
  clear_addr(t->left);
  clear_addr(t->right);
  delete t;
}

HGCLargeRunTree::SizeNode* HGCLargeRunTree::merge_size(SizeNode* a, SizeNode* b) {
  if (a == nullptr) {
    return b;
  }
  if (b == nullptr) {
    return a;
  }
  if (a->prio >= b->prio) {
    a->right = merge_size(a->right, b);
    return a;
  }
  b->left = merge_size(a, b->left);
  return b;
}

void HGCLargeRunTree::split_size(SizeNode* t, size_t regions, size_t start,
                                 SizeNode** left, SizeNode** right) {
  if (t == nullptr) {
    *left = nullptr;
    *right = nullptr;
    return;
  }
  const bool is_left = t->regions < regions ||
                       (t->regions == regions && t->start < start);
  if (is_left) {
    split_size(t->right, regions, start, &t->right, right);
    *left = t;
  } else {
    split_size(t->left, regions, start, left, &t->left);
    *right = t;
  }
}

HGCLargeRunTree::SizeNode* HGCLargeRunTree::erase_size(SizeNode* t,
                                                       size_t regions,
                                                       size_t start) {
  if (t == nullptr) {
    return nullptr;
  }
  if (t->regions > regions || (t->regions == regions && t->start > start)) {
    t->left = erase_size(t->left, regions, start);
  } else if (t->regions < regions || t->start < start) {
    t->right = erase_size(t->right, regions, start);
  } else {
    SizeNode* merged = merge_size(t->left, t->right);
    delete t;
    return merged;
  }
  return t;
}

HGCLargeRunTree::AddrNode* HGCLargeRunTree::merge_addr(AddrNode* a, AddrNode* b) {
  if (a == nullptr) {
    return b;
  }
  if (b == nullptr) {
    return a;
  }
  if (a->prio >= b->prio) {
    a->right = merge_addr(a->right, b);
    return a;
  }
  b->left = merge_addr(a, b->left);
  return b;
}

void HGCLargeRunTree::split_addr(AddrNode* t, size_t start,
                                 AddrNode** left, AddrNode** right) {
  if (t == nullptr) {
    *left = nullptr;
    *right = nullptr;
    return;
  }
  if (t->start < start) {
    split_addr(t->right, start, &t->right, right);
    *left = t;
  } else {
    split_addr(t->left, start, left, &t->left);
    *right = t;
  }
}

HGCLargeRunTree::AddrNode* HGCLargeRunTree::erase_addr(AddrNode* t, size_t start) {
  if (t == nullptr) {
    return nullptr;
  }
  if (t->start > start) {
    t->left = erase_addr(t->left, start);
  } else if (t->start < start) {
    t->right = erase_addr(t->right, start);
  } else {
    AddrNode* merged = merge_addr(t->left, t->right);
    delete t;
    return merged;
  }
  return t;
}

bool HGCLargeRunTree::alloc_fit(size_t needed, size_t* start, size_t* regions) {
  SizeNode* t = _by_size;
  SizeNode* best = nullptr;
  while (t != nullptr) {
    if (t->regions >= needed) {
      best = t;
      t = t->left;
    } else {
      t = t->right;
    }
  }
  if (best == nullptr) {
    return false;
  }
  *start = best->start;
  *regions = best->regions;
  _by_size = erase_size(_by_size, best->regions, best->start);
  _by_addr = erase_addr(_by_addr, *start);
  return true;
}

void HGCLargeRunTree::add(size_t start, size_t regions) {
  AddrNode* pred = nullptr;
  AddrNode* succ = nullptr;
  for (AddrNode* t = _by_addr; t != nullptr; ) {
    if (t->start < start) {
      pred = t;
      t = t->right;
    } else {
      succ = t;
      t = t->left;
    }
  }

  size_t merged_start = start;
  size_t merged_regions = regions;

  if (pred != nullptr && pred->start + pred->regions == start) {
    const size_t ps = pred->start;
    const size_t pr = pred->regions;
    _by_size = erase_size(_by_size, pr, ps);
    _by_addr = erase_addr(_by_addr, ps);
    merged_start = ps;
    merged_regions += pr;
  }
  if (succ != nullptr && merged_start + merged_regions == succ->start) {
    const size_t ss = succ->start;
    const size_t sr = succ->regions;
    _by_size = erase_size(_by_size, sr, ss);
    _by_addr = erase_addr(_by_addr, ss);
    merged_regions += sr;
  }

  SizeNode* size_node = new SizeNode(merged_start, merged_regions, next_prio());
  SizeNode* size_left;
  SizeNode* size_right;
  split_size(_by_size, size_node->regions, size_node->start,
             &size_left, &size_right);
  _by_size = merge_size(merge_size(size_left, size_node), size_right);

  AddrNode* addr_node = new AddrNode(merged_start, merged_regions, next_prio());
  AddrNode* addr_left;
  AddrNode* addr_right;
  split_addr(_by_addr, addr_node->start, &addr_left, &addr_right);
  _by_addr = merge_addr(merge_addr(addr_left, addr_node), addr_right);
}
