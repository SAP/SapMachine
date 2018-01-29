/*
 * Copyright (c) 2011, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "code/codeCache.hpp"
#include "code/scopeDesc.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/generateOopMap.hpp"
#include "oops/fieldStreams.hpp"
#include "oops/oop.inline.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "runtime/fieldDescriptor.hpp"
#include "runtime/javaCalls.hpp"
#include "jvmci/jvmciRuntime.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/compilerOracle.hpp"
#include "compiler/disassembler.hpp"
#include "compiler/oopMap.hpp"
#include "jvmci/jvmciCompilerToVM.hpp"
#include "jvmci/jvmciCompiler.hpp"
#include "jvmci/jvmciEnv.hpp"
#include "jvmci/jvmciJavaClasses.hpp"
#include "jvmci/jvmciCodeInstaller.hpp"
#include "jvmci/vmStructs_jvmci.hpp"
#include "gc/g1/heapRegion.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/vframe.hpp"
#include "runtime/vframe_hp.hpp"
#include "runtime/vmStructs.hpp"
#include "utilities/resourceHash.hpp"


void JNIHandleMark::push_jni_handle_block() {
  JavaThread* thread = JavaThread::current();
  if (thread != NULL) {
    // Allocate a new block for JNI handles.
    // Inlined code from jni_PushLocalFrame()
    JNIHandleBlock* java_handles = ((JavaThread*)thread)->active_handles();
    JNIHandleBlock* compile_handles = JNIHandleBlock::allocate_block(thread);
    assert(compile_handles != NULL && java_handles != NULL, "should not be NULL");
    compile_handles->set_pop_frame_link(java_handles);
    thread->set_active_handles(compile_handles);
  }
}

void JNIHandleMark::pop_jni_handle_block() {
  JavaThread* thread = JavaThread::current();
  if (thread != NULL) {
    // Release our JNI handle block
    JNIHandleBlock* compile_handles = thread->active_handles();
    JNIHandleBlock* java_handles = compile_handles->pop_frame_link();
    thread->set_active_handles(java_handles);
    compile_handles->set_pop_frame_link(NULL);
    JNIHandleBlock::release_block(compile_handles, thread); // may block
  }
}

// Entry to native method implementation that transitions current thread to '_thread_in_vm'.
#define C2V_VMENTRY(result_type, name, signature) \
  JNIEXPORT result_type JNICALL c2v_ ## name signature { \
  TRACE_jvmci_1("CompilerToVM::" #name); \
  TRACE_CALL(result_type, jvmci_ ## name signature) \
  JVMCI_VM_ENTRY_MARK; \

#define C2V_END }

oop CompilerToVM::get_jvmci_method(const methodHandle& method, TRAPS) {
  if (method() != NULL) {
    JavaValue result(T_OBJECT);
    JavaCallArguments args;
    args.push_long((jlong) (address) method());
    JavaCalls::call_static(&result, SystemDictionary::HotSpotResolvedJavaMethodImpl_klass(), vmSymbols::fromMetaspace_name(), vmSymbols::method_fromMetaspace_signature(), &args, CHECK_NULL);

    return (oop)result.get_jobject();
  }
  return NULL;
}

oop CompilerToVM::get_jvmci_type(Klass* klass, TRAPS) {
  if (klass != NULL) {
    JavaValue result(T_OBJECT);
    JavaCallArguments args;
    args.push_oop(Handle(THREAD, klass->java_mirror()));
    JavaCalls::call_static(&result, SystemDictionary::HotSpotResolvedObjectTypeImpl_klass(), vmSymbols::fromMetaspace_name(), vmSymbols::klass_fromMetaspace_signature(), &args, CHECK_NULL);

    return (oop)result.get_jobject();
  }
  return NULL;
}


int CompilerToVM::Data::Klass_vtable_start_offset;
int CompilerToVM::Data::Klass_vtable_length_offset;

int CompilerToVM::Data::Method_extra_stack_entries;

address CompilerToVM::Data::SharedRuntime_ic_miss_stub;
address CompilerToVM::Data::SharedRuntime_handle_wrong_method_stub;
address CompilerToVM::Data::SharedRuntime_deopt_blob_unpack;
address CompilerToVM::Data::SharedRuntime_deopt_blob_uncommon_trap;

size_t CompilerToVM::Data::ThreadLocalAllocBuffer_alignment_reserve;

CollectedHeap* CompilerToVM::Data::Universe_collectedHeap;
int CompilerToVM::Data::Universe_base_vtable_size;
address CompilerToVM::Data::Universe_narrow_oop_base;
int CompilerToVM::Data::Universe_narrow_oop_shift;
address CompilerToVM::Data::Universe_narrow_klass_base;
int CompilerToVM::Data::Universe_narrow_klass_shift;
void* CompilerToVM::Data::Universe_non_oop_bits;
uintptr_t CompilerToVM::Data::Universe_verify_oop_mask;
uintptr_t CompilerToVM::Data::Universe_verify_oop_bits;

bool       CompilerToVM::Data::_supports_inline_contig_alloc;
HeapWord** CompilerToVM::Data::_heap_end_addr;
HeapWord* volatile* CompilerToVM::Data::_heap_top_addr;
int CompilerToVM::Data::_max_oop_map_stack_offset;

jbyte* CompilerToVM::Data::cardtable_start_address;
int CompilerToVM::Data::cardtable_shift;

int CompilerToVM::Data::vm_page_size;

int CompilerToVM::Data::sizeof_vtableEntry = sizeof(vtableEntry);
int CompilerToVM::Data::sizeof_ExceptionTableElement = sizeof(ExceptionTableElement);
int CompilerToVM::Data::sizeof_LocalVariableTableElement = sizeof(LocalVariableTableElement);
int CompilerToVM::Data::sizeof_ConstantPool = sizeof(ConstantPool);
int CompilerToVM::Data::sizeof_SymbolPointer = sizeof(Symbol*);
int CompilerToVM::Data::sizeof_narrowKlass = sizeof(narrowKlass);
int CompilerToVM::Data::sizeof_arrayOopDesc = sizeof(arrayOopDesc);
int CompilerToVM::Data::sizeof_BasicLock = sizeof(BasicLock);

address CompilerToVM::Data::dsin;
address CompilerToVM::Data::dcos;
address CompilerToVM::Data::dtan;
address CompilerToVM::Data::dexp;
address CompilerToVM::Data::dlog;
address CompilerToVM::Data::dlog10;
address CompilerToVM::Data::dpow;

address CompilerToVM::Data::symbol_init;
address CompilerToVM::Data::symbol_clinit;

void CompilerToVM::Data::initialize(TRAPS) {
  Klass_vtable_start_offset = in_bytes(Klass::vtable_start_offset());
  Klass_vtable_length_offset = in_bytes(Klass::vtable_length_offset());

  Method_extra_stack_entries = Method::extra_stack_entries();

  SharedRuntime_ic_miss_stub = SharedRuntime::get_ic_miss_stub();
  SharedRuntime_handle_wrong_method_stub = SharedRuntime::get_handle_wrong_method_stub();
  SharedRuntime_deopt_blob_unpack = SharedRuntime::deopt_blob()->unpack();
  SharedRuntime_deopt_blob_uncommon_trap = SharedRuntime::deopt_blob()->uncommon_trap();

  ThreadLocalAllocBuffer_alignment_reserve = ThreadLocalAllocBuffer::alignment_reserve();

  Universe_collectedHeap = Universe::heap();
  Universe_base_vtable_size = Universe::base_vtable_size();
  Universe_narrow_oop_base = Universe::narrow_oop_base();
  Universe_narrow_oop_shift = Universe::narrow_oop_shift();
  Universe_narrow_klass_base = Universe::narrow_klass_base();
  Universe_narrow_klass_shift = Universe::narrow_klass_shift();
  Universe_non_oop_bits = Universe::non_oop_word();
  Universe_verify_oop_mask = Universe::verify_oop_mask();
  Universe_verify_oop_bits = Universe::verify_oop_bits();

  _supports_inline_contig_alloc = Universe::heap()->supports_inline_contig_alloc();
  _heap_end_addr = _supports_inline_contig_alloc ? Universe::heap()->end_addr() : (HeapWord**) -1;
  _heap_top_addr = _supports_inline_contig_alloc ? Universe::heap()->top_addr() : (HeapWord* volatile*) -1;

  _max_oop_map_stack_offset = (OopMapValue::register_mask - VMRegImpl::stack2reg(0)->value()) * VMRegImpl::stack_slot_size;
  int max_oop_map_stack_index = _max_oop_map_stack_offset / VMRegImpl::stack_slot_size;
  assert(OopMapValue::legal_vm_reg_name(VMRegImpl::stack2reg(max_oop_map_stack_index)), "should be valid");
  assert(!OopMapValue::legal_vm_reg_name(VMRegImpl::stack2reg(max_oop_map_stack_index + 1)), "should be invalid");

  symbol_init = (address) vmSymbols::object_initializer_name();
  symbol_clinit = (address) vmSymbols::class_initializer_name();

  BarrierSet* bs = Universe::heap()->barrier_set();
  if (bs->is_a(BarrierSet::CardTableModRef)) {
    jbyte* base = barrier_set_cast<CardTableModRefBS>(bs)->byte_map_base;
    assert(base != 0, "unexpected byte_map_base");
    cardtable_start_address = base;
    cardtable_shift = CardTableModRefBS::card_shift;
  } else {
    // No card mark barriers
    cardtable_start_address = 0;
    cardtable_shift = 0;
  }

  vm_page_size = os::vm_page_size();

#define SET_TRIGFUNC(name)                                      \
  if (StubRoutines::name() != NULL) {                           \
    name = StubRoutines::name();                                \
  } else {                                                      \
    name = CAST_FROM_FN_PTR(address, SharedRuntime::name);      \
  }

  SET_TRIGFUNC(dsin);
  SET_TRIGFUNC(dcos);
  SET_TRIGFUNC(dtan);
  SET_TRIGFUNC(dexp);
  SET_TRIGFUNC(dlog10);
  SET_TRIGFUNC(dlog);
  SET_TRIGFUNC(dpow);

#undef SET_TRIGFUNC
}

objArrayHandle CompilerToVM::initialize_intrinsics(TRAPS) {
  objArrayHandle vmIntrinsics = oopFactory::new_objArray_handle(VMIntrinsicMethod::klass(), (vmIntrinsics::ID_LIMIT - 1), CHECK_(objArrayHandle()));
  int index = 0;
  // The intrinsics for a class are usually adjacent to each other.
  // When they are, the string for the class name can be reused.
  vmSymbols::SID kls_sid = vmSymbols::NO_SID;
  Handle kls_str;
#define SID_ENUM(n) vmSymbols::VM_SYMBOL_ENUM_NAME(n)
#define VM_SYMBOL_TO_STRING(s) \
  java_lang_String::create_from_symbol(vmSymbols::symbol_at(SID_ENUM(s)), CHECK_(objArrayHandle()))
#define VM_INTRINSIC_INFO(id, kls, name, sig, ignore_fcode) {             \
    instanceHandle vmIntrinsicMethod = InstanceKlass::cast(VMIntrinsicMethod::klass())->allocate_instance_handle(CHECK_(objArrayHandle())); \
    if (kls_sid != SID_ENUM(kls)) {                                       \
      kls_str = VM_SYMBOL_TO_STRING(kls);                                 \
      kls_sid = SID_ENUM(kls);                                            \
    }                                                                     \
    Handle name_str = VM_SYMBOL_TO_STRING(name);                          \
    Handle sig_str = VM_SYMBOL_TO_STRING(sig);                            \
    VMIntrinsicMethod::set_declaringClass(vmIntrinsicMethod, kls_str());  \
    VMIntrinsicMethod::set_name(vmIntrinsicMethod, name_str());           \
    VMIntrinsicMethod::set_descriptor(vmIntrinsicMethod, sig_str());      \
    VMIntrinsicMethod::set_id(vmIntrinsicMethod, vmIntrinsics::id);       \
      vmIntrinsics->obj_at_put(index++, vmIntrinsicMethod());             \
  }

  VM_INTRINSICS_DO(VM_INTRINSIC_INFO, VM_SYMBOL_IGNORE, VM_SYMBOL_IGNORE, VM_SYMBOL_IGNORE, VM_ALIAS_IGNORE)
#undef SID_ENUM
#undef VM_SYMBOL_TO_STRING
#undef VM_INTRINSIC_INFO
  assert(index == vmIntrinsics::ID_LIMIT - 1, "must be");

  return vmIntrinsics;
}

/**
 * The set of VM flags known to be used.
 */
#define PREDEFINED_CONFIG_FLAGS(do_bool_flag, do_intx_flag, do_uintx_flag) \
  do_intx_flag(AllocateInstancePrefetchLines)                              \
  do_intx_flag(AllocatePrefetchDistance)                                   \
  do_intx_flag(AllocatePrefetchInstr)                                      \
  do_intx_flag(AllocatePrefetchLines)                                      \
  do_intx_flag(AllocatePrefetchStepSize)                                   \
  do_intx_flag(AllocatePrefetchStyle)                                      \
  do_intx_flag(BciProfileWidth)                                            \
  do_bool_flag(BootstrapJVMCI)                                             \
  do_bool_flag(CITime)                                                     \
  do_bool_flag(CITimeEach)                                                 \
  do_uintx_flag(CodeCacheSegmentSize)                                      \
  do_intx_flag(CodeEntryAlignment)                                         \
  do_bool_flag(CompactFields)                                              \
  NOT_PRODUCT(do_intx_flag(CompileTheWorldStartAt))                        \
  NOT_PRODUCT(do_intx_flag(CompileTheWorldStopAt))                         \
  do_intx_flag(ContendedPaddingWidth)                                      \
  do_bool_flag(DontCompileHugeMethods)                                     \
  do_bool_flag(EnableContended)                                            \
  do_intx_flag(FieldsAllocationStyle)                                      \
  do_bool_flag(FoldStableValues)                                           \
  do_bool_flag(ForceUnreachable)                                           \
  do_intx_flag(HugeMethodLimit)                                            \
  do_bool_flag(Inline)                                                     \
  do_intx_flag(JVMCICounterSize)                                           \
  do_bool_flag(JVMCIPrintProperties)                                       \
  do_bool_flag(JVMCIUseFastLocking)                                        \
  do_intx_flag(MethodProfileWidth)                                         \
  do_intx_flag(ObjectAlignmentInBytes)                                     \
  do_bool_flag(PrintInlining)                                              \
  do_bool_flag(ReduceInitialCardMarks)                                     \
  do_bool_flag(RestrictContended)                                          \
  do_intx_flag(StackReservedPages)                                         \
  do_intx_flag(StackShadowPages)                                           \
  do_bool_flag(TLABStats)                                                  \
  do_uintx_flag(TLABWasteIncrement)                                        \
  do_intx_flag(TypeProfileWidth)                                           \
  do_bool_flag(UseAESIntrinsics)                                           \
  X86_ONLY(do_intx_flag(UseAVX))                                           \
  do_bool_flag(UseBiasedLocking)                                           \
  do_bool_flag(UseCRC32Intrinsics)                                         \
  do_bool_flag(UseCompressedClassPointers)                                 \
  do_bool_flag(UseCompressedOops)                                          \
  do_bool_flag(UseConcMarkSweepGC)                                         \
  X86_ONLY(do_bool_flag(UseCountLeadingZerosInstruction))                  \
  X86_ONLY(do_bool_flag(UseCountTrailingZerosInstruction))                 \
  do_bool_flag(UseG1GC)                                                    \
  COMPILER2_PRESENT(do_bool_flag(UseMontgomeryMultiplyIntrinsic))          \
  COMPILER2_PRESENT(do_bool_flag(UseMontgomerySquareIntrinsic))            \
  COMPILER2_PRESENT(do_bool_flag(UseMulAddIntrinsic))                      \
  COMPILER2_PRESENT(do_bool_flag(UseMultiplyToLenIntrinsic))               \
  do_bool_flag(UsePopCountInstruction)                                     \
  do_bool_flag(UseSHA1Intrinsics)                                          \
  do_bool_flag(UseSHA256Intrinsics)                                        \
  do_bool_flag(UseSHA512Intrinsics)                                        \
  do_intx_flag(UseSSE)                                                     \
  COMPILER2_PRESENT(do_bool_flag(UseSquareToLenIntrinsic))                 \
  do_bool_flag(UseStackBanging)                                            \
  do_bool_flag(UseTLAB)                                                    \
  do_bool_flag(VerifyOops)                                                 \

#define BOXED_BOOLEAN(name, value) oop name = ((jboolean)(value) ? boxedTrue() : boxedFalse())
#define BOXED_DOUBLE(name, value) oop name; do { jvalue p; p.d = (jdouble) (value); name = java_lang_boxing_object::create(T_DOUBLE, &p, CHECK_NULL);} while(0)
#define BOXED_LONG(name, value) \
  oop name; \
  do { \
    jvalue p; p.j = (jlong) (value); \
    Handle* e = longs.get(p.j); \
    if (e == NULL) { \
      oop o = java_lang_boxing_object::create(T_LONG, &p, CHECK_NULL); \
      Handle h(THREAD, o); \
      longs.put(p.j, h); \
      name = h(); \
    } else { \
      name = (*e)(); \
    } \
  } while (0)

#define CSTRING_TO_JSTRING(name, value) \
  Handle name; \
  do { \
    if (value != NULL) { \
      Handle* e = strings.get(value); \
      if (e == NULL) { \
        Handle h = java_lang_String::create_from_str(value, CHECK_NULL); \
        strings.put(value, h); \
        name = h; \
      } else { \
        name = (*e); \
      } \
    } \
  } while (0)

C2V_VMENTRY(jobjectArray, readConfiguration, (JNIEnv *env))
  ResourceMark rm;
  HandleMark hm;

  // Used to canonicalize Long and String values.
  ResourceHashtable<jlong, Handle> longs;
  ResourceHashtable<const char*, Handle, &CompilerToVM::cstring_hash, &CompilerToVM::cstring_equals> strings;

  jvalue prim;
  prim.z = true;  oop boxedTrueOop =  java_lang_boxing_object::create(T_BOOLEAN, &prim, CHECK_NULL);
  Handle boxedTrue(THREAD, boxedTrueOop);
  prim.z = false; oop boxedFalseOop = java_lang_boxing_object::create(T_BOOLEAN, &prim, CHECK_NULL);
  Handle boxedFalse(THREAD, boxedFalseOop);

  CompilerToVM::Data::initialize(CHECK_NULL);

  VMField::klass()->initialize(CHECK_NULL);
  VMFlag::klass()->initialize(CHECK_NULL);
  VMIntrinsicMethod::klass()->initialize(CHECK_NULL);

  int len = JVMCIVMStructs::localHotSpotVMStructs_count();
  objArrayHandle vmFields = oopFactory::new_objArray_handle(VMField::klass(), len, CHECK_NULL);
  for (int i = 0; i < len ; i++) {
    VMStructEntry vmField = JVMCIVMStructs::localHotSpotVMStructs[i];
    instanceHandle vmFieldObj = InstanceKlass::cast(VMField::klass())->allocate_instance_handle(CHECK_NULL);
    size_t name_buf_len = strlen(vmField.typeName) + strlen(vmField.fieldName) + 2 /* "::" */;
    char* name_buf = NEW_RESOURCE_ARRAY_IN_THREAD(THREAD, char, name_buf_len + 1);
    sprintf(name_buf, "%s::%s", vmField.typeName, vmField.fieldName);
    CSTRING_TO_JSTRING(name, name_buf);
    CSTRING_TO_JSTRING(type, vmField.typeString);
    VMField::set_name(vmFieldObj, name());
    VMField::set_type(vmFieldObj, type());
    VMField::set_offset(vmFieldObj, vmField.offset);
    VMField::set_address(vmFieldObj, (jlong) vmField.address);
    if (vmField.isStatic && vmField.typeString != NULL) {
      if (strcmp(vmField.typeString, "bool") == 0) {
        BOXED_BOOLEAN(box, *(jbyte*) vmField.address);
        VMField::set_value(vmFieldObj, box);
      } else if (strcmp(vmField.typeString, "int") == 0 ||
                 strcmp(vmField.typeString, "jint") == 0) {
        BOXED_LONG(box, *(jint*) vmField.address);
        VMField::set_value(vmFieldObj, box);
      } else if (strcmp(vmField.typeString, "uint64_t") == 0) {
        BOXED_LONG(box, *(uint64_t*) vmField.address);
        VMField::set_value(vmFieldObj, box);
      } else if (strcmp(vmField.typeString, "address") == 0 ||
                 strcmp(vmField.typeString, "intptr_t") == 0 ||
                 strcmp(vmField.typeString, "uintptr_t") == 0 ||
                 strcmp(vmField.typeString, "OopHandle") == 0 ||
                 strcmp(vmField.typeString, "size_t") == 0 ||
                 // All foo* types are addresses.
                 vmField.typeString[strlen(vmField.typeString) - 1] == '*') {
        BOXED_LONG(box, *((address*) vmField.address));
        VMField::set_value(vmFieldObj, box);
      } else {
        JVMCI_ERROR_NULL("VM field %s has unsupported type %s", name_buf, vmField.typeString);
      }
    }
    vmFields->obj_at_put(i, vmFieldObj());
  }

  int ints_len = JVMCIVMStructs::localHotSpotVMIntConstants_count();
  int longs_len = JVMCIVMStructs::localHotSpotVMLongConstants_count();
  len = ints_len + longs_len;
  objArrayHandle vmConstants = oopFactory::new_objArray_handle(SystemDictionary::Object_klass(), len * 2, CHECK_NULL);
  int insert = 0;
  for (int i = 0; i < ints_len ; i++) {
    VMIntConstantEntry c = JVMCIVMStructs::localHotSpotVMIntConstants[i];
    CSTRING_TO_JSTRING(name, c.name);
    BOXED_LONG(value, c.value);
    vmConstants->obj_at_put(insert++, name());
    vmConstants->obj_at_put(insert++, value);
  }
  for (int i = 0; i < longs_len ; i++) {
    VMLongConstantEntry c = JVMCIVMStructs::localHotSpotVMLongConstants[i];
    CSTRING_TO_JSTRING(name, c.name);
    BOXED_LONG(value, c.value);
    vmConstants->obj_at_put(insert++, name());
    vmConstants->obj_at_put(insert++, value);
  }
  assert(insert == len * 2, "must be");

  len = JVMCIVMStructs::localHotSpotVMAddresses_count();
  objArrayHandle vmAddresses = oopFactory::new_objArray_handle(SystemDictionary::Object_klass(), len * 2, CHECK_NULL);
  for (int i = 0; i < len ; i++) {
    VMAddressEntry a = JVMCIVMStructs::localHotSpotVMAddresses[i];
    CSTRING_TO_JSTRING(name, a.name);
    BOXED_LONG(value, a.value);
    vmAddresses->obj_at_put(i * 2, name());
    vmAddresses->obj_at_put(i * 2 + 1, value);
  }

#define COUNT_FLAG(ignore) +1
#ifdef ASSERT
#define CHECK_FLAG(type, name) { \
  Flag* flag = Flag::find_flag(#name, strlen(#name), /*allow_locked*/ true, /* return_flag */ true); \
  assert(flag != NULL, "No such flag named " #name); \
  assert(flag->is_##type(), "Flag " #name " is not of type " #type); \
}
#else
#define CHECK_FLAG(type, name)
#endif

#define ADD_FLAG(type, name, convert) { \
  CHECK_FLAG(type, name) \
  instanceHandle vmFlagObj = InstanceKlass::cast(VMFlag::klass())->allocate_instance_handle(CHECK_NULL); \
  CSTRING_TO_JSTRING(fname, #name); \
  CSTRING_TO_JSTRING(ftype, #type); \
  VMFlag::set_name(vmFlagObj, fname()); \
  VMFlag::set_type(vmFlagObj, ftype()); \
  convert(value, name); \
  VMFlag::set_value(vmFlagObj, value); \
  vmFlags->obj_at_put(i++, vmFlagObj()); \
}
#define ADD_BOOL_FLAG(name)  ADD_FLAG(bool, name, BOXED_BOOLEAN)
#define ADD_INTX_FLAG(name)  ADD_FLAG(intx, name, BOXED_LONG)
#define ADD_UINTX_FLAG(name) ADD_FLAG(uintx, name, BOXED_LONG)

  len = 0 + PREDEFINED_CONFIG_FLAGS(COUNT_FLAG, COUNT_FLAG, COUNT_FLAG);
  objArrayHandle vmFlags = oopFactory::new_objArray_handle(VMFlag::klass(), len, CHECK_NULL);
  int i = 0;
  PREDEFINED_CONFIG_FLAGS(ADD_BOOL_FLAG, ADD_INTX_FLAG, ADD_UINTX_FLAG)

  objArrayHandle vmIntrinsics = CompilerToVM::initialize_intrinsics(CHECK_NULL);

  objArrayOop data = oopFactory::new_objArray(SystemDictionary::Object_klass(), 5, CHECK_NULL);
  data->obj_at_put(0, vmFields());
  data->obj_at_put(1, vmConstants());
  data->obj_at_put(2, vmAddresses());
  data->obj_at_put(3, vmFlags());
  data->obj_at_put(4, vmIntrinsics());

  return (jobjectArray) JNIHandles::make_local(THREAD, data);
#undef COUNT_FLAG
#undef ADD_FLAG
#undef ADD_BOOL_FLAG
#undef ADD_INTX_FLAG
#undef ADD_UINTX_FLAG
#undef CHECK_FLAG
C2V_END

C2V_VMENTRY(jobject, getFlagValue, (JNIEnv *, jobject c2vm, jobject name_handle))
#define RETURN_BOXED_LONG(value) oop box; jvalue p; p.j = (jlong) (value); box = java_lang_boxing_object::create(T_LONG, &p, CHECK_NULL); return JNIHandles::make_local(THREAD, box);
#define RETURN_BOXED_DOUBLE(value) oop box; jvalue p; p.d = (jdouble) (value); box = java_lang_boxing_object::create(T_DOUBLE, &p, CHECK_NULL); return JNIHandles::make_local(THREAD, box);
  Handle name(THREAD, JNIHandles::resolve(name_handle));
  if (name.is_null()) {
    THROW_0(vmSymbols::java_lang_NullPointerException());
  }
  ResourceMark rm;
  const char* cstring = java_lang_String::as_utf8_string(name());
  Flag* flag = Flag::find_flag(cstring, strlen(cstring), /* allow_locked */ true, /* return_flag */ true);
  if (flag == NULL) {
    return c2vm;
  }
  if (flag->is_bool()) {
    jvalue prim;
    prim.z = flag->get_bool();
    oop box = java_lang_boxing_object::create(T_BOOLEAN, &prim, CHECK_NULL);
    return JNIHandles::make_local(THREAD, box);
  } else if (flag->is_ccstr()) {
    Handle value = java_lang_String::create_from_str(flag->get_ccstr(), CHECK_NULL);
    return JNIHandles::make_local(THREAD, value());
  } else if (flag->is_intx()) {
    RETURN_BOXED_LONG(flag->get_intx());
  } else if (flag->is_int()) {
    RETURN_BOXED_LONG(flag->get_int());
  } else if (flag->is_uint()) {
    RETURN_BOXED_LONG(flag->get_uint());
  } else if (flag->is_uint64_t()) {
    RETURN_BOXED_LONG(flag->get_uint64_t());
  } else if (flag->is_size_t()) {
    RETURN_BOXED_LONG(flag->get_size_t());
  } else if (flag->is_uintx()) {
    RETURN_BOXED_LONG(flag->get_uintx());
  } else if (flag->is_double()) {
    RETURN_BOXED_DOUBLE(flag->get_double());
  } else {
    JVMCI_ERROR_NULL("VM flag %s has unsupported type %s", flag->_name, flag->_type);
  }
C2V_END

#undef BOXED_LONG
#undef BOXED_DOUBLE
#undef CSTRING_TO_JSTRING

C2V_VMENTRY(jbyteArray, getBytecode, (JNIEnv *, jobject, jobject jvmci_method))
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  ResourceMark rm;

  int code_size = method->code_size();
  typeArrayOop reconstituted_code = oopFactory::new_byteArray(code_size, CHECK_NULL);

  guarantee(method->method_holder()->is_rewritten(), "Method's holder should be rewritten");
  // iterate over all bytecodes and replace non-Java bytecodes

  for (BytecodeStream s(method); s.next() != Bytecodes::_illegal; ) {
    Bytecodes::Code code = s.code();
    Bytecodes::Code raw_code = s.raw_code();
    int bci = s.bci();
    int len = s.instruction_size();

    // Restore original byte code.
    reconstituted_code->byte_at_put(bci, (jbyte) (s.is_wide()? Bytecodes::_wide : code));
    if (len > 1) {
      memcpy(reconstituted_code->byte_at_addr(bci + 1), s.bcp()+1, len-1);
    }

    if (len > 1) {
      // Restore the big-endian constant pool indexes.
      // Cf. Rewriter::scan_method
      switch (code) {
        case Bytecodes::_getstatic:
        case Bytecodes::_putstatic:
        case Bytecodes::_getfield:
        case Bytecodes::_putfield:
        case Bytecodes::_invokevirtual:
        case Bytecodes::_invokespecial:
        case Bytecodes::_invokestatic:
        case Bytecodes::_invokeinterface:
        case Bytecodes::_invokehandle: {
          int cp_index = Bytes::get_native_u2((address) reconstituted_code->byte_at_addr(bci + 1));
          Bytes::put_Java_u2((address) reconstituted_code->byte_at_addr(bci + 1), (u2) cp_index);
          break;
        }

        case Bytecodes::_invokedynamic: {
          int cp_index = Bytes::get_native_u4((address) reconstituted_code->byte_at_addr(bci + 1));
          Bytes::put_Java_u4((address) reconstituted_code->byte_at_addr(bci + 1), (u4) cp_index);
          break;
        }

        default:
          break;
      }

      // Not all ldc byte code are rewritten.
      switch (raw_code) {
        case Bytecodes::_fast_aldc: {
          int cpc_index = reconstituted_code->byte_at(bci + 1) & 0xff;
          int cp_index = method->constants()->object_to_cp_index(cpc_index);
          assert(cp_index < method->constants()->length(), "sanity check");
          reconstituted_code->byte_at_put(bci + 1, (jbyte) cp_index);
          break;
        }

        case Bytecodes::_fast_aldc_w: {
          int cpc_index = Bytes::get_native_u2((address) reconstituted_code->byte_at_addr(bci + 1));
          int cp_index = method->constants()->object_to_cp_index(cpc_index);
          assert(cp_index < method->constants()->length(), "sanity check");
          Bytes::put_Java_u2((address) reconstituted_code->byte_at_addr(bci + 1), (u2) cp_index);
          break;
        }

        default:
          break;
      }
    }
  }

  return (jbyteArray) JNIHandles::make_local(THREAD, reconstituted_code);
C2V_END

C2V_VMENTRY(jint, getExceptionTableLength, (JNIEnv *, jobject, jobject jvmci_method))
  ResourceMark rm;
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  return method->exception_table_length();
C2V_END

C2V_VMENTRY(jlong, getExceptionTableStart, (JNIEnv *, jobject, jobject jvmci_method))
  ResourceMark rm;
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  if (method->exception_table_length() == 0) {
    return 0L;
  }
  return (jlong) (address) method->exception_table_start();
C2V_END

C2V_VMENTRY(jobject, asResolvedJavaMethod, (JNIEnv *, jobject, jobject executable_handle))
  oop executable = JNIHandles::resolve(executable_handle);
  oop mirror = NULL;
  int slot = 0;

  if (executable->klass() == SystemDictionary::reflect_Constructor_klass()) {
    mirror = java_lang_reflect_Constructor::clazz(executable);
    slot = java_lang_reflect_Constructor::slot(executable);
  } else {
    assert(executable->klass() == SystemDictionary::reflect_Method_klass(), "wrong type");
    mirror = java_lang_reflect_Method::clazz(executable);
    slot = java_lang_reflect_Method::slot(executable);
  }
  Klass* holder = java_lang_Class::as_Klass(mirror);
  methodHandle method = InstanceKlass::cast(holder)->method_with_idnum(slot);
  oop result = CompilerToVM::get_jvmci_method(method, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
}

C2V_VMENTRY(jobject, getResolvedJavaMethod, (JNIEnv *, jobject, jobject base, jlong offset))
  methodHandle method;
  oop base_object = JNIHandles::resolve(base);
  if (base_object == NULL) {
    method = *((Method**)(offset));
  } else if (base_object->is_a(SystemDictionary::ResolvedMethodName_klass())) {
    method = (Method*) (intptr_t) base_object->long_field(offset);
  } else if (base_object->is_a(SystemDictionary::HotSpotResolvedJavaMethodImpl_klass())) {
    method = *((Method**)(HotSpotResolvedJavaMethodImpl::metaspaceMethod(base_object) + offset));
  } else {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Unexpected type: %s", base_object->klass()->external_name()));
  }
  assert (method.is_null() || method->is_method(), "invalid read");
  oop result = CompilerToVM::get_jvmci_method(method, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
}

C2V_VMENTRY(jobject, getConstantPool, (JNIEnv *, jobject, jobject object_handle))
  constantPoolHandle cp;
  oop object = JNIHandles::resolve(object_handle);
  if (object == NULL) {
    THROW_0(vmSymbols::java_lang_NullPointerException());
  }
  if (object->is_a(SystemDictionary::HotSpotResolvedJavaMethodImpl_klass())) {
    cp = CompilerToVM::asMethod(object)->constMethod()->constants();
  } else if (object->is_a(SystemDictionary::HotSpotResolvedObjectTypeImpl_klass())) {
    cp = InstanceKlass::cast(CompilerToVM::asKlass(object))->constants();
  } else {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Unexpected type: %s", object->klass()->external_name()));
  }
  assert(!cp.is_null(), "npe");
  JavaValue method_result(T_OBJECT);
  JavaCallArguments args;
  args.push_long((jlong) (address) cp());
  JavaCalls::call_static(&method_result, SystemDictionary::HotSpotConstantPool_klass(), vmSymbols::fromMetaspace_name(), vmSymbols::constantPool_fromMetaspace_signature(), &args, CHECK_NULL);
  return JNIHandles::make_local(THREAD, (oop)method_result.get_jobject());
}

C2V_VMENTRY(jobject, getResolvedJavaType, (JNIEnv *, jobject, jobject base, jlong offset, jboolean compressed))
  Klass* klass = NULL;
  oop base_object = JNIHandles::resolve(base);
  jlong base_address = 0;
  if (base_object != NULL && offset == oopDesc::klass_offset_in_bytes()) {
    klass = base_object->klass();
  } else if (!compressed) {
    if (base_object != NULL) {
      if (base_object->is_a(SystemDictionary::HotSpotResolvedJavaMethodImpl_klass())) {
        base_address = HotSpotResolvedJavaMethodImpl::metaspaceMethod(base_object);
      } else if (base_object->is_a(SystemDictionary::HotSpotConstantPool_klass())) {
        base_address = HotSpotConstantPool::metaspaceConstantPool(base_object);
      } else if (base_object->is_a(SystemDictionary::HotSpotResolvedObjectTypeImpl_klass())) {
        base_address = (jlong) CompilerToVM::asKlass(base_object);
      } else if (base_object->is_a(SystemDictionary::Class_klass())) {
        base_address = (jlong) (address) base_object;
      } else {
        THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
                    err_msg("Unexpected arguments: %s " JLONG_FORMAT " %s", base_object->klass()->external_name(), offset, compressed ? "true" : "false"));
      }
    }
    klass = *((Klass**) (intptr_t) (base_address + offset));
  } else {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Unexpected arguments: %s " JLONG_FORMAT " %s", base_object->klass()->external_name(), offset, compressed ? "true" : "false"));
  }
  assert (klass == NULL || klass->is_klass(), "invalid read");
  oop result = CompilerToVM::get_jvmci_type(klass, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
}

C2V_VMENTRY(jobject, findUniqueConcreteMethod, (JNIEnv *, jobject, jobject jvmci_type, jobject jvmci_method))
  ResourceMark rm;
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  Klass* holder = CompilerToVM::asKlass(jvmci_type);
  if (holder->is_interface()) {
    THROW_MSG_0(vmSymbols::java_lang_InternalError(), err_msg("Interface %s should be handled in Java code", holder->external_name()));
  }

  methodHandle ucm;
  {
    MutexLocker locker(Compile_lock);
    ucm = Dependencies::find_unique_concrete_method(holder, method());
  }
  oop result = CompilerToVM::get_jvmci_method(ucm, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jobject, getImplementor, (JNIEnv *, jobject, jobject jvmci_type))
  Klass* klass = CompilerToVM::asKlass(jvmci_type);
  if (!klass->is_interface()) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
        err_msg("Expected interface type, got %s", klass->external_name()));
  }
  InstanceKlass* iklass = InstanceKlass::cast(klass);
  oop implementor = CompilerToVM::get_jvmci_type(iklass->implementor(), CHECK_NULL);
  return JNIHandles::make_local(THREAD, implementor);
C2V_END

C2V_VMENTRY(jboolean, methodIsIgnoredBySecurityStackWalk,(JNIEnv *, jobject, jobject jvmci_method))
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  return method->is_ignored_by_security_stack_walk();
C2V_END

C2V_VMENTRY(jboolean, isCompilable,(JNIEnv *, jobject, jobject jvmci_method))
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  // Skip redefined methods
  if (method->is_old()) {
    return false;
  }
  return !method->is_not_compilable(CompLevel_full_optimization);
C2V_END

C2V_VMENTRY(jboolean, hasNeverInlineDirective,(JNIEnv *, jobject, jobject jvmci_method))
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  return !Inline || CompilerOracle::should_not_inline(method) || method->dont_inline();
C2V_END

C2V_VMENTRY(jboolean, shouldInlineMethod,(JNIEnv *, jobject, jobject jvmci_method))
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  return CompilerOracle::should_inline(method) || method->force_inline();
C2V_END

C2V_VMENTRY(jobject, lookupType, (JNIEnv*, jobject, jstring jname, jclass accessing_class, jboolean resolve))
  ResourceMark rm;
  Handle name(THREAD, JNIHandles::resolve(jname));
  Symbol* class_name = java_lang_String::as_symbol(name(), CHECK_0);
  if (java_lang_String::length(name()) <= 1) {
    THROW_MSG_0(vmSymbols::java_lang_InternalError(), err_msg("Primitive type %s should be handled in Java code", class_name->as_C_string()));
  }

  Klass* resolved_klass = NULL;
  if (JNIHandles::resolve(accessing_class) == NULL) {
    THROW_0(vmSymbols::java_lang_NullPointerException());
  }
  Klass* accessing_klass = java_lang_Class::as_Klass(JNIHandles::resolve(accessing_class));
  Handle class_loader(THREAD, accessing_klass->class_loader());
  Handle protection_domain(THREAD, accessing_klass->protection_domain());

  if (resolve) {
    resolved_klass = SystemDictionary::resolve_or_null(class_name, class_loader, protection_domain, CHECK_0);
  } else {
    if (class_name->byte_at(0) == 'L' &&
      class_name->byte_at(class_name->utf8_length()-1) == ';') {
      // This is a name from a signature.  Strip off the trimmings.
      // Call recursive to keep scope of strippedsym.
      TempNewSymbol strippedsym = SymbolTable::new_symbol(class_name->as_utf8()+1,
                                                          class_name->utf8_length()-2,
                                                          CHECK_0);
      resolved_klass = SystemDictionary::find(strippedsym, class_loader, protection_domain, CHECK_0);
    } else if (FieldType::is_array(class_name)) {
      FieldArrayInfo fd;
      // dimension and object_key in FieldArrayInfo are assigned as a side-effect
      // of this call
      BasicType t = FieldType::get_array_info(class_name, fd, CHECK_0);
      if (t == T_OBJECT) {
        TempNewSymbol strippedsym = SymbolTable::new_symbol(class_name->as_utf8()+1+fd.dimension(),
                                                            class_name->utf8_length()-2-fd.dimension(),
                                                            CHECK_0);
        // naked oop "k" is OK here -- we assign back into it
        resolved_klass = SystemDictionary::find(strippedsym,
                                                             class_loader,
                                                             protection_domain,
                                                             CHECK_0);
        if (resolved_klass != NULL) {
          resolved_klass = resolved_klass->array_klass(fd.dimension(), CHECK_0);
        }
      } else {
        resolved_klass = Universe::typeArrayKlassObj(t);
        resolved_klass = TypeArrayKlass::cast(resolved_klass)->array_klass(fd.dimension(), CHECK_0);
      }
    }
  }
  oop result = CompilerToVM::get_jvmci_type(resolved_klass, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jobject, resolveConstantInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  oop result = cp->resolve_constant_at(index, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jobject, resolvePossiblyCachedConstantInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  oop result = cp->resolve_possibly_cached_constant_at(index, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jint, lookupNameAndTypeRefIndexInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  return cp->name_and_type_ref_index_at(index);
C2V_END

C2V_VMENTRY(jobject, lookupNameInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint which))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  Handle sym = java_lang_String::create_from_symbol(cp->name_ref_at(which), CHECK_NULL);
  return JNIHandles::make_local(THREAD, sym());
C2V_END

C2V_VMENTRY(jobject, lookupSignatureInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint which))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  Handle sym = java_lang_String::create_from_symbol(cp->signature_ref_at(which), CHECK_NULL);
  return JNIHandles::make_local(THREAD, sym());
C2V_END

C2V_VMENTRY(jint, lookupKlassRefIndexInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  return cp->klass_ref_index_at(index);
C2V_END

C2V_VMENTRY(jobject, resolveTypeInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  Klass* resolved_klass = cp->klass_at(index, CHECK_NULL);
  oop klass = CompilerToVM::get_jvmci_type(resolved_klass, CHECK_NULL);
  return JNIHandles::make_local(THREAD, klass);
C2V_END

C2V_VMENTRY(jobject, lookupKlassInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index, jbyte opcode))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  Klass* loading_klass = cp->pool_holder();
  bool is_accessible = false;
  Klass* klass = JVMCIEnv::get_klass_by_index(cp, index, is_accessible, loading_klass);
  Symbol* symbol = NULL;
  if (klass == NULL) {
    symbol = cp->klass_name_at(index);
  }
  oop result_oop;
  if (klass != NULL) {
    result_oop = CompilerToVM::get_jvmci_type(klass, CHECK_NULL);
  } else {
    Handle result = java_lang_String::create_from_symbol(symbol, CHECK_NULL);
    result_oop = result();
  }
  return JNIHandles::make_local(THREAD, result_oop);
C2V_END

C2V_VMENTRY(jobject, lookupAppendixInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  oop appendix_oop = ConstantPool::appendix_at_if_loaded(cp, index);
  return JNIHandles::make_local(THREAD, appendix_oop);
C2V_END

C2V_VMENTRY(jobject, lookupMethodInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index, jbyte opcode))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  InstanceKlass* pool_holder = cp->pool_holder();
  Bytecodes::Code bc = (Bytecodes::Code) (((int) opcode) & 0xFF);
  methodHandle method = JVMCIEnv::get_method_by_index(cp, index, bc, pool_holder);
  oop result = CompilerToVM::get_jvmci_method(method, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jint, constantPoolRemapInstructionOperandFromCache, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  return cp->remap_instruction_operand_from_cache(index);
C2V_END

C2V_VMENTRY(jobject, resolveFieldInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index, jobject jvmci_method, jbyte opcode, jintArray info_handle))
  ResourceMark rm;
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  Bytecodes::Code code = (Bytecodes::Code)(((int) opcode) & 0xFF);
  fieldDescriptor fd;
  LinkInfo link_info(cp, index, (jvmci_method != NULL) ? CompilerToVM::asMethod(jvmci_method) : NULL, CHECK_0);
  LinkResolver::resolve_field(fd, link_info, Bytecodes::java_code(code), false, CHECK_0);
  typeArrayOop info = (typeArrayOop) JNIHandles::resolve(info_handle);
  if (info == NULL || info->length() != 3) {
    JVMCI_ERROR_NULL("info must not be null and have a length of 3");
  }
  info->int_at_put(0, fd.access_flags().as_int());
  info->int_at_put(1, fd.offset());
  info->int_at_put(2, fd.index());
  oop field_holder = CompilerToVM::get_jvmci_type(fd.field_holder(), CHECK_NULL);
  return JNIHandles::make_local(THREAD, field_holder);
C2V_END

C2V_VMENTRY(jint, getVtableIndexForInterfaceMethod, (JNIEnv *, jobject, jobject jvmci_type, jobject jvmci_method))
  ResourceMark rm;
  Klass* klass = CompilerToVM::asKlass(jvmci_type);
  Method* method = CompilerToVM::asMethod(jvmci_method);
  if (klass->is_interface()) {
    THROW_MSG_0(vmSymbols::java_lang_InternalError(), err_msg("Interface %s should be handled in Java code", klass->external_name()));
  }
  if (!method->method_holder()->is_interface()) {
    THROW_MSG_0(vmSymbols::java_lang_InternalError(), err_msg("Method %s is not held by an interface, this case should be handled in Java code", method->name_and_sig_as_C_string()));
  }
  if (!InstanceKlass::cast(klass)->is_linked()) {
    THROW_MSG_0(vmSymbols::java_lang_InternalError(), err_msg("Class %s must be linked", klass->external_name()));
  }
  return LinkResolver::vtable_index_of_interface_method(klass, method);
C2V_END

C2V_VMENTRY(jobject, resolveMethod, (JNIEnv *, jobject, jobject receiver_jvmci_type, jobject jvmci_method, jobject caller_jvmci_type))
  Klass* recv_klass = CompilerToVM::asKlass(receiver_jvmci_type);
  Klass* caller_klass = CompilerToVM::asKlass(caller_jvmci_type);
  methodHandle method = CompilerToVM::asMethod(jvmci_method);

  Klass* resolved     = method->method_holder();
  Symbol* h_name      = method->name();
  Symbol* h_signature = method->signature();

  if (MethodHandles::is_signature_polymorphic_method(method())) {
      // Signature polymorphic methods are already resolved, JVMCI just returns NULL in this case.
      return NULL;
  }

  LinkInfo link_info(resolved, h_name, h_signature, caller_klass);
  methodHandle m;
  // Only do exact lookup if receiver klass has been linked.  Otherwise,
  // the vtable has not been setup, and the LinkResolver will fail.
  if (recv_klass->is_array_klass() ||
      (InstanceKlass::cast(recv_klass)->is_linked() && !recv_klass->is_interface())) {
    if (resolved->is_interface()) {
      m = LinkResolver::resolve_interface_call_or_null(recv_klass, link_info);
    } else {
      m = LinkResolver::resolve_virtual_call_or_null(recv_klass, link_info);
    }
  }

  if (m.is_null()) {
    // Return NULL if there was a problem with lookup (uninitialized class, etc.)
    return NULL;
  }

  oop result = CompilerToVM::get_jvmci_method(m, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jboolean, hasFinalizableSubclass,(JNIEnv *, jobject, jobject jvmci_type))
  Klass* klass = CompilerToVM::asKlass(jvmci_type);
  assert(klass != NULL, "method must not be called for primitive types");
  return Dependencies::find_finalizable_subclass(klass) != NULL;
C2V_END

C2V_VMENTRY(jobject, getClassInitializer, (JNIEnv *, jobject, jobject jvmci_type))
  Klass* klass = CompilerToVM::asKlass(jvmci_type);
  if (!klass->is_instance_klass()) {
    return NULL;
  }
  InstanceKlass* iklass = InstanceKlass::cast(klass);
  oop result = CompilerToVM::get_jvmci_method(iklass->class_initializer(), CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jlong, getMaxCallTargetOffset, (JNIEnv*, jobject, jlong addr))
  address target_addr = (address) addr;
  if (target_addr != 0x0) {
    int64_t off_low = (int64_t)target_addr - ((int64_t)CodeCache::low_bound() + sizeof(int));
    int64_t off_high = (int64_t)target_addr - ((int64_t)CodeCache::high_bound() + sizeof(int));
    return MAX2(ABS(off_low), ABS(off_high));
  }
  return -1;
C2V_END

C2V_VMENTRY(void, setNotInlinableOrCompilable,(JNIEnv *, jobject,  jobject jvmci_method))
  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  method->set_not_c1_compilable();
  method->set_not_c2_compilable();
  method->set_dont_inline(true);
C2V_END

C2V_VMENTRY(jint, installCode, (JNIEnv *jniEnv, jobject, jobject target, jobject compiled_code, jobject installed_code, jobject speculation_log))
  ResourceMark rm;
  HandleMark hm;
  JNIHandleMark jni_hm;

  Handle target_handle(THREAD, JNIHandles::resolve(target));
  Handle compiled_code_handle(THREAD, JNIHandles::resolve(compiled_code));
  CodeBlob* cb = NULL;
  Handle installed_code_handle(THREAD, JNIHandles::resolve(installed_code));
  Handle speculation_log_handle(THREAD, JNIHandles::resolve(speculation_log));

  JVMCICompiler* compiler = JVMCICompiler::instance(true, CHECK_JNI_ERR);

  TraceTime install_time("installCode", JVMCICompiler::codeInstallTimer());
  bool is_immutable_PIC = HotSpotCompiledCode::isImmutablePIC(compiled_code_handle) > 0;
  CodeInstaller installer(is_immutable_PIC);
  JVMCIEnv::CodeInstallResult result = installer.install(compiler, target_handle, compiled_code_handle, cb, installed_code_handle, speculation_log_handle, CHECK_0);

  if (PrintCodeCacheOnCompilation) {
    stringStream s;
    // Dump code cache  into a buffer before locking the tty,
    {
      MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
      CodeCache::print_summary(&s, false);
    }
    ttyLocker ttyl;
    tty->print_raw_cr(s.as_string());
  }

  if (result != JVMCIEnv::ok) {
    assert(cb == NULL, "should be");
  } else {
    if (installed_code_handle.not_null()) {
      assert(installed_code_handle->is_a(InstalledCode::klass()), "wrong type");
      nmethod::invalidate_installed_code(installed_code_handle, CHECK_0);
      {
        // Ensure that all updates to the InstalledCode fields are consistent.
        MutexLockerEx pl(Patching_lock, Mutex::_no_safepoint_check_flag);
        InstalledCode::set_address(installed_code_handle, (jlong) cb);
        InstalledCode::set_version(installed_code_handle, InstalledCode::version(installed_code_handle) + 1);
        if (cb->is_nmethod()) {
          InstalledCode::set_entryPoint(installed_code_handle, (jlong) cb->as_nmethod_or_null()->verified_entry_point());
        } else {
          InstalledCode::set_entryPoint(installed_code_handle, (jlong) cb->code_begin());
        }
        if (installed_code_handle->is_a(HotSpotInstalledCode::klass())) {
          HotSpotInstalledCode::set_size(installed_code_handle, cb->size());
          HotSpotInstalledCode::set_codeStart(installed_code_handle, (jlong) cb->code_begin());
          HotSpotInstalledCode::set_codeSize(installed_code_handle, cb->code_size());
        }
      }
    }
  }
  return result;
C2V_END

C2V_VMENTRY(jint, getMetadata, (JNIEnv *jniEnv, jobject, jobject target, jobject compiled_code, jobject metadata))
  ResourceMark rm;
  HandleMark hm;

  Handle target_handle(THREAD, JNIHandles::resolve(target));
  Handle compiled_code_handle(THREAD, JNIHandles::resolve(compiled_code));
  Handle metadata_handle(THREAD, JNIHandles::resolve(metadata));

  CodeMetadata code_metadata;
  CodeBlob *cb = NULL;
  CodeInstaller installer(true /* immutable PIC compilation */);

  JVMCIEnv::CodeInstallResult result = installer.gather_metadata(target_handle, compiled_code_handle, code_metadata, CHECK_0);
  if (result != JVMCIEnv::ok) {
    return result;
  }

  if (code_metadata.get_nr_pc_desc() > 0) {
    typeArrayHandle pcArrayOop = oopFactory::new_byteArray_handle(sizeof(PcDesc) * code_metadata.get_nr_pc_desc(), CHECK_(JVMCIEnv::cache_full));
    memcpy(pcArrayOop->byte_at_addr(0), code_metadata.get_pc_desc(), sizeof(PcDesc) * code_metadata.get_nr_pc_desc());
    HotSpotMetaData::set_pcDescBytes(metadata_handle, pcArrayOop());
  }

  if (code_metadata.get_scopes_size() > 0) {
    typeArrayHandle scopesArrayOop = oopFactory::new_byteArray_handle(code_metadata.get_scopes_size(), CHECK_(JVMCIEnv::cache_full));
    memcpy(scopesArrayOop->byte_at_addr(0), code_metadata.get_scopes_desc(), code_metadata.get_scopes_size());
    HotSpotMetaData::set_scopesDescBytes(metadata_handle, scopesArrayOop());
  }

  RelocBuffer* reloc_buffer = code_metadata.get_reloc_buffer();
  typeArrayHandle relocArrayOop = oopFactory::new_byteArray_handle((int) reloc_buffer->size(), CHECK_(JVMCIEnv::cache_full));
  if (reloc_buffer->size() > 0) {
    memcpy(relocArrayOop->byte_at_addr(0), reloc_buffer->begin(), reloc_buffer->size());
  }
  HotSpotMetaData::set_relocBytes(metadata_handle, relocArrayOop());

  const OopMapSet* oopMapSet = installer.oopMapSet();
  {
    ResourceMark mark;
    ImmutableOopMapBuilder builder(oopMapSet);
    int oopmap_size = builder.heap_size();
    typeArrayHandle oopMapArrayHandle = oopFactory::new_byteArray_handle(oopmap_size, CHECK_(JVMCIEnv::cache_full));
    builder.generate_into((address) oopMapArrayHandle->byte_at_addr(0));
    HotSpotMetaData::set_oopMaps(metadata_handle, oopMapArrayHandle());
  }

  AOTOopRecorder* recorder = code_metadata.get_oop_recorder();

  int nr_meta_refs = recorder->nr_meta_refs();
  objArrayOop metadataArray = oopFactory::new_objectArray(nr_meta_refs, CHECK_(JVMCIEnv::cache_full));
  objArrayHandle metadataArrayHandle(THREAD, metadataArray);
  for (int i = 0; i < nr_meta_refs; ++i) {
    jobject element = recorder->meta_element(i);
    if (element == NULL) {
      return JVMCIEnv::cache_full;
    }
    metadataArrayHandle->obj_at_put(i, JNIHandles::resolve(element));
  }
  HotSpotMetaData::set_metadata(metadata_handle, metadataArrayHandle());

  ExceptionHandlerTable* handler = code_metadata.get_exception_table();
  int table_size = handler->size_in_bytes();
  typeArrayHandle exceptionArrayOop = oopFactory::new_byteArray_handle(table_size, CHECK_(JVMCIEnv::cache_full));

  if (table_size > 0) {
    handler->copy_bytes_to((address) exceptionArrayOop->byte_at_addr(0));
  }
  HotSpotMetaData::set_exceptionBytes(metadata_handle, exceptionArrayOop());

  return result;
C2V_END

C2V_VMENTRY(void, resetCompilationStatistics, (JNIEnv *jniEnv, jobject))
  JVMCICompiler* compiler = JVMCICompiler::instance(true, CHECK);
  CompilerStatistics* stats = compiler->stats();
  stats->_standard.reset();
  stats->_osr.reset();
C2V_END

C2V_VMENTRY(jobject, disassembleCodeBlob, (JNIEnv *jniEnv, jobject, jobject installedCode))
  ResourceMark rm;
  HandleMark hm;

  if (installedCode == NULL) {
    THROW_MSG_NULL(vmSymbols::java_lang_NullPointerException(), "installedCode is null");
  }

  jlong codeBlob = InstalledCode::address(installedCode);
  if (codeBlob == 0L) {
    return NULL;
  }

  CodeBlob* cb = (CodeBlob*) (address) codeBlob;
  if (cb == NULL) {
    return NULL;
  }

  // We don't want the stringStream buffer to resize during disassembly as it
  // uses scoped resource memory. If a nested function called during disassembly uses
  // a ResourceMark and the buffer expands within the scope of the mark,
  // the buffer becomes garbage when that scope is exited. Experience shows that
  // the disassembled code is typically about 10x the code size so a fixed buffer
  // sized to 20x code size plus a fixed amount for header info should be sufficient.
  int bufferSize = cb->code_size() * 20 + 1024;
  char* buffer = NEW_RESOURCE_ARRAY(char, bufferSize);
  stringStream st(buffer, bufferSize);
  if (cb->is_nmethod()) {
    nmethod* nm = (nmethod*) cb;
    if (!nm->is_alive()) {
      return NULL;
    }
  }
  Disassembler::decode(cb, &st);
  if (st.size() <= 0) {
    return NULL;
  }

  Handle result = java_lang_String::create_from_platform_dependent_str(st.as_string(), CHECK_NULL);
  return JNIHandles::make_local(THREAD, result());
C2V_END

C2V_VMENTRY(jobject, getStackTraceElement, (JNIEnv*, jobject, jobject jvmci_method, int bci))
  ResourceMark rm;
  HandleMark hm;

  methodHandle method = CompilerToVM::asMethod(jvmci_method);
  oop element = java_lang_StackTraceElement::create(method, bci, CHECK_NULL);
  return JNIHandles::make_local(THREAD, element);
C2V_END

C2V_VMENTRY(jobject, executeInstalledCode, (JNIEnv*, jobject, jobject args, jobject hotspotInstalledCode))
  ResourceMark rm;
  HandleMark hm;

  jlong nmethodValue = InstalledCode::address(hotspotInstalledCode);
  if (nmethodValue == 0L) {
    THROW_NULL(vmSymbols::jdk_vm_ci_code_InvalidInstalledCodeException());
  }
  nmethod* nm = (nmethod*) (address) nmethodValue;
  methodHandle mh = nm->method();
  Symbol* signature = mh->signature();
  JavaCallArguments jca(mh->size_of_parameters());

  JavaArgumentUnboxer jap(signature, &jca, (arrayOop) JNIHandles::resolve(args), mh->is_static());
  JavaValue result(jap.get_ret_type());
  jca.set_alternative_target(nm);
  JavaCalls::call(&result, mh, &jca, CHECK_NULL);

  if (jap.get_ret_type() == T_VOID) {
    return NULL;
  } else if (jap.get_ret_type() == T_OBJECT || jap.get_ret_type() == T_ARRAY) {
    return JNIHandles::make_local(THREAD, (oop) result.get_jobject());
  } else {
    jvalue *value = (jvalue *) result.get_value_addr();
    // Narrow the value down if required (Important on big endian machines)
    switch (jap.get_ret_type()) {
      case T_BOOLEAN:
       value->z = (jboolean) value->i;
       break;
      case T_BYTE:
       value->b = (jbyte) value->i;
       break;
      case T_CHAR:
       value->c = (jchar) value->i;
       break;
      case T_SHORT:
       value->s = (jshort) value->i;
       break;
      default:
        break;
    }
    oop o = java_lang_boxing_object::create(jap.get_ret_type(), value, CHECK_NULL);
    return JNIHandles::make_local(THREAD, o);
  }
C2V_END

C2V_VMENTRY(jlongArray, getLineNumberTable, (JNIEnv *, jobject, jobject jvmci_method))
  Method* method = CompilerToVM::asMethod(jvmci_method);
  if (!method->has_linenumber_table()) {
    return NULL;
  }
  u2 num_entries = 0;
  CompressedLineNumberReadStream streamForSize(method->compressed_linenumber_table());
  while (streamForSize.read_pair()) {
    num_entries++;
  }

  CompressedLineNumberReadStream stream(method->compressed_linenumber_table());
  typeArrayOop result = oopFactory::new_longArray(2 * num_entries, CHECK_NULL);

  int i = 0;
  jlong value;
  while (stream.read_pair()) {
    value = ((long) stream.bci());
    result->long_at_put(i, value);
    value = ((long) stream.line());
    result->long_at_put(i + 1, value);
    i += 2;
  }

  return (jlongArray) JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(jlong, getLocalVariableTableStart, (JNIEnv *, jobject, jobject jvmci_method))
  ResourceMark rm;
  Method* method = CompilerToVM::asMethod(jvmci_method);
  if (!method->has_localvariable_table()) {
    return 0;
  }
  return (jlong) (address) method->localvariable_table_start();
C2V_END

C2V_VMENTRY(jint, getLocalVariableTableLength, (JNIEnv *, jobject, jobject jvmci_method))
  ResourceMark rm;
  Method* method = CompilerToVM::asMethod(jvmci_method);
  return method->localvariable_table_length();
C2V_END

C2V_VMENTRY(void, reprofile, (JNIEnv*, jobject, jobject jvmci_method))
  Method* method = CompilerToVM::asMethod(jvmci_method);
  MethodCounters* mcs = method->method_counters();
  if (mcs != NULL) {
    mcs->clear_counters();
  }
  NOT_PRODUCT(method->set_compiled_invocation_count(0));

  CompiledMethod* code = method->code();
  if (code != NULL) {
    code->make_not_entrant();
  }

  MethodData* method_data = method->method_data();
  if (method_data == NULL) {
    ClassLoaderData* loader_data = method->method_holder()->class_loader_data();
    method_data = MethodData::allocate(loader_data, method, CHECK);
    method->set_method_data(method_data);
  } else {
    method_data->initialize();
  }
C2V_END


C2V_VMENTRY(void, invalidateInstalledCode, (JNIEnv*, jobject, jobject installed_code))
  Handle installed_code_handle(THREAD, JNIHandles::resolve(installed_code));
  nmethod::invalidate_installed_code(installed_code_handle, CHECK);
C2V_END

C2V_VMENTRY(jlongArray, collectCounters, (JNIEnv*, jobject))
  typeArrayOop arrayOop = oopFactory::new_longArray(JVMCICounterSize, CHECK_NULL);
  JavaThread::collect_counters(arrayOop);
  return (jlongArray) JNIHandles::make_local(THREAD, arrayOop);
C2V_END

C2V_VMENTRY(int, allocateCompileId, (JNIEnv*, jobject, jobject jvmci_method, int entry_bci))
  HandleMark hm;
  ResourceMark rm;
  if (JNIHandles::resolve(jvmci_method) == NULL) {
    THROW_0(vmSymbols::java_lang_NullPointerException());
  }
  Method* method = CompilerToVM::asMethod(jvmci_method);
  if (entry_bci >= method->code_size() || entry_bci < -1) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(), err_msg("Unexpected bci %d", entry_bci));
  }
  return CompileBroker::assign_compile_id_unlocked(THREAD, method, entry_bci);
C2V_END


C2V_VMENTRY(jboolean, isMature, (JNIEnv*, jobject, jlong metaspace_method_data))
  MethodData* mdo = CompilerToVM::asMethodData(metaspace_method_data);
  return mdo != NULL && mdo->is_mature();
C2V_END

C2V_VMENTRY(jboolean, hasCompiledCodeForOSR, (JNIEnv*, jobject, jobject jvmci_method, int entry_bci, int comp_level))
  Method* method = CompilerToVM::asMethod(jvmci_method);
  return method->lookup_osr_nmethod_for(entry_bci, comp_level, true) != NULL;
C2V_END

C2V_VMENTRY(jobject, getSymbol, (JNIEnv*, jobject, jlong symbol))
  Handle sym = java_lang_String::create_from_symbol((Symbol*)(address)symbol, CHECK_NULL);
  return JNIHandles::make_local(THREAD, sym());
C2V_END

bool matches(jobjectArray methods, Method* method) {
  objArrayOop methods_oop = (objArrayOop) JNIHandles::resolve(methods);

  for (int i = 0; i < methods_oop->length(); i++) {
    oop resolved = methods_oop->obj_at(i);
    if (resolved->is_a(HotSpotResolvedJavaMethodImpl::klass()) && CompilerToVM::asMethod(resolved) == method) {
      return true;
    }
  }
  return false;
}

C2V_VMENTRY(jobject, getNextStackFrame, (JNIEnv*, jobject compilerToVM, jobject hs_frame, jobjectArray methods, jint initialSkip))
  ResourceMark rm;

  if (!thread->has_last_Java_frame()) return NULL;
  Handle result = HotSpotStackFrameReference::klass()->allocate_instance_handle(CHECK_NULL);
  HotSpotStackFrameReference::klass()->initialize(CHECK_NULL);

  StackFrameStream fst(thread);
  if (hs_frame != NULL) {
    // look for the correct stack frame if one is given
    intptr_t* stack_pointer = (intptr_t*) HotSpotStackFrameReference::stackPointer(hs_frame);
    while (fst.current()->sp() != stack_pointer && !fst.is_done()) {
      fst.next();
    }
    if (fst.current()->sp() != stack_pointer) {
      THROW_MSG_NULL(vmSymbols::java_lang_IllegalStateException(), "stack frame not found")
    }
  }

  int frame_number = 0;
  vframe* vf = vframe::new_vframe(fst.current(), fst.register_map(), thread);
  if (hs_frame != NULL) {
    // look for the correct vframe within the stack frame if one is given
    int last_frame_number = HotSpotStackFrameReference::frameNumber(hs_frame);
    while (frame_number < last_frame_number) {
      if (vf->is_top()) {
        THROW_MSG_NULL(vmSymbols::java_lang_IllegalStateException(), "invalid frame number")
      }
      vf = vf->sender();
      frame_number ++;
    }
    // move one frame forward
    if (vf->is_top()) {
      if (fst.is_done()) {
        return NULL;
      }
      fst.next();
      vf = vframe::new_vframe(fst.current(), fst.register_map(), thread);
      frame_number = 0;
    } else {
      vf = vf->sender();
      frame_number++;
    }
  }

  while (true) {
    // look for the given method
    while (true) {
      StackValueCollection* locals = NULL;
      if (vf->is_compiled_frame()) {
        // compiled method frame
        compiledVFrame* cvf = compiledVFrame::cast(vf);
        if (methods == NULL || matches(methods, cvf->method())) {
          if (initialSkip > 0) {
            initialSkip --;
          } else {
            ScopeDesc* scope = cvf->scope();
            // native wrappers do not have a scope
            if (scope != NULL && scope->objects() != NULL) {
              bool realloc_failures = Deoptimization::realloc_objects(thread, fst.current(), scope->objects(), CHECK_NULL);
              Deoptimization::reassign_fields(fst.current(), fst.register_map(), scope->objects(), realloc_failures, false);

              GrowableArray<ScopeValue*>* local_values = scope->locals();
              assert(local_values != NULL, "NULL locals");
              typeArrayOop array_oop = oopFactory::new_boolArray(local_values->length(), CHECK_NULL);
              typeArrayHandle array(THREAD, array_oop);
              for (int i = 0; i < local_values->length(); i++) {
                ScopeValue* value = local_values->at(i);
                if (value->is_object()) {
                  array->bool_at_put(i, true);
                }
              }
              HotSpotStackFrameReference::set_localIsVirtual(result, array());
            } else {
              HotSpotStackFrameReference::set_localIsVirtual(result, NULL);
            }

            locals = cvf->locals();
            HotSpotStackFrameReference::set_bci(result, cvf->bci());
            oop method = CompilerToVM::get_jvmci_method(cvf->method(), CHECK_NULL);
            HotSpotStackFrameReference::set_method(result, method);
          }
        }
      } else if (vf->is_interpreted_frame()) {
        // interpreted method frame
        interpretedVFrame* ivf = interpretedVFrame::cast(vf);
        if (methods == NULL || matches(methods, ivf->method())) {
          if (initialSkip > 0) {
            initialSkip --;
          } else {
            locals = ivf->locals();
            HotSpotStackFrameReference::set_bci(result, ivf->bci());
            oop method = CompilerToVM::get_jvmci_method(ivf->method(), CHECK_NULL);
            HotSpotStackFrameReference::set_method(result, method);
            HotSpotStackFrameReference::set_localIsVirtual(result, NULL);
          }
        }
      }

      // locals != NULL means that we found a matching frame and result is already partially initialized
      if (locals != NULL) {
        HotSpotStackFrameReference::set_compilerToVM(result, JNIHandles::resolve(compilerToVM));
        HotSpotStackFrameReference::set_stackPointer(result, (jlong) fst.current()->sp());
        HotSpotStackFrameReference::set_frameNumber(result, frame_number);

        // initialize the locals array
        objArrayOop array_oop = oopFactory::new_objectArray(locals->size(), CHECK_NULL);
        objArrayHandle array(THREAD, array_oop);
        for (int i = 0; i < locals->size(); i++) {
          StackValue* var = locals->at(i);
          if (var->type() == T_OBJECT) {
            array->obj_at_put(i, locals->at(i)->get_obj()());
          }
        }
        HotSpotStackFrameReference::set_locals(result, array());

        return JNIHandles::make_local(thread, result());
      }

      if (vf->is_top()) {
        break;
      }
      frame_number++;
      vf = vf->sender();
    } // end of vframe loop

    if (fst.is_done()) {
      break;
    }
    fst.next();
    vf = vframe::new_vframe(fst.current(), fst.register_map(), thread);
    frame_number = 0;
  } // end of frame loop

  // the end was reached without finding a matching method
  return NULL;
C2V_END

C2V_VMENTRY(void, resolveInvokeDynamicInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  CallInfo callInfo;
  LinkResolver::resolve_invoke(callInfo, Handle(), cp, index, Bytecodes::_invokedynamic, CHECK);
  ConstantPoolCacheEntry* cp_cache_entry = cp->invokedynamic_cp_cache_entry_at(index);
  cp_cache_entry->set_dynamic_call(cp, callInfo);
C2V_END

C2V_VMENTRY(void, resolveInvokeHandleInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  Klass* holder = cp->klass_ref_at(index, CHECK);
  Symbol* name = cp->name_ref_at(index);
  if (MethodHandles::is_signature_polymorphic_name(holder, name)) {
    CallInfo callInfo;
    LinkResolver::resolve_invoke(callInfo, Handle(), cp, index, Bytecodes::_invokehandle, CHECK);
    ConstantPoolCacheEntry* cp_cache_entry = cp->cache()->entry_at(cp->decode_cpcache_index(index));
    cp_cache_entry->set_method_handle(cp, callInfo);
  }
C2V_END

C2V_VMENTRY(jint, isResolvedInvokeHandleInPool, (JNIEnv*, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = CompilerToVM::asConstantPool(jvmci_constant_pool);
  ConstantPoolCacheEntry* cp_cache_entry = cp->cache()->entry_at(cp->decode_cpcache_index(index));
  if (cp_cache_entry->is_resolved(Bytecodes::_invokehandle)) {
    // MethodHandle.invoke* --> LambdaForm?
    ResourceMark rm;

    LinkInfo link_info(cp, index, CATCH);

    Klass* resolved_klass = link_info.resolved_klass();

    Symbol* name_sym = cp->name_ref_at(index);

    vmassert(MethodHandles::is_method_handle_invoke_name(resolved_klass, name_sym), "!");
    vmassert(MethodHandles::is_signature_polymorphic_name(resolved_klass, name_sym), "!");

    methodHandle adapter_method(cp_cache_entry->f1_as_method());

    methodHandle resolved_method(adapter_method);

    // Can we treat it as a regular invokevirtual?
    if (resolved_method->method_holder() == resolved_klass && resolved_method->name() == name_sym) {
      vmassert(!resolved_method->is_static(),"!");
      vmassert(MethodHandles::is_signature_polymorphic_method(resolved_method()),"!");
      vmassert(!MethodHandles::is_signature_polymorphic_static(resolved_method->intrinsic_id()), "!");
      vmassert(cp_cache_entry->appendix_if_resolved(cp) == NULL, "!");
      vmassert(cp_cache_entry->method_type_if_resolved(cp) == NULL, "!");

      methodHandle m(LinkResolver::linktime_resolve_virtual_method_or_null(link_info));
      vmassert(m == resolved_method, "!!");
      return -1;
    }

    return Bytecodes::_invokevirtual;
  }
  if (cp_cache_entry->is_resolved(Bytecodes::_invokedynamic)) {
    return Bytecodes::_invokedynamic;
  }
  return -1;
C2V_END


C2V_VMENTRY(jobject, getSignaturePolymorphicHolders, (JNIEnv*, jobject))
  objArrayHandle holders = oopFactory::new_objArray_handle(SystemDictionary::String_klass(), 2, CHECK_NULL);
  Handle mh = java_lang_String::create_from_str("Ljava/lang/invoke/MethodHandle;", CHECK_NULL);
  Handle vh = java_lang_String::create_from_str("Ljava/lang/invoke/VarHandle;", CHECK_NULL);
  holders->obj_at_put(0, mh());
  holders->obj_at_put(1, vh());
  return JNIHandles::make_local(THREAD, holders());
C2V_END

C2V_VMENTRY(jboolean, shouldDebugNonSafepoints, (JNIEnv*, jobject))
  //see compute_recording_non_safepoints in debugInfroRec.cpp
  if (JvmtiExport::should_post_compiled_method_load() && FLAG_IS_DEFAULT(DebugNonSafepoints)) {
    return true;
  }
  return DebugNonSafepoints;
C2V_END

// public native void materializeVirtualObjects(HotSpotStackFrameReference stackFrame, boolean invalidate);
C2V_VMENTRY(void, materializeVirtualObjects, (JNIEnv*, jobject, jobject hs_frame, bool invalidate))
  ResourceMark rm;

  if (hs_frame == NULL) {
    THROW_MSG(vmSymbols::java_lang_NullPointerException(), "stack frame is null")
  }

  HotSpotStackFrameReference::klass()->initialize(CHECK);

  // look for the given stack frame
  StackFrameStream fst(thread);
  intptr_t* stack_pointer = (intptr_t*) HotSpotStackFrameReference::stackPointer(hs_frame);
  while (fst.current()->sp() != stack_pointer && !fst.is_done()) {
    fst.next();
  }
  if (fst.current()->sp() != stack_pointer) {
    THROW_MSG(vmSymbols::java_lang_IllegalStateException(), "stack frame not found")
  }

  if (invalidate) {
    if (!fst.current()->is_compiled_frame()) {
      THROW_MSG(vmSymbols::java_lang_IllegalStateException(), "compiled stack frame expected")
    }
    assert(fst.current()->cb()->is_nmethod(), "nmethod expected");
    ((nmethod*) fst.current()->cb())->make_not_entrant();
  }
  Deoptimization::deoptimize(thread, *fst.current(), fst.register_map(), Deoptimization::Reason_none);
  // look for the frame again as it has been updated by deopt (pc, deopt state...)
  StackFrameStream fstAfterDeopt(thread);
  while (fstAfterDeopt.current()->sp() != stack_pointer && !fstAfterDeopt.is_done()) {
    fstAfterDeopt.next();
  }
  if (fstAfterDeopt.current()->sp() != stack_pointer) {
    THROW_MSG(vmSymbols::java_lang_IllegalStateException(), "stack frame not found after deopt")
  }

  vframe* vf = vframe::new_vframe(fstAfterDeopt.current(), fstAfterDeopt.register_map(), thread);
  if (!vf->is_compiled_frame()) {
    THROW_MSG(vmSymbols::java_lang_IllegalStateException(), "compiled stack frame expected")
  }

  GrowableArray<compiledVFrame*>* virtualFrames = new GrowableArray<compiledVFrame*>(10);
  while (true) {
    assert(vf->is_compiled_frame(), "Wrong frame type");
    virtualFrames->push(compiledVFrame::cast(vf));
    if (vf->is_top()) {
      break;
    }
    vf = vf->sender();
  }

  int last_frame_number = HotSpotStackFrameReference::frameNumber(hs_frame);
  if (last_frame_number >= virtualFrames->length()) {
    THROW_MSG(vmSymbols::java_lang_IllegalStateException(), "invalid frame number")
  }

  // Reallocate the non-escaping objects and restore their fields.
  assert (virtualFrames->at(last_frame_number)->scope() != NULL,"invalid scope");
  GrowableArray<ScopeValue*>* objects = virtualFrames->at(last_frame_number)->scope()->objects();

  if (objects == NULL) {
    // no objects to materialize
    return;
  }

  bool realloc_failures = Deoptimization::realloc_objects(thread, fstAfterDeopt.current(), objects, CHECK);
  Deoptimization::reassign_fields(fstAfterDeopt.current(), fstAfterDeopt.register_map(), objects, realloc_failures, false);

  for (int frame_index = 0; frame_index < virtualFrames->length(); frame_index++) {
    compiledVFrame* cvf = virtualFrames->at(frame_index);

    GrowableArray<ScopeValue*>* scopeLocals = cvf->scope()->locals();
    StackValueCollection* locals = cvf->locals();
    if (locals != NULL) {
      for (int i2 = 0; i2 < locals->size(); i2++) {
        StackValue* var = locals->at(i2);
        if (var->type() == T_OBJECT && scopeLocals->at(i2)->is_object()) {
          jvalue val;
          val.l = (jobject) locals->at(i2)->get_obj()();
          cvf->update_local(T_OBJECT, i2, val);
        }
      }
    }

    GrowableArray<ScopeValue*>* scopeExpressions = cvf->scope()->expressions();
    StackValueCollection* expressions = cvf->expressions();
    if (expressions != NULL) {
      for (int i2 = 0; i2 < expressions->size(); i2++) {
        StackValue* var = expressions->at(i2);
        if (var->type() == T_OBJECT && scopeExpressions->at(i2)->is_object()) {
          jvalue val;
          val.l = (jobject) expressions->at(i2)->get_obj()();
          cvf->update_stack(T_OBJECT, i2, val);
        }
      }
    }

    GrowableArray<MonitorValue*>* scopeMonitors = cvf->scope()->monitors();
    GrowableArray<MonitorInfo*>* monitors = cvf->monitors();
    if (monitors != NULL) {
      for (int i2 = 0; i2 < monitors->length(); i2++) {
        cvf->update_monitor(i2, monitors->at(i2));
      }
    }
  }

  // all locals are materialized by now
  HotSpotStackFrameReference::set_localIsVirtual(hs_frame, NULL);

  // update the locals array
  objArrayHandle array(THREAD, HotSpotStackFrameReference::locals(hs_frame));
  StackValueCollection* locals = virtualFrames->at(last_frame_number)->locals();
  for (int i = 0; i < locals->size(); i++) {
    StackValue* var = locals->at(i);
    if (var->type() == T_OBJECT) {
      array->obj_at_put(i, locals->at(i)->get_obj()());
    }
  }
C2V_END

C2V_VMENTRY(void, writeDebugOutput, (JNIEnv*, jobject, jbyteArray bytes, jint offset, jint length))
  if (bytes == NULL) {
    THROW(vmSymbols::java_lang_NullPointerException());
  }
  typeArrayOop array = (typeArrayOop) JNIHandles::resolve(bytes);

  // Check if offset and length are non negative.
  if (offset < 0 || length < 0) {
    THROW(vmSymbols::java_lang_ArrayIndexOutOfBoundsException());
  }
  // Check if the range is valid.
  if ((((unsigned int) length + (unsigned int) offset) > (unsigned int) array->length())) {
    THROW(vmSymbols::java_lang_ArrayIndexOutOfBoundsException());
  }
  while (length > 0) {
    jbyte* start = array->byte_at_addr(offset);
    tty->write((char*) start, MIN2(length, (jint)O_BUFLEN));
    length -= O_BUFLEN;
    offset += O_BUFLEN;
  }
C2V_END

C2V_VMENTRY(void, flushDebugOutput, (JNIEnv*, jobject))
  tty->flush();
C2V_END

C2V_VMENTRY(int, methodDataProfileDataSize, (JNIEnv*, jobject, jlong metaspace_method_data, jint position))
  ResourceMark rm;
  MethodData* mdo = CompilerToVM::asMethodData(metaspace_method_data);
  ProfileData* profile_data = mdo->data_at(position);
  if (mdo->is_valid(profile_data)) {
    return profile_data->size_in_bytes();
  }
  DataLayout* data    = mdo->extra_data_base();
  DataLayout* end   = mdo->extra_data_limit();
  for (;; data = mdo->next_extra(data)) {
    assert(data < end, "moved past end of extra data");
    profile_data = data->data_in();
    if (mdo->dp_to_di(profile_data->dp()) == position) {
      return profile_data->size_in_bytes();
    }
  }
  THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(), err_msg("Invalid profile data position %d", position));
C2V_END

C2V_VMENTRY(jlong, getFingerprint, (JNIEnv*, jobject, jlong metaspace_klass))
  Klass *k = CompilerToVM::asKlass(metaspace_klass);
  if (k->is_instance_klass()) {
    return InstanceKlass::cast(k)->get_stored_fingerprint();
  } else {
    return 0;
  }
C2V_END

C2V_VMENTRY(jobject, getHostClass, (JNIEnv*, jobject, jobject jvmci_type))
  InstanceKlass* k = InstanceKlass::cast(CompilerToVM::asKlass(jvmci_type));
  InstanceKlass* host = k->host_klass();
  oop result = CompilerToVM::get_jvmci_type(host, CHECK_NULL);
  return JNIHandles::make_local(THREAD, result);
C2V_END

C2V_VMENTRY(int, interpreterFrameSize, (JNIEnv*, jobject, jobject bytecode_frame_handle))
  if (bytecode_frame_handle == NULL) {
    THROW_0(vmSymbols::java_lang_NullPointerException());
  }

  oop top_bytecode_frame = JNIHandles::resolve_non_null(bytecode_frame_handle);
  oop bytecode_frame = top_bytecode_frame;
  int size = 0;
  int callee_parameters = 0;
  int callee_locals = 0;
  Method* method = getMethodFromHotSpotMethod(BytecodePosition::method(bytecode_frame));
  int extra_args = method->max_stack() - BytecodeFrame::numStack(bytecode_frame);

  while (bytecode_frame != NULL) {
    int locks = BytecodeFrame::numLocks(bytecode_frame);
    int temps = BytecodeFrame::numStack(bytecode_frame);
    bool is_top_frame = (bytecode_frame == top_bytecode_frame);
    Method* method = getMethodFromHotSpotMethod(BytecodePosition::method(bytecode_frame));

    int frame_size = BytesPerWord * Interpreter::size_activation(method->max_stack(),
                                                                 temps + callee_parameters,
                                                                 extra_args,
                                                                 locks,
                                                                 callee_parameters,
                                                                 callee_locals,
                                                                 is_top_frame);
    size += frame_size;

    callee_parameters = method->size_of_parameters();
    callee_locals = method->max_locals();
    extra_args = 0;
    bytecode_frame = BytecodePosition::caller(bytecode_frame);
  }
  return size + Deoptimization::last_frame_adjust(0, callee_locals) * BytesPerWord;
C2V_END

C2V_VMENTRY(void, compileToBytecode, (JNIEnv*, jobject, jobject lambda_form_handle))
  Handle lambda_form(THREAD, JNIHandles::resolve_non_null(lambda_form_handle));
  if (lambda_form->is_a(SystemDictionary::LambdaForm_klass())) {
    TempNewSymbol compileToBytecode = SymbolTable::new_symbol("compileToBytecode", CHECK);
    JavaValue result(T_VOID);
    JavaCalls::call_special(&result, lambda_form, SystemDictionary::LambdaForm_klass(), compileToBytecode, vmSymbols::void_method_signature(), CHECK);
  } else {
    THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Unexpected type: %s", lambda_form->klass()->external_name()));
  }
C2V_END

#define CC (char*)  /*cast a literal from (const char*)*/
#define FN_PTR(f) CAST_FROM_FN_PTR(void*, &(c2v_ ## f))

#define STRING                "Ljava/lang/String;"
#define OBJECT                "Ljava/lang/Object;"
#define CLASS                 "Ljava/lang/Class;"
#define EXECUTABLE            "Ljava/lang/reflect/Executable;"
#define STACK_TRACE_ELEMENT   "Ljava/lang/StackTraceElement;"
#define INSTALLED_CODE        "Ljdk/vm/ci/code/InstalledCode;"
#define TARGET_DESCRIPTION    "Ljdk/vm/ci/code/TargetDescription;"
#define BYTECODE_FRAME        "Ljdk/vm/ci/code/BytecodeFrame;"
#define RESOLVED_METHOD       "Ljdk/vm/ci/meta/ResolvedJavaMethod;"
#define HS_RESOLVED_METHOD    "Ljdk/vm/ci/hotspot/HotSpotResolvedJavaMethodImpl;"
#define HS_RESOLVED_KLASS     "Ljdk/vm/ci/hotspot/HotSpotResolvedObjectTypeImpl;"
#define HS_CONSTANT_POOL      "Ljdk/vm/ci/hotspot/HotSpotConstantPool;"
#define HS_COMPILED_CODE      "Ljdk/vm/ci/hotspot/HotSpotCompiledCode;"
#define HS_CONFIG             "Ljdk/vm/ci/hotspot/HotSpotVMConfig;"
#define HS_METADATA           "Ljdk/vm/ci/hotspot/HotSpotMetaData;"
#define HS_STACK_FRAME_REF    "Ljdk/vm/ci/hotspot/HotSpotStackFrameReference;"
#define HS_SPECULATION_LOG    "Ljdk/vm/ci/hotspot/HotSpotSpeculationLog;"
#define METASPACE_METHOD_DATA "J"

JNINativeMethod CompilerToVM::methods[] = {
  {CC "getBytecode",                                  CC "(" HS_RESOLVED_METHOD ")[B",                                                      FN_PTR(getBytecode)},
  {CC "getExceptionTableStart",                       CC "(" HS_RESOLVED_METHOD ")J",                                                       FN_PTR(getExceptionTableStart)},
  {CC "getExceptionTableLength",                      CC "(" HS_RESOLVED_METHOD ")I",                                                       FN_PTR(getExceptionTableLength)},
  {CC "findUniqueConcreteMethod",                     CC "(" HS_RESOLVED_KLASS HS_RESOLVED_METHOD ")" HS_RESOLVED_METHOD,                   FN_PTR(findUniqueConcreteMethod)},
  {CC "getImplementor",                               CC "(" HS_RESOLVED_KLASS ")" HS_RESOLVED_KLASS,                                       FN_PTR(getImplementor)},
  {CC "getStackTraceElement",                         CC "(" HS_RESOLVED_METHOD "I)" STACK_TRACE_ELEMENT,                                   FN_PTR(getStackTraceElement)},
  {CC "methodIsIgnoredBySecurityStackWalk",           CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(methodIsIgnoredBySecurityStackWalk)},
  {CC "setNotInlinableOrCompilable",                  CC "(" HS_RESOLVED_METHOD ")V",                                                       FN_PTR(setNotInlinableOrCompilable)},
  {CC "isCompilable",                                 CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(isCompilable)},
  {CC "hasNeverInlineDirective",                      CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(hasNeverInlineDirective)},
  {CC "shouldInlineMethod",                           CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(shouldInlineMethod)},
  {CC "lookupType",                                   CC "(" STRING CLASS "Z)" HS_RESOLVED_KLASS,                                           FN_PTR(lookupType)},
  {CC "lookupNameInPool",                             CC "(" HS_CONSTANT_POOL "I)" STRING,                                                  FN_PTR(lookupNameInPool)},
  {CC "lookupNameAndTypeRefIndexInPool",              CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(lookupNameAndTypeRefIndexInPool)},
  {CC "lookupSignatureInPool",                        CC "(" HS_CONSTANT_POOL "I)" STRING,                                                  FN_PTR(lookupSignatureInPool)},
  {CC "lookupKlassRefIndexInPool",                    CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(lookupKlassRefIndexInPool)},
  {CC "lookupKlassInPool",                            CC "(" HS_CONSTANT_POOL "I)Ljava/lang/Object;",                                       FN_PTR(lookupKlassInPool)},
  {CC "lookupAppendixInPool",                         CC "(" HS_CONSTANT_POOL "I)" OBJECT,                                                  FN_PTR(lookupAppendixInPool)},
  {CC "lookupMethodInPool",                           CC "(" HS_CONSTANT_POOL "IB)" HS_RESOLVED_METHOD,                                     FN_PTR(lookupMethodInPool)},
  {CC "constantPoolRemapInstructionOperandFromCache", CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(constantPoolRemapInstructionOperandFromCache)},
  {CC "resolveConstantInPool",                        CC "(" HS_CONSTANT_POOL "I)" OBJECT,                                                  FN_PTR(resolveConstantInPool)},
  {CC "resolvePossiblyCachedConstantInPool",          CC "(" HS_CONSTANT_POOL "I)" OBJECT,                                                  FN_PTR(resolvePossiblyCachedConstantInPool)},
  {CC "resolveTypeInPool",                            CC "(" HS_CONSTANT_POOL "I)" HS_RESOLVED_KLASS,                                       FN_PTR(resolveTypeInPool)},
  {CC "resolveFieldInPool",                           CC "(" HS_CONSTANT_POOL "I" HS_RESOLVED_METHOD "B[I)" HS_RESOLVED_KLASS,              FN_PTR(resolveFieldInPool)},
  {CC "resolveInvokeDynamicInPool",                   CC "(" HS_CONSTANT_POOL "I)V",                                                        FN_PTR(resolveInvokeDynamicInPool)},
  {CC "resolveInvokeHandleInPool",                    CC "(" HS_CONSTANT_POOL "I)V",                                                        FN_PTR(resolveInvokeHandleInPool)},
  {CC "isResolvedInvokeHandleInPool",                 CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(isResolvedInvokeHandleInPool)},
  {CC "resolveMethod",                                CC "(" HS_RESOLVED_KLASS HS_RESOLVED_METHOD HS_RESOLVED_KLASS ")" HS_RESOLVED_METHOD, FN_PTR(resolveMethod)},
  {CC "getSignaturePolymorphicHolders",               CC "()[" STRING,                                                                      FN_PTR(getSignaturePolymorphicHolders)},
  {CC "getVtableIndexForInterfaceMethod",             CC "(" HS_RESOLVED_KLASS HS_RESOLVED_METHOD ")I",                                     FN_PTR(getVtableIndexForInterfaceMethod)},
  {CC "getClassInitializer",                          CC "(" HS_RESOLVED_KLASS ")" HS_RESOLVED_METHOD,                                      FN_PTR(getClassInitializer)},
  {CC "hasFinalizableSubclass",                       CC "(" HS_RESOLVED_KLASS ")Z",                                                        FN_PTR(hasFinalizableSubclass)},
  {CC "getMaxCallTargetOffset",                       CC "(J)J",                                                                            FN_PTR(getMaxCallTargetOffset)},
  {CC "asResolvedJavaMethod",                         CC "(" EXECUTABLE ")" HS_RESOLVED_METHOD,                                             FN_PTR(asResolvedJavaMethod)},
  {CC "getResolvedJavaMethod",                        CC "(Ljava/lang/Object;J)" HS_RESOLVED_METHOD,                                        FN_PTR(getResolvedJavaMethod)},
  {CC "getConstantPool",                              CC "(Ljava/lang/Object;)" HS_CONSTANT_POOL,                                           FN_PTR(getConstantPool)},
  {CC "getResolvedJavaType",                          CC "(Ljava/lang/Object;JZ)" HS_RESOLVED_KLASS,                                        FN_PTR(getResolvedJavaType)},
  {CC "readConfiguration",                            CC "()[" OBJECT,                                                                      FN_PTR(readConfiguration)},
  {CC "installCode",                                  CC "(" TARGET_DESCRIPTION HS_COMPILED_CODE INSTALLED_CODE HS_SPECULATION_LOG ")I",    FN_PTR(installCode)},
  {CC "getMetadata",                                  CC "(" TARGET_DESCRIPTION HS_COMPILED_CODE HS_METADATA ")I",                          FN_PTR(getMetadata)},
  {CC "resetCompilationStatistics",                   CC "()V",                                                                             FN_PTR(resetCompilationStatistics)},
  {CC "disassembleCodeBlob",                          CC "(" INSTALLED_CODE ")" STRING,                                                     FN_PTR(disassembleCodeBlob)},
  {CC "executeInstalledCode",                         CC "([" OBJECT INSTALLED_CODE ")" OBJECT,                                             FN_PTR(executeInstalledCode)},
  {CC "getLineNumberTable",                           CC "(" HS_RESOLVED_METHOD ")[J",                                                      FN_PTR(getLineNumberTable)},
  {CC "getLocalVariableTableStart",                   CC "(" HS_RESOLVED_METHOD ")J",                                                       FN_PTR(getLocalVariableTableStart)},
  {CC "getLocalVariableTableLength",                  CC "(" HS_RESOLVED_METHOD ")I",                                                       FN_PTR(getLocalVariableTableLength)},
  {CC "reprofile",                                    CC "(" HS_RESOLVED_METHOD ")V",                                                       FN_PTR(reprofile)},
  {CC "invalidateInstalledCode",                      CC "(" INSTALLED_CODE ")V",                                                           FN_PTR(invalidateInstalledCode)},
  {CC "collectCounters",                              CC "()[J",                                                                            FN_PTR(collectCounters)},
  {CC "allocateCompileId",                            CC "(" HS_RESOLVED_METHOD "I)I",                                                      FN_PTR(allocateCompileId)},
  {CC "isMature",                                     CC "(" METASPACE_METHOD_DATA ")Z",                                                    FN_PTR(isMature)},
  {CC "hasCompiledCodeForOSR",                        CC "(" HS_RESOLVED_METHOD "II)Z",                                                     FN_PTR(hasCompiledCodeForOSR)},
  {CC "getSymbol",                                    CC "(J)" STRING,                                                                      FN_PTR(getSymbol)},
  {CC "getNextStackFrame",                            CC "(" HS_STACK_FRAME_REF "[" RESOLVED_METHOD "I)" HS_STACK_FRAME_REF,                FN_PTR(getNextStackFrame)},
  {CC "materializeVirtualObjects",                    CC "(" HS_STACK_FRAME_REF "Z)V",                                                      FN_PTR(materializeVirtualObjects)},
  {CC "shouldDebugNonSafepoints",                     CC "()Z",                                                                             FN_PTR(shouldDebugNonSafepoints)},
  {CC "writeDebugOutput",                             CC "([BII)V",                                                                         FN_PTR(writeDebugOutput)},
  {CC "flushDebugOutput",                             CC "()V",                                                                             FN_PTR(flushDebugOutput)},
  {CC "methodDataProfileDataSize",                    CC "(JI)I",                                                                           FN_PTR(methodDataProfileDataSize)},
  {CC "getFingerprint",                               CC "(J)J",                                                                            FN_PTR(getFingerprint)},
  {CC "getHostClass",                                 CC "(" HS_RESOLVED_KLASS ")" HS_RESOLVED_KLASS,                                       FN_PTR(getHostClass)},
  {CC "interpreterFrameSize",                         CC "(" BYTECODE_FRAME ")I",                                                           FN_PTR(interpreterFrameSize)},
  {CC "compileToBytecode",                            CC "(" OBJECT ")V",                                                                   FN_PTR(compileToBytecode)},
  {CC "getFlagValue",                                 CC "(" STRING ")" OBJECT,                                                             FN_PTR(getFlagValue)},
};

int CompilerToVM::methods_count() {
  return sizeof(methods) / sizeof(JNINativeMethod);
}
