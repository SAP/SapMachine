 /*
 * Copyright (c) 2012, 2017, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

// A ClassLoaderData identifies the full set of class types that a class
// loader's name resolution strategy produces for a given configuration of the
// class loader.
// Class types in the ClassLoaderData may be defined by from class file binaries
// provided by the class loader, or from other class loader it interacts with
// according to its name resolution strategy.
//
// Class loaders that implement a deterministic name resolution strategy
// (including with respect to their delegation behavior), such as the boot, the
// platform, and the system loaders of the JDK's built-in class loader
// hierarchy, always produce the same linkset for a given configuration.
//
// ClassLoaderData carries information related to a linkset (e.g.,
// metaspace holding its klass definitions).
// The System Dictionary and related data structures (e.g., placeholder table,
// loader constraints table) as well as the runtime representation of classes
// only reference ClassLoaderData.
//
// Instances of java.lang.ClassLoader holds a pointer to a ClassLoaderData that
// that represent the loader's "linking domain" in the JVM.
//
// The bootstrap loader (represented by NULL) also has a ClassLoaderData,
// the singleton class the_null_class_loader_data().

#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/metadataOnStackMark.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/packageEntry.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/gcLocker.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/mutex.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/synchronizer.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"
#if INCLUDE_ALL_GCS
#include "gc/g1/g1SATBCardTableModRefBS.hpp"
#endif // INCLUDE_ALL_GCS
#if INCLUDE_TRACE
#include "trace/tracing.hpp"
#endif

ClassLoaderData * ClassLoaderData::_the_null_class_loader_data = NULL;

ClassLoaderData::ClassLoaderData(Handle h_class_loader, bool is_anonymous, Dependencies dependencies) :
  _class_loader(h_class_loader()),
  _is_anonymous(is_anonymous),
  // An anonymous class loader data doesn't have anything to keep
  // it from being unloaded during parsing of the anonymous class.
  // The null-class-loader should always be kept alive.
  _keep_alive((is_anonymous || h_class_loader.is_null()) ? 1 : 0),
  _metaspace(NULL), _unloading(false), _klasses(NULL),
  _modules(NULL), _packages(NULL),
  _claimed(0), _modified_oops(true), _accumulated_modified_oops(false),
  _jmethod_ids(NULL), _handles(), _deallocate_list(NULL),
  _next(NULL), _dependencies(dependencies),
  _metaspace_lock(new Mutex(Monitor::leaf+1, "Metaspace allocation lock", true,
                            Monitor::_safepoint_check_never)) {

  // A ClassLoaderData created solely for an anonymous class should never have a
  // ModuleEntryTable or PackageEntryTable created for it. The defining package
  // and module for an anonymous class will be found in its host class.
  if (!is_anonymous) {
    _packages = new PackageEntryTable(PackageEntryTable::_packagetable_entry_size);
    if (h_class_loader.is_null()) {
      // Create unnamed module for boot loader
      _unnamed_module = ModuleEntry::create_boot_unnamed_module(this);
    } else {
      // Create unnamed module for all other loaders
      _unnamed_module = ModuleEntry::create_unnamed_module(this);
    }
  } else {
    _unnamed_module = NULL;
  }

  if (!is_anonymous) {
    _dictionary = create_dictionary();
  } else {
    _dictionary = NULL;
  }
  TRACE_INIT_ID(this);
}

void ClassLoaderData::init_dependencies(TRAPS) {
  assert(!Universe::is_fully_initialized(), "should only be called when initializing");
  assert(is_the_null_class_loader_data(), "should only call this for the null class loader");
  _dependencies.init(CHECK);
}

void ClassLoaderData::Dependencies::init(TRAPS) {
  // Create empty dependencies array to add to. CMS requires this to be
  // an oop so that it can track additions via card marks.  We think.
  _list_head = oopFactory::new_objectArray(2, CHECK);
}

ClassLoaderData::ChunkedHandleList::~ChunkedHandleList() {
  Chunk* c = _head;
  while (c != NULL) {
    Chunk* next = c->_next;
    delete c;
    c = next;
  }
}

oop* ClassLoaderData::ChunkedHandleList::add(oop o) {
  if (_head == NULL || _head->_size == Chunk::CAPACITY) {
    Chunk* next = new Chunk(_head);
    OrderAccess::release_store(&_head, next);
  }
  oop* handle = &_head->_data[_head->_size];
  *handle = o;
  OrderAccess::release_store(&_head->_size, _head->_size + 1);
  return handle;
}

inline void ClassLoaderData::ChunkedHandleList::oops_do_chunk(OopClosure* f, Chunk* c, const juint size) {
  for (juint i = 0; i < size; i++) {
    if (c->_data[i] != NULL) {
      f->do_oop(&c->_data[i]);
    }
  }
}

void ClassLoaderData::ChunkedHandleList::oops_do(OopClosure* f) {
  Chunk* head = OrderAccess::load_acquire(&_head);
  if (head != NULL) {
    // Must be careful when reading size of head
    oops_do_chunk(f, head, OrderAccess::load_acquire(&head->_size));
    for (Chunk* c = head->_next; c != NULL; c = c->_next) {
      oops_do_chunk(f, c, c->_size);
    }
  }
}

#ifdef ASSERT
class VerifyContainsOopClosure : public OopClosure {
  oop* _target;
  bool _found;

 public:
  VerifyContainsOopClosure(oop* target) : _target(target), _found(false) {}

  void do_oop(oop* p) {
    if (p == _target) {
      _found = true;
    }
  }

  void do_oop(narrowOop* p) {
    // The ChunkedHandleList should not contain any narrowOop
    ShouldNotReachHere();
  }

  bool found() const {
    return _found;
  }
};

bool ClassLoaderData::ChunkedHandleList::contains(oop* p) {
  VerifyContainsOopClosure cl(p);
  oops_do(&cl);
  return cl.found();
}
#endif // ASSERT

bool ClassLoaderData::claim() {
  if (_claimed == 1) {
    return false;
  }

  return (int) Atomic::cmpxchg(1, &_claimed, 0) == 0;
}

// Anonymous classes have their own ClassLoaderData that is marked to keep alive
// while the class is being parsed, and if the class appears on the module fixup list.
// Due to the uniqueness that no other class shares the anonymous class' name or
// ClassLoaderData, no other non-GC thread has knowledge of the anonymous class while
// it is being defined, therefore _keep_alive is not volatile or atomic.
void ClassLoaderData::inc_keep_alive() {
  if (is_anonymous()) {
    assert(_keep_alive >= 0, "Invalid keep alive increment count");
    _keep_alive++;
  }
}

void ClassLoaderData::dec_keep_alive() {
  if (is_anonymous()) {
    assert(_keep_alive > 0, "Invalid keep alive decrement count");
    _keep_alive--;
  }
}

void ClassLoaderData::oops_do(OopClosure* f, bool must_claim, bool clear_mod_oops) {
  if (must_claim && !claim()) {
    return;
  }

  // Only clear modified_oops after the ClassLoaderData is claimed.
  if (clear_mod_oops) {
    clear_modified_oops();
  }

  f->do_oop(&_class_loader);
  _dependencies.oops_do(f);
  _handles.oops_do(f);
}

void ClassLoaderData::Dependencies::oops_do(OopClosure* f) {
  f->do_oop((oop*)&_list_head);
}

void ClassLoaderData::classes_do(KlassClosure* klass_closure) {
  // Lock-free access requires load_acquire
  for (Klass* k = OrderAccess::load_acquire(&_klasses); k != NULL; k = k->next_link()) {
    klass_closure->do_klass(k);
    assert(k != k->next_link(), "no loops!");
  }
}

void ClassLoaderData::classes_do(void f(Klass * const)) {
  // Lock-free access requires load_acquire
  for (Klass* k = OrderAccess::load_acquire(&_klasses); k != NULL; k = k->next_link()) {
    f(k);
    assert(k != k->next_link(), "no loops!");
  }
}

void ClassLoaderData::methods_do(void f(Method*)) {
  // Lock-free access requires load_acquire
  for (Klass* k = OrderAccess::load_acquire(&_klasses); k != NULL; k = k->next_link()) {
    if (k->is_instance_klass() && InstanceKlass::cast(k)->is_loaded()) {
      InstanceKlass::cast(k)->methods_do(f);
    }
  }
}

void ClassLoaderData::loaded_classes_do(KlassClosure* klass_closure) {
  // Lock-free access requires load_acquire
  for (Klass* k = OrderAccess::load_acquire(&_klasses); k != NULL; k = k->next_link()) {
    // Do not filter ArrayKlass oops here...
    if (k->is_array_klass() || (k->is_instance_klass() && InstanceKlass::cast(k)->is_loaded())) {
      klass_closure->do_klass(k);
    }
  }
}

void ClassLoaderData::classes_do(void f(InstanceKlass*)) {
  // Lock-free access requires load_acquire
  for (Klass* k = OrderAccess::load_acquire(&_klasses); k != NULL; k = k->next_link()) {
    if (k->is_instance_klass()) {
      f(InstanceKlass::cast(k));
    }
    assert(k != k->next_link(), "no loops!");
  }
}

void ClassLoaderData::modules_do(void f(ModuleEntry*)) {
  assert_locked_or_safepoint(Module_lock);
  if (_unnamed_module != NULL) {
    f(_unnamed_module);
  }
  if (_modules != NULL) {
    for (int i = 0; i < _modules->table_size(); i++) {
      for (ModuleEntry* entry = _modules->bucket(i);
           entry != NULL;
           entry = entry->next()) {
        f(entry);
      }
    }
  }
}

void ClassLoaderData::packages_do(void f(PackageEntry*)) {
  assert_locked_or_safepoint(Module_lock);
  if (_packages != NULL) {
    for (int i = 0; i < _packages->table_size(); i++) {
      for (PackageEntry* entry = _packages->bucket(i);
           entry != NULL;
           entry = entry->next()) {
        f(entry);
      }
    }
  }
}

void ClassLoaderData::record_dependency(const Klass* k, TRAPS) {
  assert(k != NULL, "invariant");

  ClassLoaderData * const from_cld = this;
  ClassLoaderData * const to_cld = k->class_loader_data();

  // Dependency to the null class loader data doesn't need to be recorded
  // because the null class loader data never goes away.
  if (to_cld->is_the_null_class_loader_data()) {
    return;
  }

  oop to;
  if (to_cld->is_anonymous()) {
    // Anonymous class dependencies are through the mirror.
    to = k->java_mirror();
  } else {
    to = to_cld->class_loader();

    // If from_cld is anonymous, even if it's class_loader is a parent of 'to'
    // we still have to add it.  The class_loader won't keep from_cld alive.
    if (!from_cld->is_anonymous()) {
      // Check that this dependency isn't from the same or parent class_loader
      oop from = from_cld->class_loader();

      oop curr = from;
      while (curr != NULL) {
        if (curr == to) {
          return; // this class loader is in the parent list, no need to add it.
        }
        curr = java_lang_ClassLoader::parent(curr);
      }
    }
  }

  // It's a dependency we won't find through GC, add it. This is relatively rare
  // Must handle over GC point.
  Handle dependency(THREAD, to);
  from_cld->_dependencies.add(dependency, CHECK);

  // Added a potentially young gen oop to the ClassLoaderData
  record_modified_oops();
}


void ClassLoaderData::Dependencies::add(Handle dependency, TRAPS) {
  // Check first if this dependency is already in the list.
  // Save a pointer to the last to add to under the lock.
  objArrayOop ok = _list_head;
  objArrayOop last = NULL;
  while (ok != NULL) {
    last = ok;
    if (ok->obj_at(0) == dependency()) {
      // Don't need to add it
      return;
    }
    ok = (objArrayOop)ok->obj_at(1);
  }

  // Must handle over GC points
  assert (last != NULL, "dependencies should be initialized");
  objArrayHandle last_handle(THREAD, last);

  // Create a new dependency node with fields for (class_loader or mirror, next)
  objArrayOop deps = oopFactory::new_objectArray(2, CHECK);
  deps->obj_at_put(0, dependency());

  // Must handle over GC points
  objArrayHandle new_dependency(THREAD, deps);

  // Add the dependency under lock
  locked_add(last_handle, new_dependency, THREAD);
}

void ClassLoaderData::Dependencies::locked_add(objArrayHandle last_handle,
                                               objArrayHandle new_dependency,
                                               Thread* THREAD) {

  // Have to lock and put the new dependency on the end of the dependency
  // array so the card mark for CMS sees that this dependency is new.
  // Can probably do this lock free with some effort.
  ObjectLocker ol(Handle(THREAD, _list_head), THREAD);

  oop loader_or_mirror = new_dependency->obj_at(0);

  // Since the dependencies are only added, add to the end.
  objArrayOop end = last_handle();
  objArrayOop last = NULL;
  while (end != NULL) {
    last = end;
    // check again if another thread added it to the end.
    if (end->obj_at(0) == loader_or_mirror) {
      // Don't need to add it
      return;
    }
    end = (objArrayOop)end->obj_at(1);
  }
  assert (last != NULL, "dependencies should be initialized");
  // fill in the first element with the oop in new_dependency.
  if (last->obj_at(0) == NULL) {
    last->obj_at_put(0, new_dependency->obj_at(0));
  } else {
    last->obj_at_put(1, new_dependency());
  }
}

void ClassLoaderDataGraph::clear_claimed_marks() {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->clear_claimed();
  }
}

void ClassLoaderData::add_class(Klass* k, bool publicize /* true */) {
  {
    MutexLockerEx ml(metaspace_lock(), Mutex::_no_safepoint_check_flag);
    Klass* old_value = _klasses;
    k->set_next_link(old_value);
    // Link the new item into the list, making sure the linked class is stable
    // since the list can be walked without a lock
    OrderAccess::release_store(&_klasses, k);
  }

  if (publicize && k->class_loader_data() != NULL) {
    ResourceMark rm;
    log_trace(class, loader, data)("Adding k: " PTR_FORMAT " %s to CLD: "
                  PTR_FORMAT " loader: " PTR_FORMAT " %s",
                  p2i(k),
                  k->external_name(),
                  p2i(k->class_loader_data()),
                  p2i((void *)k->class_loader()),
                  loader_name());
  }
}

// Class iterator used by the compiler.  It gets some number of classes at
// a safepoint to decay invocation counters on the methods.
class ClassLoaderDataGraphKlassIteratorStatic {
  ClassLoaderData* _current_loader_data;
  Klass*           _current_class_entry;
 public:

  ClassLoaderDataGraphKlassIteratorStatic() : _current_loader_data(NULL), _current_class_entry(NULL) {}

  InstanceKlass* try_get_next_class() {
    assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");
    int max_classes = InstanceKlass::number_of_instance_classes();
    assert(max_classes > 0, "should not be called with no instance classes");
    for (int i = 0; i < max_classes; ) {

      if (_current_class_entry != NULL) {
        Klass* k = _current_class_entry;
        _current_class_entry = _current_class_entry->next_link();

        if (k->is_instance_klass()) {
          InstanceKlass* ik = InstanceKlass::cast(k);
          i++;  // count all instance classes found
          // Not yet loaded classes are counted in max_classes
          // but only return loaded classes.
          if (ik->is_loaded()) {
            return ik;
          }
        }
      } else {
        // Go to next CLD
        if (_current_loader_data != NULL) {
          _current_loader_data = _current_loader_data->next();
        }
        // Start at the beginning
        if (_current_loader_data == NULL) {
          _current_loader_data = ClassLoaderDataGraph::_head;
        }

        _current_class_entry = _current_loader_data->klasses();
      }
    }
    // Should never be reached unless all instance classes have failed or are not fully loaded.
    // Caller handles NULL.
    return NULL;
  }

  // If the current class for the static iterator is a class being unloaded or
  // deallocated, adjust the current class.
  void adjust_saved_class(ClassLoaderData* cld) {
    if (_current_loader_data == cld) {
      _current_loader_data = cld->next();
      if (_current_loader_data != NULL) {
        _current_class_entry = _current_loader_data->klasses();
      }  // else try_get_next_class will start at the head
    }
  }

  void adjust_saved_class(Klass* klass) {
    if (_current_class_entry == klass) {
      _current_class_entry = klass->next_link();
    }
  }
};

static ClassLoaderDataGraphKlassIteratorStatic static_klass_iterator;

InstanceKlass* ClassLoaderDataGraph::try_get_next_class() {
  return static_klass_iterator.try_get_next_class();
}


// Remove a klass from the _klasses list for scratch_class during redefinition
// or parsed class in the case of an error.
void ClassLoaderData::remove_class(Klass* scratch_class) {
  assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");

  // Adjust global class iterator.
  static_klass_iterator.adjust_saved_class(scratch_class);

  Klass* prev = NULL;
  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    if (k == scratch_class) {
      if (prev == NULL) {
        _klasses = k->next_link();
      } else {
        Klass* next = k->next_link();
        prev->set_next_link(next);
      }
      return;
    }
    prev = k;
    assert(k != k->next_link(), "no loops!");
  }
  ShouldNotReachHere();   // should have found this class!!
}

void ClassLoaderData::unload() {
  _unloading = true;

  // Tell serviceability tools these classes are unloading
  classes_do(InstanceKlass::notify_unload_class);

  LogTarget(Debug, class, loader, data) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);
    ls.print(": unload loader data " INTPTR_FORMAT, p2i(this));
    ls.print(" for instance " INTPTR_FORMAT " of %s", p2i((void *)class_loader()),
               loader_name());
    if (is_anonymous()) {
      ls.print(" for anonymous class  " INTPTR_FORMAT " ", p2i(_klasses));
    }
    ls.cr();
  }

  // Some items on the _deallocate_list need to free their C heap structures
  // if they are not already on the _klasses list.
  unload_deallocate_list();

  // Clean up global class iterator for compiler
  static_klass_iterator.adjust_saved_class(this);
}

ModuleEntryTable* ClassLoaderData::modules() {
  // Lazily create the module entry table at first request.
  // Lock-free access requires load_acquire.
  ModuleEntryTable* modules = OrderAccess::load_acquire(&_modules);
  if (modules == NULL) {
    MutexLocker m1(Module_lock);
    // Check if _modules got allocated while we were waiting for this lock.
    if ((modules = _modules) == NULL) {
      modules = new ModuleEntryTable(ModuleEntryTable::_moduletable_entry_size);

      {
        MutexLockerEx m1(metaspace_lock(), Mutex::_no_safepoint_check_flag);
        // Ensure _modules is stable, since it is examined without a lock
        OrderAccess::release_store(&_modules, modules);
      }
    }
  }
  return modules;
}

const int _boot_loader_dictionary_size    = 1009;
const int _default_loader_dictionary_size = 107;

Dictionary* ClassLoaderData::create_dictionary() {
  assert(!is_anonymous(), "anonymous class loader data do not have a dictionary");
  int size;
  bool resizable = false;
  if (_the_null_class_loader_data == NULL) {
    size = _boot_loader_dictionary_size;
    resizable = true;
  } else if (class_loader()->is_a(SystemDictionary::reflect_DelegatingClassLoader_klass())) {
    size = 1;  // there's only one class in relection class loader and no initiated classes
  } else if (is_system_class_loader_data()) {
    size = _boot_loader_dictionary_size;
    resizable = true;
  } else {
    size = _default_loader_dictionary_size;
    resizable = true;
  }
  if (!DynamicallyResizeSystemDictionaries || DumpSharedSpaces || UseSharedSpaces) {
    resizable = false;
  }
  return new Dictionary(this, size, resizable);
}

// Unloading support
oop ClassLoaderData::keep_alive_object() const {
  assert_locked_or_safepoint(_metaspace_lock);
  assert(!keep_alive(), "Don't use with CLDs that are artificially kept alive");
  return is_anonymous() ? _klasses->java_mirror() : class_loader();
}

bool ClassLoaderData::is_alive(BoolObjectClosure* is_alive_closure) const {
  bool alive = keep_alive() // null class loader and incomplete anonymous klasses.
      || is_alive_closure->do_object_b(keep_alive_object());

  return alive;
}

ClassLoaderData::~ClassLoaderData() {
  // Release C heap structures for all the classes.
  classes_do(InstanceKlass::release_C_heap_structures);

  // Release C heap allocated hashtable for all the packages.
  if (_packages != NULL) {
    // Destroy the table itself
    delete _packages;
    _packages = NULL;
  }

  // Release C heap allocated hashtable for all the modules.
  if (_modules != NULL) {
    // Destroy the table itself
    delete _modules;
    _modules = NULL;
  }

  // Release C heap allocated hashtable for the dictionary
  if (_dictionary != NULL) {
    // Destroy the table itself
    delete _dictionary;
    _dictionary = NULL;
  }

  if (_unnamed_module != NULL) {
    _unnamed_module->delete_unnamed_module();
    _unnamed_module = NULL;
  }

  // release the metaspace
  Metaspace *m = _metaspace;
  if (m != NULL) {
    _metaspace = NULL;
    delete m;
  }
  // Clear all the JNI handles for methods
  // These aren't deallocated and are going to look like a leak, but that's
  // needed because we can't really get rid of jmethodIDs because we don't
  // know when native code is going to stop using them.  The spec says that
  // they're "invalid" but existing programs likely rely on their being
  // NULL after class unloading.
  if (_jmethod_ids != NULL) {
    Method::clear_jmethod_ids(this);
  }
  // Delete lock
  delete _metaspace_lock;

  // Delete free list
  if (_deallocate_list != NULL) {
    delete _deallocate_list;
  }
}

// Returns true if this class loader data is for the system class loader.
bool ClassLoaderData::is_system_class_loader_data() const {
  return SystemDictionary::is_system_class_loader(class_loader());
}

// Returns true if this class loader data is for the platform class loader.
bool ClassLoaderData::is_platform_class_loader_data() const {
  return SystemDictionary::is_platform_class_loader(class_loader());
}

// Returns true if this class loader data is one of the 3 builtin
// (boot, application/system or platform) class loaders. Note, the
// builtin loaders are not freed by a GC.
bool ClassLoaderData::is_builtin_class_loader_data() const {
  return (is_the_null_class_loader_data() ||
          SystemDictionary::is_system_class_loader(class_loader()) ||
          SystemDictionary::is_platform_class_loader(class_loader()));
}

Metaspace* ClassLoaderData::metaspace_non_null() {
  // If the metaspace has not been allocated, create a new one.  Might want
  // to create smaller arena for Reflection class loaders also.
  // The reason for the delayed allocation is because some class loaders are
  // simply for delegating with no metadata of their own.
  // Lock-free access requires load_acquire.
  Metaspace* metaspace = OrderAccess::load_acquire(&_metaspace);
  if (metaspace == NULL) {
    MutexLockerEx ml(_metaspace_lock,  Mutex::_no_safepoint_check_flag);
    // Check if _metaspace got allocated while we were waiting for this lock.
    if ((metaspace = _metaspace) == NULL) {
      if (this == the_null_class_loader_data()) {
        assert (class_loader() == NULL, "Must be");
        metaspace = new Metaspace(_metaspace_lock, Metaspace::BootMetaspaceType);
      } else if (is_anonymous()) {
        if (class_loader() != NULL) {
          log_trace(class, loader, data)("is_anonymous: %s", class_loader()->klass()->internal_name());
        }
        metaspace = new Metaspace(_metaspace_lock, Metaspace::AnonymousMetaspaceType);
      } else if (class_loader()->is_a(SystemDictionary::reflect_DelegatingClassLoader_klass())) {
        if (class_loader() != NULL) {
          log_trace(class, loader, data)("is_reflection: %s", class_loader()->klass()->internal_name());
        }
        metaspace = new Metaspace(_metaspace_lock, Metaspace::ReflectionMetaspaceType);
      } else {
        metaspace = new Metaspace(_metaspace_lock, Metaspace::StandardMetaspaceType);
      }
      // Ensure _metaspace is stable, since it is examined without a lock
      OrderAccess::release_store(&_metaspace, metaspace);
    }
  }
  return metaspace;
}

OopHandle ClassLoaderData::add_handle(Handle h) {
  MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
  record_modified_oops();
  return OopHandle(_handles.add(h()));
}

void ClassLoaderData::remove_handle(OopHandle h) {
  assert(!is_unloading(), "Do not remove a handle for a CLD that is unloading");
  oop* ptr = h.ptr_raw();
  if (ptr != NULL) {
    assert(_handles.contains(ptr), "Got unexpected handle " PTR_FORMAT, p2i(ptr));
#if INCLUDE_ALL_GCS
    // This barrier is used by G1 to remember the old oop values, so
    // that we don't forget any objects that were live at the snapshot at
    // the beginning.
    if (UseG1GC) {
      oop obj = *ptr;
      if (obj != NULL) {
        G1SATBCardTableModRefBS::enqueue(obj);
      }
    }
#endif
    *ptr = NULL;
  }
}

void ClassLoaderData::init_handle_locked(OopHandle& dest, Handle h) {
  MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
  if (dest.resolve() != NULL) {
    return;
  } else {
    dest = _handles.add(h());
  }
}

// Add this metadata pointer to be freed when it's safe.  This is only during
// class unloading because Handles might point to this metadata field.
void ClassLoaderData::add_to_deallocate_list(Metadata* m) {
  // Metadata in shared region isn't deleted.
  if (!m->is_shared()) {
    MutexLockerEx ml(metaspace_lock(),  Mutex::_no_safepoint_check_flag);
    if (_deallocate_list == NULL) {
      _deallocate_list = new (ResourceObj::C_HEAP, mtClass) GrowableArray<Metadata*>(100, true);
    }
    _deallocate_list->append_if_missing(m);
  }
}

// Deallocate free metadata on the free list.  How useful the PermGen was!
void ClassLoaderData::free_deallocate_list() {
  // Don't need lock, at safepoint
  assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");
  assert(!is_unloading(), "only called for ClassLoaderData that are not unloading");
  if (_deallocate_list == NULL) {
    return;
  }
  // Go backwards because this removes entries that are freed.
  for (int i = _deallocate_list->length() - 1; i >= 0; i--) {
    Metadata* m = _deallocate_list->at(i);
    if (!m->on_stack()) {
      _deallocate_list->remove_at(i);
      // There are only three types of metadata that we deallocate directly.
      // Cast them so they can be used by the template function.
      if (m->is_method()) {
        MetadataFactory::free_metadata(this, (Method*)m);
      } else if (m->is_constantPool()) {
        MetadataFactory::free_metadata(this, (ConstantPool*)m);
      } else if (m->is_klass()) {
        MetadataFactory::free_metadata(this, (InstanceKlass*)m);
      } else {
        ShouldNotReachHere();
      }
    } else {
      // Metadata is alive.
      // If scratch_class is on stack then it shouldn't be on this list!
      assert(!m->is_klass() || !((InstanceKlass*)m)->is_scratch_class(),
             "scratch classes on this list should be dead");
      // Also should assert that other metadata on the list was found in handles.
    }
  }
}

// This is distinct from free_deallocate_list.  For class loader data that are
// unloading, this frees the C heap memory for items on the list, and unlinks
// scratch or error classes so that unloading events aren't triggered for these
// classes. The metadata is removed with the unloading metaspace.
// There isn't C heap memory allocated for methods, so nothing is done for them.
void ClassLoaderData::unload_deallocate_list() {
  // Don't need lock, at safepoint
  assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");
  assert(is_unloading(), "only called for ClassLoaderData that are unloading");
  if (_deallocate_list == NULL) {
    return;
  }
  // Go backwards because this removes entries that are freed.
  for (int i = _deallocate_list->length() - 1; i >= 0; i--) {
    Metadata* m = _deallocate_list->at(i);
    assert (!m->on_stack(), "wouldn't be unloading if this were so");
    _deallocate_list->remove_at(i);
    if (m->is_constantPool()) {
      ((ConstantPool*)m)->release_C_heap_structures();
    } else if (m->is_klass()) {
      InstanceKlass* ik = (InstanceKlass*)m;
      // also releases ik->constants() C heap memory
      InstanceKlass::release_C_heap_structures(ik);
      // Remove the class so unloading events aren't triggered for
      // this class (scratch or error class) in do_unloading().
      remove_class(ik);
    }
  }
}

// These anonymous class loaders are to contain classes used for JSR292
ClassLoaderData* ClassLoaderData::anonymous_class_loader_data(oop loader, TRAPS) {
  // Add a new class loader data to the graph.
  Handle lh(THREAD, loader);
  return ClassLoaderDataGraph::add(lh, true, THREAD);
}

const char* ClassLoaderData::loader_name() {
  // Handles null class loader
  return SystemDictionary::loader_name(class_loader());
}

#ifndef PRODUCT
// Define to dump klasses
#undef CLD_DUMP_KLASSES

void ClassLoaderData::dump(outputStream * const out) {
  out->print("ClassLoaderData CLD: " PTR_FORMAT ", loader: " PTR_FORMAT ", loader_klass: " PTR_FORMAT " %s {",
      p2i(this), p2i((void *)class_loader()),
      p2i(class_loader() != NULL ? class_loader()->klass() : NULL), loader_name());
  if (claimed()) out->print(" claimed ");
  if (is_unloading()) out->print(" unloading ");
  out->cr();
  if (metaspace_or_null() != NULL) {
    out->print_cr("metaspace: " INTPTR_FORMAT, p2i(metaspace_or_null()));
    metaspace_or_null()->dump(out);
  } else {
    out->print_cr("metaspace: NULL");
  }

#ifdef CLD_DUMP_KLASSES
  if (Verbose) {
    Klass* k = _klasses;
    while (k != NULL) {
      out->print_cr("klass " PTR_FORMAT ", %s", p2i(k), k->name()->as_C_string());
      assert(k != k->next_link(), "no loops!");
      k = k->next_link();
    }
  }
#endif  // CLD_DUMP_KLASSES
#undef CLD_DUMP_KLASSES
  if (_jmethod_ids != NULL) {
    Method::print_jmethod_ids(this, out);
  }
  out->print_cr("}");
}
#endif // PRODUCT

void ClassLoaderData::verify() {
  assert_locked_or_safepoint(_metaspace_lock);
  oop cl = class_loader();

  guarantee(this == class_loader_data(cl) || is_anonymous(), "Must be the same");
  guarantee(cl != NULL || this == ClassLoaderData::the_null_class_loader_data() || is_anonymous(), "must be");

  // Verify the integrity of the allocated space.
  if (metaspace_or_null() != NULL) {
    metaspace_or_null()->verify();
  }

  for (Klass* k = _klasses; k != NULL; k = k->next_link()) {
    guarantee(k->class_loader_data() == this, "Must be the same");
    k->verify();
    assert(k != k->next_link(), "no loops!");
  }
}

bool ClassLoaderData::contains_klass(Klass* klass) {
  // Lock-free access requires load_acquire
  for (Klass* k = OrderAccess::load_acquire(&_klasses); k != NULL; k = k->next_link()) {
    if (k == klass) return true;
  }
  return false;
}


// GC root of class loader data created.
ClassLoaderData* ClassLoaderDataGraph::_head = NULL;
ClassLoaderData* ClassLoaderDataGraph::_unloading = NULL;
ClassLoaderData* ClassLoaderDataGraph::_saved_unloading = NULL;
ClassLoaderData* ClassLoaderDataGraph::_saved_head = NULL;

bool ClassLoaderDataGraph::_should_purge = false;
bool ClassLoaderDataGraph::_metaspace_oom = false;

// Add a new class loader data node to the list.  Assign the newly created
// ClassLoaderData into the java/lang/ClassLoader object as a hidden field
ClassLoaderData* ClassLoaderDataGraph::add(Handle loader, bool is_anonymous, TRAPS) {
  // We need to allocate all the oops for the ClassLoaderData before allocating the
  // actual ClassLoaderData object.
  ClassLoaderData::Dependencies dependencies(CHECK_NULL);

  NoSafepointVerifier no_safepoints; // we mustn't GC until we've installed the
                                     // ClassLoaderData in the graph since the CLD
                                     // contains unhandled oops

  ClassLoaderData* cld = new ClassLoaderData(loader, is_anonymous, dependencies);


  if (!is_anonymous) {
    ClassLoaderData** cld_addr = java_lang_ClassLoader::loader_data_addr(loader());
    // First, Atomically set it
    ClassLoaderData* old = Atomic::cmpxchg(cld, cld_addr, (ClassLoaderData*)NULL);
    if (old != NULL) {
      delete cld;
      // Returns the data.
      return old;
    }
  }

  // We won the race, and therefore the task of adding the data to the list of
  // class loader data
  ClassLoaderData** list_head = &_head;
  ClassLoaderData* next = _head;

  do {
    cld->set_next(next);
    ClassLoaderData* exchanged = Atomic::cmpxchg(cld, list_head, next);
    if (exchanged == next) {
      LogTarget(Debug, class, loader, data) lt;
      if (lt.is_enabled()) {
       PauseNoSafepointVerifier pnsv(&no_safepoints); // Need safe points for JavaCalls::call_virtual
       LogStream ls(lt);
       print_creation(&ls, loader, cld, CHECK_NULL);
      }
      return cld;
    }
    next = exchanged;
  } while (true);
}

void ClassLoaderDataGraph::print_creation(outputStream* out, Handle loader, ClassLoaderData* cld, TRAPS) {
  Handle string;
  if (loader.not_null()) {
    // Include the result of loader.toString() in the output. This allows
    // the user of the log to identify the class loader instance.
    JavaValue result(T_OBJECT);
    Klass* spec_klass = SystemDictionary::ClassLoader_klass();
    JavaCalls::call_virtual(&result,
                            loader,
                            spec_klass,
                            vmSymbols::toString_name(),
                            vmSymbols::void_string_signature(),
                            CHECK);
    assert(result.get_type() == T_OBJECT, "just checking");
    string = Handle(THREAD, (oop)result.get_jobject());
  }

  ResourceMark rm;
  out->print("create class loader data " INTPTR_FORMAT, p2i(cld));
  out->print(" for instance " INTPTR_FORMAT " of %s", p2i((void *)cld->class_loader()),
             cld->loader_name());

  if (string.not_null()) {
    out->print(": ");
    java_lang_String::print(string(), out);
  }
  out->cr();
}


void ClassLoaderDataGraph::oops_do(OopClosure* f, bool must_claim) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->oops_do(f, must_claim);
  }
}

void ClassLoaderDataGraph::keep_alive_oops_do(OopClosure* f, bool must_claim) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    if (cld->keep_alive()) {
      cld->oops_do(f, must_claim);
    }
  }
}

void ClassLoaderDataGraph::always_strong_oops_do(OopClosure* f, bool must_claim) {
  if (ClassUnloading) {
    keep_alive_oops_do(f, must_claim);
  } else {
    oops_do(f, must_claim);
  }
}

void ClassLoaderDataGraph::cld_do(CLDClosure* cl) {
  for (ClassLoaderData* cld = _head; cl != NULL && cld != NULL; cld = cld->next()) {
    cl->do_cld(cld);
  }
}

void ClassLoaderDataGraph::cld_unloading_do(CLDClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  // Only walk the head until any clds not purged from prior unloading
  // (CMS doesn't purge right away).
  for (ClassLoaderData* cld = _unloading; cld != _saved_unloading; cld = cld->next()) {
    assert(cld->is_unloading(), "invariant");
    cl->do_cld(cld);
  }
}

void ClassLoaderDataGraph::roots_cld_do(CLDClosure* strong, CLDClosure* weak) {
  for (ClassLoaderData* cld = _head;  cld != NULL; cld = cld->_next) {
    CLDClosure* closure = cld->keep_alive() ? strong : weak;
    if (closure != NULL) {
      closure->do_cld(cld);
    }
  }
}

void ClassLoaderDataGraph::keep_alive_cld_do(CLDClosure* cl) {
  roots_cld_do(cl, NULL);
}

void ClassLoaderDataGraph::always_strong_cld_do(CLDClosure* cl) {
  if (ClassUnloading) {
    keep_alive_cld_do(cl);
  } else {
    cld_do(cl);
  }
}

void ClassLoaderDataGraph::classes_do(KlassClosure* klass_closure) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->classes_do(klass_closure);
  }
}

void ClassLoaderDataGraph::classes_do(void f(Klass* const)) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->classes_do(f);
  }
}

void ClassLoaderDataGraph::methods_do(void f(Method*)) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->methods_do(f);
  }
}

void ClassLoaderDataGraph::modules_do(void f(ModuleEntry*)) {
  assert_locked_or_safepoint(Module_lock);
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->modules_do(f);
  }
}

void ClassLoaderDataGraph::modules_unloading_do(void f(ModuleEntry*)) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  // Only walk the head until any clds not purged from prior unloading
  // (CMS doesn't purge right away).
  for (ClassLoaderData* cld = _unloading; cld != _saved_unloading; cld = cld->next()) {
    assert(cld->is_unloading(), "invariant");
    cld->modules_do(f);
  }
}

void ClassLoaderDataGraph::packages_do(void f(PackageEntry*)) {
  assert_locked_or_safepoint(Module_lock);
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->packages_do(f);
  }
}

void ClassLoaderDataGraph::packages_unloading_do(void f(PackageEntry*)) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  // Only walk the head until any clds not purged from prior unloading
  // (CMS doesn't purge right away).
  for (ClassLoaderData* cld = _unloading; cld != _saved_unloading; cld = cld->next()) {
    assert(cld->is_unloading(), "invariant");
    cld->packages_do(f);
  }
}

void ClassLoaderDataGraph::loaded_classes_do(KlassClosure* klass_closure) {
  for (ClassLoaderData* cld = _head; cld != NULL; cld = cld->next()) {
    cld->loaded_classes_do(klass_closure);
  }
}

void ClassLoaderDataGraph::classes_unloading_do(void f(Klass* const)) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  // Only walk the head until any clds not purged from prior unloading
  // (CMS doesn't purge right away).
  for (ClassLoaderData* cld = _unloading; cld != _saved_unloading; cld = cld->next()) {
    assert(cld->is_unloading(), "invariant");
    cld->classes_do(f);
  }
}

#define FOR_ALL_DICTIONARY(X) for (ClassLoaderData* X = _head; X != NULL; X = X->next()) \
                                if (X->dictionary() != NULL)

// Walk classes in the loaded class dictionaries in various forms.
// Only walks the classes defined in this class loader.
void ClassLoaderDataGraph::dictionary_classes_do(void f(InstanceKlass*)) {
  FOR_ALL_DICTIONARY(cld) {
    cld->dictionary()->classes_do(f);
  }
}

// Only walks the classes defined in this class loader.
void ClassLoaderDataGraph::dictionary_classes_do(void f(InstanceKlass*, TRAPS), TRAPS) {
  FOR_ALL_DICTIONARY(cld) {
    cld->dictionary()->classes_do(f, CHECK);
  }
}

// Walks all entries in the dictionary including entries initiated by this class loader.
void ClassLoaderDataGraph::dictionary_all_entries_do(void f(InstanceKlass*, ClassLoaderData*)) {
  FOR_ALL_DICTIONARY(cld) {
    cld->dictionary()->all_entries_do(f);
  }
}

void ClassLoaderDataGraph::verify_dictionary() {
  FOR_ALL_DICTIONARY(cld) {
    cld->dictionary()->verify();
  }
}

void ClassLoaderDataGraph::print_dictionary(outputStream* st) {
  FOR_ALL_DICTIONARY(cld) {
    st->print("Dictionary for ");
    cld->print_value_on(st);
    st->cr();
    cld->dictionary()->print_on(st);
    st->cr();
  }
}

void ClassLoaderDataGraph::print_dictionary_statistics(outputStream* st) {
  FOR_ALL_DICTIONARY(cld) {
    ResourceMark rm;
    stringStream tempst;
    tempst.print("System Dictionary for %s", cld->loader_name());
    cld->dictionary()->print_table_statistics(st, tempst.as_string());
  }
}

GrowableArray<ClassLoaderData*>* ClassLoaderDataGraph::new_clds() {
  assert(_head == NULL || _saved_head != NULL, "remember_new_clds(true) not called?");

  GrowableArray<ClassLoaderData*>* array = new GrowableArray<ClassLoaderData*>();

  // The CLDs in [_head, _saved_head] were all added during last call to remember_new_clds(true);
  ClassLoaderData* curr = _head;
  while (curr != _saved_head) {
    if (!curr->claimed()) {
      array->push(curr);
      LogTarget(Debug, class, loader, data) lt;
      if (lt.is_enabled()) {
        LogStream ls(lt);
        ls.print("found new CLD: ");
        curr->print_value_on(&ls);
        ls.cr();
      }
    }

    curr = curr->_next;
  }

  return array;
}

bool ClassLoaderDataGraph::unload_list_contains(const void* x) {
  assert(SafepointSynchronize::is_at_safepoint(), "only safe to call at safepoint");
  for (ClassLoaderData* cld = _unloading; cld != NULL; cld = cld->next()) {
    if (cld->metaspace_or_null() != NULL && cld->metaspace_or_null()->contains(x)) {
      return true;
    }
  }
  return false;
}

#ifndef PRODUCT
bool ClassLoaderDataGraph::contains_loader_data(ClassLoaderData* loader_data) {
  for (ClassLoaderData* data = _head; data != NULL; data = data->next()) {
    if (loader_data == data) {
      return true;
    }
  }

  return false;
}
#endif // PRODUCT


// Move class loader data from main list to the unloaded list for unloading
// and deallocation later.
bool ClassLoaderDataGraph::do_unloading(BoolObjectClosure* is_alive_closure,
                                        bool clean_previous_versions) {

  ClassLoaderData* data = _head;
  ClassLoaderData* prev = NULL;
  bool seen_dead_loader = false;

  // Mark metadata seen on the stack only so we can delete unneeded entries.
  // Only walk all metadata, including the expensive code cache walk, for Full GC
  // and only if class redefinition and if there's previous versions of
  // Klasses to delete.
  bool walk_all_metadata = clean_previous_versions &&
                           JvmtiExport::has_redefined_a_class() &&
                           InstanceKlass::has_previous_versions_and_reset();
  MetadataOnStackMark md_on_stack(walk_all_metadata);

  // Save previous _unloading pointer for CMS which may add to unloading list before
  // purging and we don't want to rewalk the previously unloaded class loader data.
  _saved_unloading = _unloading;

  data = _head;
  while (data != NULL) {
    if (data->is_alive(is_alive_closure)) {
      // clean metaspace
      if (walk_all_metadata) {
        data->classes_do(InstanceKlass::purge_previous_versions);
      }
      data->free_deallocate_list();
      prev = data;
      data = data->next();
      continue;
    }
    seen_dead_loader = true;
    ClassLoaderData* dead = data;
    dead->unload();
    data = data->next();
    // Remove from loader list.
    // This class loader data will no longer be found
    // in the ClassLoaderDataGraph.
    if (prev != NULL) {
      prev->set_next(data);
    } else {
      assert(dead == _head, "sanity check");
      _head = data;
    }
    dead->set_next(_unloading);
    _unloading = dead;
  }

  if (seen_dead_loader) {
    data = _head;
    while (data != NULL) {
      // Remove entries in the dictionary of live class loader that have
      // initiated loading classes in a dead class loader.
      if (data->dictionary() != NULL) {
        data->dictionary()->do_unloading();
      }
      // Walk a ModuleEntry's reads, and a PackageEntry's exports
      // lists to determine if there are modules on those lists that are now
      // dead and should be removed.  A module's life cycle is equivalent
      // to its defining class loader's life cycle.  Since a module is
      // considered dead if its class loader is dead, these walks must
      // occur after each class loader's aliveness is determined.
      if (data->packages() != NULL) {
        data->packages()->purge_all_package_exports();
      }
      if (data->modules_defined()) {
        data->modules()->purge_all_module_reads();
      }
      data = data->next();
    }

    post_class_unload_events();
  }

  return seen_dead_loader;
}

void ClassLoaderDataGraph::purge() {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  ClassLoaderData* list = _unloading;
  _unloading = NULL;
  ClassLoaderData* next = list;
  bool classes_unloaded = false;
  while (next != NULL) {
    ClassLoaderData* purge_me = next;
    next = purge_me->next();
    delete purge_me;
    classes_unloaded = true;
  }
  if (classes_unloaded) {
    Metaspace::purge();
    set_metaspace_oom(false);
  }
}

int ClassLoaderDataGraph::resize_if_needed() {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  int resized = 0;
  if (Dictionary::does_any_dictionary_needs_resizing()) {
    FOR_ALL_DICTIONARY(cld) {
      if (cld->dictionary()->resize_if_needed()) {
        resized++;
      }
    }
  }
  return resized;
}

void ClassLoaderDataGraph::post_class_unload_events() {
#if INCLUDE_TRACE
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint!");
  if (Tracing::enabled()) {
    if (Tracing::is_event_enabled(TraceClassUnloadEvent)) {
      assert(_unloading != NULL, "need class loader data unload list!");
      _class_unload_time = Ticks::now();
      classes_unloading_do(&class_unload_event);
    }
    Tracing::on_unloading_classes();
  }
#endif
}

ClassLoaderDataGraphKlassIteratorAtomic::ClassLoaderDataGraphKlassIteratorAtomic()
    : _next_klass(NULL) {
  ClassLoaderData* cld = ClassLoaderDataGraph::_head;
  Klass* klass = NULL;

  // Find the first klass in the CLDG.
  while (cld != NULL) {
    assert_locked_or_safepoint(cld->metaspace_lock());
    klass = cld->_klasses;
    if (klass != NULL) {
      _next_klass = klass;
      return;
    }
    cld = cld->next();
  }
}

Klass* ClassLoaderDataGraphKlassIteratorAtomic::next_klass_in_cldg(Klass* klass) {
  Klass* next = klass->next_link();
  if (next != NULL) {
    return next;
  }

  // No more klasses in the current CLD. Time to find a new CLD.
  ClassLoaderData* cld = klass->class_loader_data();
  assert_locked_or_safepoint(cld->metaspace_lock());
  while (next == NULL) {
    cld = cld->next();
    if (cld == NULL) {
      break;
    }
    next = cld->_klasses;
  }

  return next;
}

Klass* ClassLoaderDataGraphKlassIteratorAtomic::next_klass() {
  Klass* head = _next_klass;

  while (head != NULL) {
    Klass* next = next_klass_in_cldg(head);

    Klass* old_head = Atomic::cmpxchg(next, &_next_klass, head);

    if (old_head == head) {
      return head; // Won the CAS.
    }

    head = old_head;
  }

  // Nothing more for the iterator to hand out.
  assert(head == NULL, "head is " PTR_FORMAT ", expected not null:", p2i(head));
  return NULL;
}

ClassLoaderDataGraphMetaspaceIterator::ClassLoaderDataGraphMetaspaceIterator() {
  _data = ClassLoaderDataGraph::_head;
}

ClassLoaderDataGraphMetaspaceIterator::~ClassLoaderDataGraphMetaspaceIterator() {}

#ifndef PRODUCT
// callable from debugger
extern "C" int print_loader_data_graph() {
  ClassLoaderDataGraph::dump_on(tty);
  return 0;
}

void ClassLoaderDataGraph::verify() {
  for (ClassLoaderData* data = _head; data != NULL; data = data->next()) {
    data->verify();
  }
}

void ClassLoaderDataGraph::dump_on(outputStream * const out) {
  for (ClassLoaderData* data = _head; data != NULL; data = data->next()) {
    data->dump(out);
  }
  MetaspaceAux::dump(out);
}
#endif // PRODUCT

void ClassLoaderData::print_value_on(outputStream* out) const {
  if (class_loader() == NULL) {
    out->print("NULL class loader");
  } else {
    out->print("class loader " INTPTR_FORMAT " ", p2i(this));
    class_loader()->print_value_on(out);
  }
}

void ClassLoaderData::print_on(outputStream* out) const {
  if (class_loader() == NULL) {
    out->print("NULL class loader");
  } else {
    out->print("class loader " INTPTR_FORMAT " ", p2i(this));
    class_loader()->print_on(out);
  }
}

#if INCLUDE_TRACE

Ticks ClassLoaderDataGraph::_class_unload_time;

void ClassLoaderDataGraph::class_unload_event(Klass* const k) {
  assert(k != NULL, "invariant");

  // post class unload event
  EventClassUnload event(UNTIMED);
  event.set_endtime(_class_unload_time);
  event.set_unloadedClass(k);
  event.set_definingClassLoader(k->class_loader_data());
  event.commit();
}

#endif // INCLUDE_TRACE
