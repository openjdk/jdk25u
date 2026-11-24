#ifndef SHARE_GC_H_HREGION_INLINE_HPP
#define SHARE_GC_H_HREGION_INLINE_HPP

#include "gc/h/hRegion.hpp"

template <typename Closure>
void HGCRegion::arenas_do(Closure* cl) {
  if (is_large()) {
    return;
  }
  for (size_t i = 0; i < (size_t(1) << _k); ++i) {
    cl->do_arena(arena_at(i));
  }
}

#endif
