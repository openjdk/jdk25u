#ifndef SHARE_GC_H_HCARDSET_INLINE_HPP
#define SHARE_GC_H_HCARDSET_INLINE_HPP

#include "gc/h/hCardSet.hpp"

#include "gc/h/hCardSetContainers.inline.hpp"
#include "logging/log.hpp"

template <class T>
inline T* HGCCardSet::container_ptr(ContainerPtr ptr) {
  return (T*)strip_container_type(ptr);
}

inline HGCCardSet::ContainerPtr HGCCardSet::make_container_ptr(void* value, uintptr_t type) {
  assert(container_type(value) == 0, "Given ptr " PTR_FORMAT " already has type bits set", p2i(value));
  return (ContainerPtr)((uintptr_t)value | type);
}

template <class CardOrRangeVisitor>
inline void HGCCardSet::iterate_cards_or_ranges_in_container(ContainerPtr const container, CardOrRangeVisitor& cl) {
  switch (container_type(container)) {
    case ContainerInlinePtr: {
      if (cl.start_iterate(MergedInline)) {
        HGCCardSetInlinePtr ptr(container);
        ptr.iterate(cl, _config->inline_ptr_bits_per_card());
      }
      return;
    }
    case ContainerArrayOfCards: {
      if (cl.start_iterate(MergedArrayOfCards)) {
        container_ptr<HGCCardSetArray>(container)->iterate(cl);
      }
      return;
    }
    case ContainerBitMap: {
      ShouldNotReachHere();
      return;
    }
    case ContainerHowl: {
      assert(container_type(FullCardSet) == ContainerHowl, "Must be");
      if (container == FullCardSet) {
        if (cl.start_iterate(MergedFull)) {
          cl(0, _config->max_cards_in_region());
        }
        return;
      }
      if (cl.start_iterate(MergedHowl)) {
        container_ptr<HGCCardSetHowl>(container)->iterate(cl, _config);
      }
      return;
    }
  }
  log_error(gc)("Unknown card set container type %u", container_type(container));
  ShouldNotReachHere();
}

#endif
