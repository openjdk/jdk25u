#ifndef SHARE_GC_H_HBARRIERSETRUNTIME_HPP
#define SHARE_GC_H_HBARRIERSETRUNTIME_HPP

#include "gc/shared/cardTable.hpp"
#include "memory/allStatic.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

class oopDesc;
class JavaThread;

class HGCBarrierSetRuntime: public AllStatic {
public:

  static void write_ref_array_pre_oop_entry(oop* dst, size_t length);
  static void write_ref_array_pre_narrow_oop_entry(narrowOop* dst, size_t length);

  static void write_ref_field_pre_entry(oopDesc* orig, JavaThread* thread);
  static void write_ref_field_pre_addr_entry(void* addr, JavaThread* thread);

  static void write_ref_field_post_entry(volatile CardTable::CardValue* card, JavaThread* thread);
  static void write_ref_field_post_addr_entry(void* addr);
  static void write_ref_array_post_entry(HeapWord* dst, size_t length, JavaThread* thread);
};

#endif
