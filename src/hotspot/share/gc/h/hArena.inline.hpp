#ifndef SHARE_GC_H_HARENA_INLINE_HPP
#define SHARE_GC_H_HARENA_INLINE_HPP

#include "gc/h/hArena.hpp"

#include "logging/log.hpp"
#include "memory/memRegion.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/count_trailing_zeros.hpp"
#include "utilities/ticks.hpp"

extern THREAD_LOCAL oopDesc* hgc_diag_holder;

template <typename OopClosureType>
void HGCArena::process_gray(OopClosureType* cl) {
  BitMap::bm_word_t* gray = _gray_bitset.words();
  BitMap::bm_word_t* black = _black_bitset.words();
  const size_t words = _gray_bitset.size() / BitsPerWord;
  const BitMap::idx_t pending_size = _pending_word_bitset.size();
  assert(_gray_bitset.size() % BitsPerWord == 0, "bitset is word-aligned");
  log_trace(gc, mark)("HGC arena " PTR_FORMAT ": process_gray begin", p2i(_start));
  uint loops = 0;
  size_t objects = 0;
  BitMap::idx_t scan = 0;
  for (;;) {

    BitMap::idx_t w = _pending_word_bitset.find_first_set_bit(scan, pending_size);
    if (w >= pending_size) {
      if (_pending_word_bitset.is_empty()) {
        break;
      }
      scan = 0;
      continue;
    }
    scan = w + 1;
    _pending_word_bitset.par_clear_bit(w);
    ++loops;
    if ((loops & 0x3ff) == 0) {
      log_trace(gc, mark)("HGC arena " PTR_FORMAT ": process_gray still running, loops=%u",
                          p2i(_start), loops);
    }
    if (w >= words) {
      continue;
    }

    for (BitMap::bm_word_t todo = gray[w] & ~black[w]; todo != 0; todo &= todo - 1) {
      const BitMap::idx_t index = w * BitsPerWord + count_trailing_zeros(todo);
      oop obj = cast_to_oop(addr_of_offset(static_cast<Offset>(index)));
      if (hgc_verify_enabled()) {
        assert(oopDesc::is_oop(obj, true), "object is not parsable");
      }
      hgc_diag_holder = obj;
      ++objects;
      if (hgc_mark_diag_enabled()) {
        Ticks start = Ticks::now();
        obj->oop_iterate(cl);
        double elapsed_ms = (Ticks::now() - start).seconds() * 1000.0;
        if (elapsed_ms >= 1.0) {
          ResourceMark rm;
          log_info(gc, mark)("HGC arena " PTR_FORMAT ": slow object " PTR_FORMAT
                             " class=%s size=%zu offset=%zu time=%.3fms",
                             p2i(_start), p2i(obj), obj->klass()->external_name(),
                             obj->size() * HeapWordSize, (size_t)index, elapsed_ms);
        }
      } else {
        obj->oop_iterate(cl);
      }
      _black_bitset.par_set_bit(index);
    }
  }
  log_trace(gc, mark)("HGC arena " PTR_FORMAT ": process_gray end, loops=%u objects=%zu",
                      p2i(_start), loops, objects);
}

template <typename Closure>
void HGCArena::oops_black_do(Closure* cl) {
  BitMap::bm_word_t* black = _black_bitset.words();
  const size_t words = _black_bitset.size() / BitsPerWord;
  for (size_t w = 0; w < words; ++w) {
    for (BitMap::bm_word_t todo = black[w]; todo != 0; todo &= todo - 1) {
      const BitMap::idx_t index = w * BitsPerWord + count_trailing_zeros(todo);
      cl->do_holder(cast_to_oop(addr_of_offset(static_cast<Offset>(index))));
    }
  }
}

template <typename Closure>
void HGCArena::oops_starts_do(Closure* cl) {
  ensure_start_bitset();
  BitMap::bm_word_t* starts = _start_bitset.words();
  const size_t words = _start_bitset.size() / BitsPerWord;
  for (size_t w = 0; w < words; ++w) {
    for (BitMap::bm_word_t todo = starts[w]; todo != 0; todo &= todo - 1) {
      const BitMap::idx_t index = w * BitsPerWord + count_trailing_zeros(todo);
      cl->do_holder(cast_to_oop(addr_of_offset(static_cast<Offset>(index))));
    }
  }
}

template <typename OopClosureType>
void HGCArena::oops_on_memregion(HeapWord* start, HeapWord* end, OopClosureType* cl, bool only_marked) {
  ensure_start_bitset();
  BitMap::idx_t first = offset(start);
  BitMap::idx_t last = offset(end);
  MemRegion mr(start, end);

  BitMap::idx_t prev = _start_bitset.find_last_set_bit(0, first + 1);
  if (prev <= first) {
    oop obj = cast_to_oop(addr_of_offset(static_cast<Offset>(prev)));

    if (hgc_verify_enabled()) {
      assert(oopDesc::is_oop(obj, true), "corrupt object start map at " PTR_FORMAT, p2i(obj));
    }
    if ((!only_marked || is_black(cast_from_oop<HeapWord*>(obj)) || is_gray(cast_from_oop<HeapWord*>(obj))) &&
        cast_from_oop<HeapWord*>(obj) + obj->size() > start) {
      obj->oop_iterate(cl, mr);
    }
  }

  for (BitMap::idx_t i = _start_bitset.find_first_set_bit(first + 1, last);
       i < last;
       i = _start_bitset.find_first_set_bit(i + 1, last)) {
    oop obj = cast_to_oop(addr_of_offset(static_cast<Offset>(i)));
    if (hgc_verify_enabled()) {
      assert(oopDesc::is_oop(obj, true), "corrupt object start map at " PTR_FORMAT, p2i(obj));
    }
    if (only_marked && !is_black(cast_from_oop<HeapWord*>(obj)) && !is_gray(cast_from_oop<HeapWord*>(obj))) {
      continue;
    }
    if (cast_from_oop<HeapWord*>(obj) + obj->size() > end) {
      obj->oop_iterate(cl, mr);
    } else {
      obj->oop_iterate(cl);
    }
  }
}

#endif
