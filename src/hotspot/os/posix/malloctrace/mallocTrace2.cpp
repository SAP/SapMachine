/*
 * Copyright (c) 2023 SAP SE. All rights reserved.
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

#if defined(LINUX) || defined(__APPLE__)

#include "precompiled.hpp"

#include "jvm_io.h"
#include "mallochooks.h"
#include "malloctrace/mallocTrace2.hpp"

#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/timer.hpp"
#include "utilities/ostream.hpp"
#include "utilities/ticks.hpp"

#include <pthread.h>
#include <stdlib.h>

#if !defined(__APPLE__)
#include <malloc.h>
#endif

// To test in jtreg tests use
// JTREG="JAVA_OPTIONS=-XX:+UseMallocHooks -XX:+MallocTraceAtStartup -XX:+MallocTraceDump -XX:MallocTraceDumpInterval=10 -XX:MallocTraceDumpOutput=`pwd`/mtrace_@pid.txt -XX:ErrorFile=`pwd`/hs_err%p.log"

// A simple smoke test
// jconsole -J-XX:+UseMallocHooks -J-XX:+MallocTraceAtStartup -J-XX:+MallocTraceDump -J-XX:MallocTraceStackDepth=12 -J-XX:MallocTraceDumpInterval=10


// Some compile time constants for the maps.

#define MAX_STACK_MAP_LOAD 0.5
#define STACK_MAP_INIT_SIZE 1024

#define MAX_ALLOC_MAP_LOAD 2.5
#define ALLOC_MAP_INIT_SIZE 1024

// Must be a power of two minus 1.
#define MAX_FRAMES 31

// The number of top frames to skip.
#define FRAMES_TO_SKIP 0

// Must be a power of two.
#define NR_OF_STACK_MAPS 16
#define NR_OF_ALLOC_MAPS 16

namespace sap {

static bool is_non_empty_string(char const* str) {
  return (str != NULL) && (str[0] != '\0');
}

static uint64_t parse_timespan_part(char const* start, char const* end, char const** error) {
  char buf[32];

  // Strip trailing spaces.
  while ((end > start) && (end[-1] == ' ')) {
    end--;
  }

  if (start == end) {
    *error = "empty time";
    return 0;
  }

  if (((size_t) (end - start)) >= sizeof(buf) - 1) {
    *error = "time too long";
    return 0;
  }

  memcpy(buf, start, end - start);
  buf[end - start] = '\0';

  char* found_end;
  uint64_t result = (uint64_t) strtoull(buf, &found_end, 10);

  if ((found_end != end) && (*found_end != '\0')) {
    *error = "Could not parse integer";
  }

  return (uint64_t) atoll(buf);
}

static uint64_t parse_timespan(char const* spec, char const** error = NULL) {
  uint64_t result = 0;
  char const* start = spec;
  char const* pos = start;
  char const* backup_error;
  uint64_t limit_in_days = 365;

  if (error == NULL) {
    error = &backup_error;
  }

  *error = NULL;

  while (*pos != '\0') {
    switch (*pos) {
      case ' ':
        if (pos == start) {
          start++;
        }
        break;

      case 's':
        result += parse_timespan_part(start, pos, error);
        start = pos + 1;
        break;

      case 'm':
        result += 60 * parse_timespan_part(start, pos, error);
        start = pos + 1;
        break;

      case 'h':
        result += 60 * 60 * parse_timespan_part(start, pos, error);
        start = pos + 1;
        break;

      case 'd':
        result += 24 * 60 * 60 * parse_timespan_part(start, pos, error);
        start = pos + 1;
        break;

      default:
        if ((*pos < '0') || (*pos > '9')) {
          *error = "Unexpected character";
          return 0;
        }
    }

    pos++;
  }

  if (pos != start) {
    *error = "time wihtout unit";
  }

  if (result / (24 * 60 * 60) > limit_in_days) {
    *error = "time too large";
  }

  return result;
}

// Keep sap namespace free from implementation classes.
namespace mallocStatImpl {

// Allocates memory of the same size. It's pretty fast, but doesn't return
// free memory to the OS.
class Allocator {
private:
  // We need padding, since we have arrays of this class used in parallel.
  char          _pre_pad[DEFAULT_CACHE_LINE_SIZE];
  real_funcs_t* _funcs;
  size_t        _allocation_size;
  int           _entries_per_chunk;
  void**        _chunks;
  int           _nr_of_chunks;
  void**        _free_list;
  size_t        _free_entries;
  char          _post_pad[DEFAULT_CACHE_LINE_SIZE];

public:
  Allocator(size_t allocation_size, int entries_per_chunk, real_funcs_t* funcs);
  ~Allocator();

  void* allocate();
  void free(void* ptr);
  size_t allocated();
  size_t unused();
};

Allocator::Allocator(size_t allocation_size, int entries_per_chunk, real_funcs_t* funcs) :
  _funcs(funcs),
  _allocation_size(align_up(allocation_size, 8)), // We need no stricter alignment
  _entries_per_chunk(entries_per_chunk),
  _chunks(NULL),
  _nr_of_chunks(0),
  _free_list(NULL),
  _free_entries(0) {
}

Allocator::~Allocator() {
  for (int i = 0; i < _nr_of_chunks; ++i) {
    _funcs->free(_chunks[i]);
  }
}

void* Allocator::allocate() {
  if (_free_list != NULL) {
    void** result = _free_list;
    _free_list = (void**) result[0];
    assert(_free_entries > 0, "free entries count invalid.");
    _free_entries -= 1;

    return result;
  }

  // We need a new chunk.
  char* new_chunk = (char*) _funcs->malloc(_entries_per_chunk * _allocation_size);

  if (new_chunk == NULL) {
    return NULL;
  }

  void** new_chunks = (void**) _funcs->realloc(_chunks, sizeof(void**) * (_nr_of_chunks + 1));

  if (new_chunks == NULL) {
    return NULL;
  }

  new_chunks[_nr_of_chunks] = new_chunk;
  _nr_of_chunks += 1;
  _chunks = new_chunks;

  for (int i = 0; i < _entries_per_chunk; ++i) {
    free(new_chunk + i * _allocation_size);
  }

  return allocate();
}

void Allocator::free(void* ptr) {
  if (ptr != NULL) {
    void** as_array = (void**) ptr;
    as_array[0] = (void*) _free_list;
    _free_list = as_array;
    _free_entries += 1;
  }
}

size_t Allocator::allocated() {
  return _allocation_size * _entries_per_chunk * _nr_of_chunks; 
}

size_t Allocator::unused() {
#if defined(ASSERT)
  size_t real_free_entries = 0;
  void** entry = _free_list;

  while (entry != NULL) {
    real_free_entries += 1;
    entry = (void**) entry[0];
  }

  assert(_free_entries == real_free_entries, "free entries inconsistent");
#endif

  return _allocation_size * _free_entries;
}

class AddressHashSet {
private:
  int           _mask;
  int           _count;
  address*      _set;
  real_funcs_t* _funcs;

  int get_slot(address to_check);

public:
  AddressHashSet(real_funcs_t* funcs, bool enabled);
  ~AddressHashSet();

  bool contains(address to_check);
  bool add(address to_add);
  size_t allocated();
  double load();
};

AddressHashSet::AddressHashSet(real_funcs_t* funcs, bool enabled) :
  _mask(enabled ? 0 : 1),
  _count(0),
  _set(NULL),
  _funcs(funcs) {
}

AddressHashSet::~AddressHashSet() {
  _funcs->free(_set);
}

int AddressHashSet::get_slot(address to_check) {
  assert(to_check != NULL, "Invalid value");

  if (_set == NULL) {
    // Initialize lazily.
    if (_mask == 0) {
      _mask = 8191;
      _set = (address*) _funcs->calloc(_mask + 1, sizeof(address));
    }

    // When we overflow, return treat each address as not be contained. This is
    // the safe behaviour for our use case.
    return -1;
  }

  int slot = (int) (((uintptr_t) to_check) & _mask);

  while (_set[slot] != NULL) {
    if (_set[slot] == to_check) {
      return slot;
    }

    slot = (slot + 1) & _mask;
  }

  return slot;
}

bool AddressHashSet::contains(address to_check) {
  int slot = get_slot(to_check);

  return (slot >= 0) && (_set[slot] != NULL);
}

bool AddressHashSet::add(address to_add) {
  int slot = get_slot(to_add);

  if ((slot < 0) || (_set[slot] != NULL)) {
      // Already present.
      return false;
  }

  // Check if we should resize.
  if (_count * 2 > _mask) {
    address* old_set = _set;
    int old_mask = _mask;

    _mask = _mask * 2 + 1;
    _count = 0;

    _set = (address*) _funcs->calloc(_mask + 1, sizeof(address));

    // If full, we fall back to always return false.
    if (_set == NULL) {
      _funcs->free(old_set);

      return false;
    }

    for (int i = 0; i <= old_mask; ++i) {
      if (old_set[i] != NULL) {
        add(old_set[i]);
      }
    }

    _funcs->free(old_set);
    add(to_add);
  } else {
    _set[slot] = to_add;
    _count += 1;
  }

  return true;
}

size_t AddressHashSet::allocated() {
  if (_set == NULL) {
    return 0;
  }

  return (_mask + 1) * sizeof(address);
}

double AddressHashSet::load() {
  if (_set == NULL) {
    return 0.0;
  }

  return 1.0 * _count / (_mask + 1);
}

// A pthread mutex usable in arrays.
struct Lock {
  char            _pre_pad[DEFAULT_CACHE_LINE_SIZE];
  pthread_mutex_t _lock;
};

class Locker : public StackObj {
private:
  pthread_mutex_t* _mutex;

public:
  Locker(Lock& mutex, bool disabled = false);
  ~Locker();
};

Locker::Locker(Lock& lock, bool disabled) :
  _mutex(disabled ? NULL : &lock._lock) {
  if ((_mutex != NULL) && (pthread_mutex_lock(_mutex) != 0)) {
    fatal("Could not lock mutex");
  }
}

Locker::~Locker() {
  if ((_mutex != NULL) && (pthread_mutex_unlock(_mutex) != 0)) {
    fatal("Could not unlock mutex");
  }
}


//
//
//
//
// Class StatEntry
//
//
//
//

class StatEntry {
private:
  StatEntry* _next;
  uint64_t   _hash_and_nr_of_frames;
  uint64_t   _size;
  uint64_t   _count;
  address    _frames[1];

public:
  StatEntry(size_t hash, size_t size, int nr_of_frames, address* frames) :
    _next(NULL),
    _hash_and_nr_of_frames((hash * (MAX_FRAMES + 1)) + nr_of_frames),
    _size(size),
    _count(1) {
    memcpy(_frames, frames, sizeof(address) * nr_of_frames);
    assert(nr_of_frames <= MAX_FRAMES, "too many frames");
  }
  uint64_t hash() {
    return _hash_and_nr_of_frames / (MAX_FRAMES + 1);
  }

  int map_index() {
    return hash() & (NR_OF_STACK_MAPS - 1);
  }

  StatEntry* next() {
    return _next;
  }

  void set_next(StatEntry* next) {
    _next = next;
  }

  void add_allocation(size_t size) {
    _size += size;
    _count += 1;
  }

  void remove_allocation(size_t size) {
    assert(_size >= size, "Size cannot get negative");
    assert(_count >= 1, "Count cannot get negative");
    _size -= size;
    _count -= 1;
  }

  uint64_t size() {
    return _size;
  }

  uint64_t count() {
    return _count;
  }

  int nr_of_frames() {
    return _hash_and_nr_of_frames & MAX_FRAMES;
  }

  address* frames() {
    return _frames;
  }
};


struct StatEntryCopy {
  StatEntry* _entry;
  uint64_t   _size;
  uint64_t   _count;
};

// The entry for a single allocation. Note that we don't store the pointer itself
// but use the hash code instead. Our hash function is invertible, so this is OK.
class AllocEntry {
private:
  uint64_t    _hash;
  StatEntry*  _entry;
  AllocEntry* _next;

#if defined(ASSERT)
  void*       _ptr; // Is not really needed, but helps debugging.
#endif

public:

#if defined(ASSERT)

  AllocEntry(uint64_t hash, StatEntry* entry, AllocEntry* next, void* ptr) :
    _hash(hash),
    _entry(entry),
    _next(next),
    _ptr(ptr) {
  }

#else

  AllocEntry(uint64_t hash, StatEntry* entry, AllocEntry* next) :
    _hash(hash),
    _entry(entry),
    _next(next) {
  }

#endif

  uint64_t hash() {
    return _hash;
  }

  StatEntry* entry() {
    return _entry;
  }

  AllocEntry* next() {
    return _next;
  }

  void set_next(AllocEntry* next) {
    _next = next;
  }

  AllocEntry** next_ptr() {
    return &_next;
  }

#if defined(ASSERT)
  void* ptr() {
    return _ptr;
  }
#endif
};



static register_hooks_t* register_hooks;
static get_real_funcs_t* get_real_funcs;

#if defined(__APPLE__)
#define LD_PRELOAD "DYLD_INSERT_LIBRARIES"
#define LIB_MALLOC_HOOKS "libmallochooks.dylib"
#else
#define LD_PRELOAD  "LD_PRELOAD"
#define LIB_MALLOC_HOOKS "libmallochooks.so"
#endif

static void print_needed_preload_env(outputStream* st) {
  st->print_cr("%s=%s/%s", LD_PRELOAD, Arguments::get_dll_dir(), LIB_MALLOC_HOOKS);
  st->print_cr("Its current value is %s", getenv(LD_PRELOAD));
}

static void remove_malloc_hooks_from_env() {
  char const* env = ::getenv(LD_PRELOAD);

  if ((env == NULL) || (env[0] == '\0')) {
     return;
  }

  // Create a env with ':' prepended and appended. This makes the
  // code easier.
  stringStream guarded_env;
  guarded_env.print(":%s:", env);

  stringStream new_env;
  size_t len = strlen(LIB_MALLOC_HOOKS);
  char const* base = guarded_env.base();
  char const* pos = base;

  while ((pos = strstr(pos, LIB_MALLOC_HOOKS)) != NULL) {
    if (pos[len] != ':') {
      pos += 1;

      continue;
    }

    if (pos[-1] == ':') {
      new_env.print("%.*s%s", (int) (pos - base) - 1, base, pos + len);
    } else if (pos[-1] == '/') {
      char const* c = pos - 1;

      while (c[0] != ':') {
        --c;
      }

      new_env.print("%.*s%s", (int) (c - base + 1), base, pos + len + 1);
    } else {
      pos += 1;

      continue;
    }

    if (new_env.size() <= 2) {
      ::unsetenv(LD_PRELOAD);
    } else {
      stringStream ss;
      ss.print("%.*s", MAX(0, (int) (new_env.size() - 2)), new_env.base() + 1);
      ::setenv(LD_PRELOAD, ss.base(), 1);
    }

    return;
  }
}

static real_funcs_t* setup_hooks(registered_hooks_t* hooks, outputStream* st) {
  if (register_hooks == NULL) {
    register_hooks = (register_hooks_t*) dlsym((void*) RTLD_DEFAULT, REGISTER_HOOKS_NAME);
    get_real_funcs = (get_real_funcs_t*) dlsym((void*) RTLD_DEFAULT, GET_REAL_FUNCS_NAME);

    if ((register_hooks == NULL) || (get_real_funcs == NULL)) {
      if (UseMallocHooks) {
        st->print_raw_cr("Could not find preloaded libmallochooks while -XX:+UseMallocHooks is set. " \
                         "This usually happens if the VM is not loaded via the JDK launcher (e.g. " \
                         "java.exe). In this case you must preload the library by setting the " \
                         "following environment variable: ");
        print_needed_preload_env(st);
      } else {
        st->print_cr("Could not find preloaded libmallochooks. Try using -XX:+UseMallocHooks " \
                     "Vm option to automatically preload it using the JDK launcher. Or you can set " \
                     "the following environment variable: ");
        print_needed_preload_env(st);
      }

      st->print_raw_cr("VM arguments:");
      Arguments::print_summary_on(st);

      return NULL;
    }
  }

  register_hooks(hooks);

  return get_real_funcs();
}

typedef int backtrace_func_t(void** stacks, int max_depth);

// A value usable in arrays.
template<typename T> struct Padded {
public:
  char _pre_pad[DEFAULT_CACHE_LINE_SIZE];
  T    _val;
};

class MallocStatisticImpl : public AllStatic {
private:

  static real_funcs_t*      _funcs;
  static backtrace_func_t*  _backtrace;
  static char const*        _backtrace_name;
  static bool               _use_backtrace;
  static volatile bool      _initialized;
  static bool               _enabled;
  static bool               _shutdown;
  static bool               _track_free;
  static bool               _detailed_stats;
  static bool               _tried_to_load_backtrace;
  static int                _max_frames;
  static registered_hooks_t _malloc_stat_hooks;
  static Lock               _malloc_stat_lock;
  static pthread_key_t      _malloc_suspended;
  static pthread_key_t      _malloc_rd_enabled;

  // the +1 is for cache line reasons, so we ensure the last use entry
  // doesn't share a cache line with another object.
  static StatEntry**        _stack_maps[NR_OF_STACK_MAPS];
  static Lock               _stack_maps_lock[NR_OF_STACK_MAPS + 1];
  static int                _stack_maps_mask[NR_OF_STACK_MAPS];
  static Padded<int>        _stack_maps_size[NR_OF_STACK_MAPS + 1];
  static int                _stack_maps_limit[NR_OF_STACK_MAPS];
  static Allocator*         _stack_maps_alloc[NR_OF_STACK_MAPS];
  static int                _entry_size;

  static AllocEntry**       _alloc_maps[NR_OF_ALLOC_MAPS];
  static Lock               _alloc_maps_lock[NR_OF_ALLOC_MAPS + 1];
  static int                _alloc_maps_mask[NR_OF_ALLOC_MAPS];
  static Padded<int>        _alloc_maps_size[NR_OF_ALLOC_MAPS + 1];
  static int                _alloc_maps_limit[NR_OF_ALLOC_MAPS];
  static Allocator*         _alloc_maps_alloc[NR_OF_ALLOC_MAPS];

  static uint64_t           _to_track_mask;
  static uint64_t           _to_track_limit;

  static volatile uint64_t  _stack_walk_time;
  static volatile uint64_t  _stack_walk_count;
  static volatile uint64_t  _tracked_ptrs;
  static volatile uint64_t  _not_tracked_ptrs;
  static volatile uint64_t  _failed_frees;

  static void*              _rainy_day_fund;
  static registered_hooks_t _rainy_day_hooks;
  static Lock               _rainy_day_fund_lock;
  static volatile registered_hooks_t* _rainy_day_hooks_ptr;

  // The hooks.
  static void* malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) ;
  static void* calloc_hook(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size);
  static void* realloc_hook(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size);
  static void  free_hook(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size);
  static int   posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size);
  static void* memalign_hook(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size);
  static void* aligned_alloc_hook(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size);
  static void* valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size);
  static void* pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size);

  // The hooks used after we use the rainy day fund
  static void* malloc_hook_rd(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) ;
  static void* calloc_hook_rd(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size);
  static void* realloc_hook_rd(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size);
  static void  free_hook_rd(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size);
  static int   posix_memalign_hook_rd(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size);
  static void* memalign_hook_rd(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size);
  static void* aligned_alloc_hook_rd(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size);
  static void* valloc_hook_rd(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size);
  static void* pvalloc_hook_rd(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size);
  static void wait_for_rainy_day_fund();

  static StatEntry* record_allocation_size(size_t to_add, int nr_of_frames, address* frames);
  static void record_allocation(void* ptr, uint64_t hash, int nr_of_frames, address* frames);
  static StatEntry* record_free(void* ptr, uint64_t hash, size_t size);

  static uint64_t ptr_hash(void* ptr);
  static bool should_track(uint64_t hash);

  static void resize_stack_map(int map, int new_mask);
  static void resize_alloc_map(int map, int new_mask);
  static void cleanup_for_stack_map(int map);
  static void cleanup_for_alloc_map(int map);
  static void cleanup();

  static bool dump_entry(outputStream* st, StatEntryCopy* entry, int index,
                         uint64_t total_size, uint64_t total_count, int total_entries,
                         char const* filter, AddressHashSet* filter_cache);

public:
  static void initialize();
  static bool rainy_day_fund_used();
  static bool enable(outputStream* st, TraceSpec const& spec);
  static bool disable(outputStream* st);
  static bool dump(outputStream* msg_stream, outputStream* dump_stream, DumpSpec const& spec);
  static void shutdown();
};

registered_hooks_t MallocStatisticImpl::_malloc_stat_hooks = {
  MallocStatisticImpl::malloc_hook,
  MallocStatisticImpl::calloc_hook,
  MallocStatisticImpl::realloc_hook,
  MallocStatisticImpl::free_hook,
  MallocStatisticImpl::posix_memalign_hook,
  MallocStatisticImpl::memalign_hook,
  MallocStatisticImpl::aligned_alloc_hook,
  MallocStatisticImpl::valloc_hook,
  MallocStatisticImpl::pvalloc_hook
};

registered_hooks_t  MallocStatisticImpl::_rainy_day_hooks = {
  MallocStatisticImpl::malloc_hook_rd,
  MallocStatisticImpl::calloc_hook_rd,
  MallocStatisticImpl::realloc_hook_rd,
  MallocStatisticImpl::free_hook_rd,
  MallocStatisticImpl::posix_memalign_hook_rd,
  MallocStatisticImpl::memalign_hook_rd,
  MallocStatisticImpl::aligned_alloc_hook_rd,
  MallocStatisticImpl::valloc_hook_rd,
  MallocStatisticImpl::pvalloc_hook_rd
};

real_funcs_t*     MallocStatisticImpl::_funcs;
backtrace_func_t* MallocStatisticImpl::_backtrace;
char const*       MallocStatisticImpl::_backtrace_name;
bool              MallocStatisticImpl::_use_backtrace;
volatile bool     MallocStatisticImpl::_initialized;
bool              MallocStatisticImpl::_enabled;
bool              MallocStatisticImpl::_shutdown;
bool              MallocStatisticImpl::_track_free;
bool              MallocStatisticImpl::_detailed_stats;
bool              MallocStatisticImpl::_tried_to_load_backtrace;
int               MallocStatisticImpl::_max_frames;
Lock              MallocStatisticImpl::_malloc_stat_lock;
pthread_key_t     MallocStatisticImpl::_malloc_suspended;
pthread_key_t     MallocStatisticImpl::_malloc_rd_enabled;
StatEntry**       MallocStatisticImpl::_stack_maps[NR_OF_STACK_MAPS];
Lock              MallocStatisticImpl::_stack_maps_lock[NR_OF_STACK_MAPS + 1];
int               MallocStatisticImpl::_stack_maps_mask[NR_OF_STACK_MAPS];
Padded<int>       MallocStatisticImpl::_stack_maps_size[NR_OF_STACK_MAPS + 1];
int               MallocStatisticImpl::_stack_maps_limit[NR_OF_STACK_MAPS];
Allocator*        MallocStatisticImpl::_stack_maps_alloc[NR_OF_STACK_MAPS];
int               MallocStatisticImpl::_entry_size;
AllocEntry**      MallocStatisticImpl::_alloc_maps[NR_OF_ALLOC_MAPS];
Lock              MallocStatisticImpl::_alloc_maps_lock[NR_OF_ALLOC_MAPS + 1];
int               MallocStatisticImpl::_alloc_maps_mask[NR_OF_ALLOC_MAPS];
Padded<int>       MallocStatisticImpl::_alloc_maps_size[NR_OF_ALLOC_MAPS + 1];
int               MallocStatisticImpl::_alloc_maps_limit[NR_OF_ALLOC_MAPS];
Allocator*        MallocStatisticImpl::_alloc_maps_alloc[NR_OF_ALLOC_MAPS];
uint64_t          MallocStatisticImpl::_to_track_mask;
uint64_t          MallocStatisticImpl::_to_track_limit;
volatile uint64_t MallocStatisticImpl::_stack_walk_time;
volatile uint64_t MallocStatisticImpl::_stack_walk_count;
volatile uint64_t MallocStatisticImpl::_tracked_ptrs;
volatile uint64_t MallocStatisticImpl::_not_tracked_ptrs;
volatile uint64_t MallocStatisticImpl::_failed_frees;
void*             MallocStatisticImpl::_rainy_day_fund;
Lock              MallocStatisticImpl::_rainy_day_fund_lock;

volatile registered_hooks_t*
                  MallocStatisticImpl::_rainy_day_hooks_ptr = &MallocStatisticImpl::_rainy_day_hooks;

#define CAPTURE_STACK() \
  address frames[MAX_FRAMES + FRAMES_TO_SKIP]; \
  uint64_t ticks = _detailed_stats ? Ticks::now().nanoseconds() : 0; \
  int nr_of_frames = 0; \
  if (_max_frames <= 2) { \
    /* Skip, since we will fill it it later anyway. */ \
  } else if (_use_backtrace) { \
    nr_of_frames = _backtrace((void**) frames, _max_frames + FRAMES_TO_SKIP); \
  } else { \
    /* We have to unblock SIGSEGV signal handling, since os::is_first_C_frame()
       calls SafeFetch, which needs the proper handling of SIGSEGV. */ \
    sigset_t curr, old; \
    sigemptyset(&curr); \
    sigaddset(&curr, SIGSEGV); \
    pthread_sigmask(SIG_UNBLOCK, &curr, &old); \
    frame fr = os::current_frame(); \
    while (fr.pc() && nr_of_frames < _max_frames + FRAMES_TO_SKIP) { \
      frames[nr_of_frames] = fr.pc(); \
      nr_of_frames += 1; \
      if (nr_of_frames >= _max_frames + FRAMES_TO_SKIP) break; \
      if (fr.fp() == NULL || fr.cb() != NULL || fr.sender_pc() == NULL || os::is_first_C_frame(&fr)) \
        break; \
      fr = os::get_sender_for_C_frame(&fr); \
    } \
    pthread_sigmask(SIG_SETMASK, &old, NULL); \
  } \
  /* We know at least the function and the caller. */ \
  if (nr_of_frames < 2) { \
    frames[0] = real_func; \
    frames[1] = (address) caller_address; \
    nr_of_frames = MAX2(2, _max_frames); \
  } \
  if (_detailed_stats) { \
    Atomic::add(&_stack_walk_time, Ticks::now().nanoseconds() - ticks); \
    Atomic::add(&_stack_walk_count, (uint64_t) 1); \
  }

#if defined(ASSERT)
static uint64_t ptr_hash_backup(void* ptr) {
  uint64_t hash = (uint64_t) ptr;
  hash = (~hash) + (hash << 21);
  hash = hash ^ (hash >> 24);
  hash = hash * 265;
  hash = hash ^ (hash >> 14);
  hash = hash * 21;
  hash = hash ^ (hash >> 28);
  hash = hash + (hash << 31);

  return hash;
}
#endif

static void after_child_fork() {
  if (register_hooks != NULL) {
    register_hooks(NULL);
  }
}

uint64_t MallocStatisticImpl::ptr_hash(void* ptr) {
  if (!_track_free && (_to_track_mask == 0)) {
    return 0;
  }

  uint64_t hash = (uint64_t) ptr;
  hash = (~hash) + (hash << 21);
  hash = hash ^ (hash >> 24);
  hash = (hash + (hash << 3)) + (hash << 8);
  hash = hash ^ (hash >> 14);
  hash = (hash + (hash << 2)) + (hash << 4);
  hash = hash ^ (hash >> 28);
  hash = hash + (hash << 31);

  assert(hash == ptr_hash_backup(ptr), "Must be the same");

  return hash;
}

bool MallocStatisticImpl::should_track(uint64_t hash) {
  if (_detailed_stats) {
    if ((hash & _to_track_mask) < _to_track_limit) {
      Atomic::add(&_tracked_ptrs, (uint64_t) 1);
    } else {
      Atomic::add(&_not_tracked_ptrs, (uint64_t) 1);
    }
  }

  return (hash & _to_track_mask) < _to_track_limit;
}

#define malloc_not_suspended() (pthread_getspecific(_malloc_suspended) == NULL)

void* MallocStatisticImpl::malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc,
                                       malloc_size_func_t real_malloc_size) {
  void* result = real_malloc(size);
  uint64_t hash = ptr_hash(result);
  address real_func = (address) malloc;

  if ((result != NULL) && should_track(hash) && malloc_not_suspended()) {
    CAPTURE_STACK();

    if (_track_free) {
      record_allocation(result, hash, nr_of_frames, frames);
    } else {
      record_allocation_size(size, nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::calloc_hook(size_t elems, size_t size, void* caller_address,
                                       calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size) {
  void* result = real_calloc(elems, size);
  uint64_t hash = ptr_hash(result);
  address real_func = (address) calloc;

  if ((result != NULL) && should_track(hash) && malloc_not_suspended()) {
    CAPTURE_STACK();

    if (_track_free) {
      record_allocation(result, hash, nr_of_frames, frames);
    } else {
      record_allocation_size(elems * size, nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::realloc_hook(void* ptr, size_t size, void* caller_address,
                                        realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size) {
  size_t old_size = ptr != NULL ? real_malloc_size(ptr) : 0;
  uint64_t old_hash = ptr_hash(ptr);
  address real_func = (address) realloc;

  // We have to speculate the realloc does not fail, since realloc itself frees
  // the ptr potentially and another thread might get it from malloc and tries
  // to add to the alloc hash map before we could remove it here.
  StatEntry* freed_entry = NULL;

  if (_track_free && (ptr != NULL) && should_track(old_hash)) {
    freed_entry = record_free(ptr, old_hash, old_size);
  }

  void* result = real_realloc(ptr, size);

  if ((result == NULL) && (freed_entry != NULL) && (size > 0)) {
    // We failed, but we already removed the freed memory, so we have to re-add it.
    record_allocation(ptr, old_hash, freed_entry->nr_of_frames(), freed_entry->frames());

    return NULL;
  }

  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && should_track(hash)) {
    CAPTURE_STACK();

    if (_track_free) {
      if (should_track(hash) && malloc_not_suspended()) {
        record_allocation(result, hash, nr_of_frames, frames);
      }
    } else if ((old_size < size) && should_track(hash) && malloc_not_suspended()) {
      // Track the additional allocate bytes. This is somewhat wrong, since
      // we don't know the requested size of the original allocation and
      // old_size might be greater.
      record_allocation_size(size - old_size, nr_of_frames, frames);
    }
  }

  return result;
}

void MallocStatisticImpl::free_hook(void* ptr, void* caller_address, free_func_t* real_free,
                                    malloc_size_func_t real_malloc_size) {
  if ((ptr != NULL) && _track_free) {
    uint64_t hash = ptr_hash(ptr);

    if (should_track(hash)) {
      record_free(ptr, hash, real_malloc_size(ptr));
    }
  }

  real_free(ptr);
}

int MallocStatisticImpl::posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address,
                                             posix_memalign_func_t* real_posix_memalign,
                                             malloc_size_func_t real_malloc_size) {
  int result = real_posix_memalign(ptr, align, size);
  uint64_t hash = ptr_hash(*ptr);
  address real_func = (address) posix_memalign;

  if ((result == 0) && should_track(hash) && malloc_not_suspended()) {
    CAPTURE_STACK();

    if (_track_free) {
      record_allocation(*ptr, hash, nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(*ptr), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::memalign_hook(size_t align, size_t size, void* caller_address,
                                         memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size) {
  void* result = real_memalign(align, size);
  uint64_t hash = ptr_hash(result);
#if !defined(__APPLE__)
  address real_func = (address) memalign;
#else
  address real_func = (address) memalign_hook;
#endif

  if ((result != NULL) && should_track(hash) && malloc_not_suspended()) {
    CAPTURE_STACK();

    if (_track_free) {
      record_allocation(result, hash, nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::aligned_alloc_hook(size_t align, size_t size, void* caller_address,
                                              aligned_alloc_func_t* real_aligned_alloc,
                                              malloc_size_func_t real_malloc_size) {
  void* result = real_aligned_alloc(align, size);
  uint64_t hash = ptr_hash(result);
#if !defined(__APPLE__)
  address real_func = (address) aligned_alloc;
#else
  address real_func = (address) aligned_alloc_hook;
#endif

  if ((result != NULL) && should_track(hash) && malloc_not_suspended()) {
    CAPTURE_STACK();

    if (_track_free) {
      record_allocation(result, hash, nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc,
                                       malloc_size_func_t real_malloc_size) {
  void* result = real_valloc(size);
  uint64_t hash = ptr_hash(result);
#if defined(__GLIBC__) || defined(__APPLE__)
  address real_func = (address) valloc;
#else
  address real_func = (address) valloc_hook;
#endif

  if ((result != NULL) && should_track(hash) && malloc_not_suspended()) {
    CAPTURE_STACK();

    if (_track_free) {
      record_allocation(result, hash, nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc,
                                        malloc_size_func_t real_malloc_size) {
  void* result = real_pvalloc(size);
  uint64_t hash = ptr_hash(result);
#if defined(__GLIBC__)
  address real_func = (address) pvalloc;
#else
  address real_func = (address) pvalloc_hook;
#endif

  if ((result != NULL) && should_track(hash) && malloc_not_suspended()) {
    CAPTURE_STACK();

    if (_track_free) {
      record_allocation(result, hash, nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
}


void* MallocStatisticImpl::malloc_hook_rd(size_t size, void* caller_address, malloc_func_t* real_malloc,
                                          malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_malloc(size);
}

void* MallocStatisticImpl::calloc_hook_rd(size_t elems, size_t size, void* caller_address,
                                         calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_calloc(elems, size);
}

void* MallocStatisticImpl::realloc_hook_rd(void* ptr, size_t size, void* caller_address,
                                           realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_realloc(ptr, size);
}

void  MallocStatisticImpl::free_hook_rd(void* ptr, void* caller_address, free_func_t* real_free,
                                        malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  real_free(ptr);
}

int   MallocStatisticImpl::posix_memalign_hook_rd(void** ptr, size_t align, size_t size, void* caller_address,
                                                 posix_memalign_func_t* real_posix_memalign,
                                                 malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_posix_memalign(ptr, align, size);
}

void* MallocStatisticImpl::memalign_hook_rd(size_t align, size_t size, void* caller_address,
                                            memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_memalign(align, size);
}

void* MallocStatisticImpl::aligned_alloc_hook_rd(size_t align, size_t size, void* caller_address,
                                                 aligned_alloc_func_t* real_aligned_alloc,
                                                 malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_aligned_alloc(align, size);
}

void* MallocStatisticImpl::valloc_hook_rd(size_t size, void* caller_address, valloc_func_t* real_valloc,
                                          malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_valloc(size);
}

void* MallocStatisticImpl::pvalloc_hook_rd(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc,
                                           malloc_size_func_t real_malloc_size)
{
  wait_for_rainy_day_fund();

  return real_pvalloc(size);
}

void MallocStatisticImpl::wait_for_rainy_day_fund() {
  Locker locker(_rainy_day_fund_lock);
}

static bool is_same_stack(StatEntry* to_check, int nr_of_frames, address* frames) {
  for (int i = 0; i < nr_of_frames; ++i) {
    if (to_check->frames()[i] != frames[i]) {
      return false;
    }
  }

  return true;
}

static size_t hash_for_frames(int nr_of_frames, address* frames) {
  size_t result = 0;

  for (int i = 0; i < nr_of_frames; ++i) {
    intptr_t frame_addr = (intptr_t) frames[i];
    result = result * 31 + ((frame_addr & 0xfffffff0) >> 4) LP64_ONLY(+ 127 * (frame_addr >> 36));
  }

  // Avoid more bits than we can store in the entry.
  return (MAX_FRAMES + 1) * result/ (MAX_FRAMES + 1);
}

StatEntry*  MallocStatisticImpl::record_allocation_size(size_t to_add, int nr_of_frames, address* frames) {
  // Skip the top frame since it is always from the hooks.
  nr_of_frames = MAX2(nr_of_frames - FRAMES_TO_SKIP, 0);
  frames += FRAMES_TO_SKIP;

  assert(nr_of_frames <= _max_frames, "Overflow");

  size_t hash = hash_for_frames(nr_of_frames, frames);
  int idx = hash & (NR_OF_STACK_MAPS - 1);
  assert((idx >= 0) && (idx < NR_OF_STACK_MAPS), "invalid map index");

  Locker locker(_stack_maps_lock[idx]);

  if (!_enabled) {
    return NULL;
  }

  int slot = (hash / NR_OF_STACK_MAPS) & _stack_maps_mask[idx];
  assert((slot >= 0) || (slot <= _stack_maps_mask[idx]), "Invalid slot");
  StatEntry* to_check = _stack_maps[idx][slot];

  // Check if we already know this stack.
  while (to_check != NULL) {
    if ((to_check->hash() == hash) && (to_check->nr_of_frames() == nr_of_frames)) {
      if (is_same_stack(to_check, nr_of_frames, frames)) {
        to_check->add_allocation(to_add);

        return to_check;
      }
    }

    to_check = to_check->next();
  }

  // Need a new entry. Fail silently if we don't get the memory.
  void* mem = _stack_maps_alloc[idx]->allocate();

  if (mem != NULL) {
    StatEntry* entry = new (mem) StatEntry(hash, to_add, nr_of_frames, frames);
    assert(hash == entry->hash(), "Must be the same");
    assert(nr_of_frames == entry->nr_of_frames(), "Must be equal");
    entry->set_next(_stack_maps[idx][slot]);
    _stack_maps[idx][slot] = entry;
    _stack_maps_size[idx]._val += 1;

    if (_stack_maps_size[idx]._val > _stack_maps_limit[idx]) {
      resize_stack_map(idx, _stack_maps_mask[idx] * 2 + 1);
    }

    return entry;
  }

  return NULL;
}

void MallocStatisticImpl::record_allocation(void* ptr, uint64_t hash, int nr_of_frames, address* frames) {
  assert(_track_free, "Only used for detailed tracking");
  size_t size = _funcs->malloc_size(ptr);

  StatEntry* stat_entry = record_allocation_size(size, nr_of_frames, frames);

  if (stat_entry == NULL) {
    return;
  }

  int idx = (int) (hash & (NR_OF_ALLOC_MAPS - 1));
  Locker locker(_alloc_maps_lock[idx]);

  if (!_enabled) {
    return;
  }

  int slot = (hash / NR_OF_ALLOC_MAPS) & _alloc_maps_mask[idx];

  // Should not already be in the table, so we remove the check in the optimized version
#ifdef ASSERT
  AllocEntry* entry = _alloc_maps[idx][slot];

  while (entry != NULL) {
    if (entry->hash() == hash) {
      char tmp[1024];
      pthread_setspecific(_malloc_suspended, (void*) 1);
      shutdown();

      address caller_address = (address) NULL;
      address real_func = (address) NULL;
      CAPTURE_STACK();

      fdStream ss(1);
      ss.print_cr("Same hash " UINT64_FORMAT " for %p and %p", (uint64_t) hash, ptr, entry->ptr());
      ss.print_raw_cr("Current stack:");

      for (int i = 0; i < nr_of_frames; ++i) {
        ss.print("  [" PTR_FORMAT "]  ", p2i(frames[i]));
        os::print_function_and_library_name(&ss, frames[i], tmp, sizeof(tmp), true, true, false);
        ss.cr();
      }

      ss.print_raw_cr("Orig stack:");
      StatEntry* stat_entry = entry->entry();

      for (int i = 0; i < stat_entry->nr_of_frames(); ++i) {
        address frame = stat_entry->frames()[i];
        ss.print("  [" PTR_FORMAT "]  ", p2i(frame));

        if (os::print_function_and_library_name(&ss, frame, tmp, sizeof(tmp), true, true, false)) {
          ss.cr();
        } else {
          CodeBlob* blob = CodeCache::find_blob((void*) frame);

          if (blob != NULL) {
            ss.print_raw(" ");
            blob->print_value_on(&ss);
            ss.cr();
          } else {
            ss.print_raw_cr(" <unknown code>");
          }
        }
      }
    }

    assert((entry->hash() != hash) || (ptr == entry->ptr()), "Same hash for different pointer");
    assert(entry->hash() != hash, "Must not be already present");
    entry = entry->next();
  }
#endif

  void* mem = _alloc_maps_alloc[idx]->allocate();

  if (mem != NULL) {
#if defined(ASSERT)
    AllocEntry* entry = new (mem) AllocEntry(hash, stat_entry, _alloc_maps[idx][slot], ptr);
#else
    AllocEntry* entry = new (mem) AllocEntry(hash, stat_entry, _alloc_maps[idx][slot]);
#endif
    _alloc_maps[idx][slot] = entry;
    _alloc_maps_size[idx]._val += 1;

    if (_alloc_maps_size[idx]._val > _alloc_maps_limit[idx]) {
      resize_alloc_map(idx, _alloc_maps_mask[idx] * 2 + 1);
    }
  }
}

StatEntry* MallocStatisticImpl::record_free(void* ptr, uint64_t hash, size_t size) {
  assert(_track_free, "Only used for detailed tracking");

  int idx = (int) (hash & (NR_OF_ALLOC_MAPS - 1));
  Locker locker(_alloc_maps_lock[idx]);

  if (!_enabled) {
    return NULL;
  }

  int slot = (hash / NR_OF_ALLOC_MAPS) & _alloc_maps_mask[idx];
  AllocEntry** entry = &_alloc_maps[idx][slot];

  while (*entry != NULL) {
    if ((*entry)->hash() == hash) {
      StatEntry* stat_entry = (*entry)->entry();
      assert((*entry)->ptr() == ptr, "Same hash must be same pointer");
      AllocEntry* next = (*entry)->next();
      _alloc_maps_alloc[idx]->free(*entry);
      _alloc_maps_size[idx]._val -= 1;
      *entry = next;

      // Should not be in the table anymore.
#ifdef ASSERT
      AllocEntry* to_check = _alloc_maps[idx][slot];

      while (to_check != NULL) {
        assert(to_check->hash() != hash, "Must not be already present");
        to_check = to_check->next();
      }
#endif

      // We need to lock the stat table containing the entry to avoid
      // races when changing the size and count fields.
      int idx2 = (int) (stat_entry->hash() & (NR_OF_STACK_MAPS - 1));
      Locker locker2(_stack_maps_lock[idx2]);
      stat_entry->remove_allocation(size);

      return stat_entry;
    }

    entry = (*entry)->next_ptr();
  }

  // We missed an allocation. This is fine, since we might have enabled the
  // trace after the allocation itself (or it might be a bug in the progam,
  // but we can't be sure).
  if (_detailed_stats) {
    Atomic::add(&_failed_frees, (uint64_t) 1);
  }

  return NULL;
}

void MallocStatisticImpl::cleanup_for_stack_map(int idx) {
  Locker locker(_stack_maps_lock[idx]);

  if (_stack_maps_alloc[idx] != NULL) {
    _stack_maps_alloc[idx]->~Allocator();
    _funcs->free(_stack_maps_alloc[idx]);
    _stack_maps_alloc[idx] = NULL;
  }

  if (_stack_maps[idx] != NULL) {
    _funcs->free(_stack_maps[idx]);
    _stack_maps[idx] = NULL;
  }
}

void MallocStatisticImpl::cleanup_for_alloc_map(int idx) {
  Locker locker(_alloc_maps_lock[idx]);

  if (_alloc_maps_alloc[idx] != NULL) {
    _alloc_maps_alloc[idx]->~Allocator();
    _funcs->free(_alloc_maps_alloc[idx]);
    _alloc_maps_alloc[idx] = NULL;
  }

  if (_alloc_maps[idx] != NULL) {
    _funcs->free(_alloc_maps[idx]);
    _alloc_maps[idx] = NULL;
  }
}

void MallocStatisticImpl::cleanup() {
  for (int i = 0; i < NR_OF_STACK_MAPS; ++i) {
    cleanup_for_stack_map(i);
  }

  for (int i = 0; i < NR_OF_ALLOC_MAPS; ++i) {
    cleanup_for_alloc_map(i);
  }

  if (_funcs != NULL) {
    _funcs->free(_rainy_day_fund);
    _rainy_day_fund = NULL;
  }

}

void MallocStatisticImpl::resize_stack_map(int map, int new_mask) {
  StatEntry** new_map = (StatEntry**) _funcs->calloc(new_mask + 1, sizeof(StatEntry*));
  StatEntry** old_map = _stack_maps[map];

  // Fail silently if we don't get the memory.
  if (new_map != NULL) {
    for (int i = 0; i <= _stack_maps_mask[map]; ++i) {
      StatEntry* entry = old_map[i];

      while (entry != NULL) {
        StatEntry* next_entry = entry->next();
        int slot = entry->hash() & new_mask;
        entry->set_next(new_map[slot]);
        new_map[slot] = entry;
        entry = next_entry;
      }
    }

    _stack_maps[map] = new_map;
    _stack_maps_mask[map] = new_mask;
    _stack_maps_limit[map] = (int) ((_stack_maps_mask[map] + 1) * MAX_STACK_MAP_LOAD);
    _funcs->free(old_map);
  }
}

void MallocStatisticImpl::resize_alloc_map(int map, int new_mask) {
  AllocEntry** new_map = (AllocEntry**) _funcs->calloc(new_mask + 1, sizeof(StatEntry*));
  AllocEntry** old_map = _alloc_maps[map];

  // Fail silently if we don't get the memory.
  if (new_map != NULL) {
    for (int i = 0; i <= _alloc_maps_mask[map]; ++i) {
      AllocEntry* entry = old_map[i];

      while (entry != NULL) {
        AllocEntry* next_entry = entry->next();
        int slot = (entry->hash() / NR_OF_ALLOC_MAPS) & new_mask;
        entry->set_next(new_map[slot]);
        new_map[slot] = entry;
        entry = next_entry;
      }
    }

    _alloc_maps[map] = new_map;
    _alloc_maps_mask[map] = new_mask;
    _alloc_maps_limit[map] = (int) ((_alloc_maps_mask[map] + 1) * MAX_ALLOC_MAP_LOAD);
    _funcs->free(old_map);
  }
}

bool MallocStatisticImpl::rainy_day_fund_used() {
  return _rainy_day_hooks_ptr == NULL;
}

void MallocStatisticImpl::initialize() {
  if (_initialized) {
    return;
  }

  _initialized = true;

  if (pthread_mutex_init(&_malloc_stat_lock._lock, NULL) != 0) {
    fatal("Could not initialize lock 1");
  }

  pthread_mutexattr_t attr;

  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

  if (pthread_mutex_init(&_rainy_day_fund_lock._lock, &attr) != 0) {
    fatal("Could not initialize lock 2");
  }

  if (pthread_key_create(&_malloc_suspended, NULL) != 0) {
    fatal("Could not initialize key 1");
  }

  if (pthread_key_create(&_malloc_rd_enabled, NULL) != 0) {
    fatal("Could not initialize key 2");
  }

  for (int i = 0; i < NR_OF_STACK_MAPS; ++i) {
    if (pthread_mutex_init(&_stack_maps_lock[i]._lock, NULL) != 0) {
      fatal("Could not initialize lock");
    }
  }

  for (int i = 0; i < NR_OF_ALLOC_MAPS; ++i) {
    if (pthread_mutex_init(&_alloc_maps_lock[i]._lock, NULL) != 0) {
      fatal("Could not initialize lock");
    }
  }
}

bool MallocStatisticImpl::enable(outputStream* st, TraceSpec const& spec) {
  initialize();
  Locker lock(_malloc_stat_lock);

  if (_enabled) {
    if (spec._force) {
      _enabled = false;
      setup_hooks(NULL, st);
      cleanup();

      st->print_raw_cr("Disabling already running trace first.");
    } else {
      st->print_raw_cr("Malloc statistic is already enabled!");

      return false;
    }
  }

  if (_shutdown) {
    st->print_raw_cr("Malloc statistic is already shut down!");

    return false;
  }

  if (spec._stack_depth < 2 || spec._stack_depth > MAX_FRAMES) {
    st->print_cr("The given stack depth %d is outside of the valid range [%d, %d]",
                 spec._stack_depth, 2, MAX_FRAMES);

    return false;
  }

  // Get the backtrace function if needed.
  if (spec._use_backtrace && !_tried_to_load_backtrace) {
    _tried_to_load_backtrace = true;

#if defined(__APPLE__)
    // Try libunwind first on mac.
    _backtrace = (backtrace_func_t*) dlsym(RTLD_DEFAULT, "unw_backtrace");
    _backtrace_name = "backtrace (libunwind)";

    if (_backtrace == NULL) {
      _backtrace = (backtrace_func_t*) dlsym(RTLD_DEFAULT, "backtrace");
      _backtrace_name = "backtrace";
    }
#else
    _backtrace = (backtrace_func_t*) dlsym(RTLD_DEFAULT, "backtrace");
    _backtrace_name = "backtrace";

    if (_backtrace == NULL) {
      // Try if we have libunwind installed.
      char ebuf[512];
      void* libunwind = os::dll_load(MallocTraceUnwindLibName, ebuf, sizeof ebuf);

      if (libunwind != NULL) {
        _backtrace = (backtrace_func_t*) dlsym(libunwind, "unw_backtrace");
        _backtrace_name = "backtrace (libunwind)";
      }
    }
#endif

    // Clear dlerror.
    dlerror();

    if (_backtrace != NULL) {
      // Trigger initialization needed.
      void* tmp[1];
      _backtrace(tmp, 1);
    }
  }

  _track_free = spec._track_free;
  _detailed_stats = spec._detailed_stats;

  if (_track_free) {
    st->print_raw_cr("Tracking live memory.");
  } else {
    st->print_raw_cr("Tracking all allocated memory.");
  }

  if (_detailed_stats) {
    st->print_raw_cr("Collecting detailed statistics.");
  }

  int only_nth = MIN2(1000, MAX2(1, spec._only_nth));

  if (only_nth > 1) {
    for (int i = 1; i < 63; ++i) {
      uint64_t pow = ((uint64_t) 1) << i;
      _to_track_limit = pow / only_nth;
      double diff = pow / (double) _to_track_limit - only_nth;

      _to_track_mask = pow - 1;

      if (diff > -0.1 && diff < 0.1) {
        break;
      }
    }

    st->print_cr("Tracking about every %d allocations (%d / %d).", only_nth, (int) _to_track_mask, (int) _to_track_limit);
  } else {
    _to_track_mask = 0;
    _to_track_limit = 1;
  }

  _use_backtrace = spec._use_backtrace && (_backtrace != NULL);

  // Reset statistic counters.
  _stack_walk_time  = 0;
  _stack_walk_count = 0;
  _tracked_ptrs     = 0;
  _not_tracked_ptrs = 0;
  _failed_frees     = 0;

  if (_use_backtrace && spec._use_backtrace) {
    st->print_raw_cr("Using backtrace() to sample stacks.");
  } else if (spec._use_backtrace) {
    st->print_raw_cr("Using fallback mechanism to sample stacks, since backtrace() was not available.");
  } else {
    st->print_raw_cr("Using fallback mechanism to sample stacks.");
  }

  _max_frames = spec._stack_depth;
  real_funcs_t* result = setup_hooks(&_malloc_stat_hooks, st);

  if (result == NULL) {
    return false;
  }

  // Never set _funcs to NULL, even if we fail. It's just safer that way.
  _funcs = result;
  _entry_size = sizeof(StatEntry) + sizeof(address) * (_max_frames - 1);

  if (spec._rainy_day_fund > 0) {
    _rainy_day_fund = _funcs->malloc(spec._rainy_day_fund);

    if (_rainy_day_fund == NULL) {
      st->print_cr("Could not allocate rainy day fund of %d bytes", spec._rainy_day_fund);
      cleanup();

      return false;
    }
  }

  for (int i = 0; i < NR_OF_STACK_MAPS; ++i) {
    void* mem = _funcs->malloc(sizeof(Allocator));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the allocator!");
      cleanup();

      return false;
    }

    size_t entry_size = sizeof(StatEntry) + sizeof(address) * (_max_frames - 1);
    _stack_maps_alloc[i] = new (mem) Allocator(entry_size, 256, _funcs);
    _stack_maps_mask[i] = STACK_MAP_INIT_SIZE - 1;
    _stack_maps_size[i]._val = 0;
    _stack_maps_limit[i] = (int) ((_stack_maps_mask[i] + 1) * MAX_STACK_MAP_LOAD);
    _stack_maps[i] = (StatEntry**) _funcs->calloc(_stack_maps_mask[i] + 1, sizeof(StatEntry*));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the stack map!");
      cleanup();

      return false;
    }
  }

  for (int i = 0; i < NR_OF_ALLOC_MAPS; ++i) {
    void* mem = _funcs->malloc(sizeof(Allocator));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the allocator!");
      cleanup();

      return false;
    }

    _alloc_maps_alloc[i] = new (mem) Allocator(sizeof(AllocEntry), 2048, _funcs);
    _alloc_maps_mask[i] = ALLOC_MAP_INIT_SIZE - 1;
    _alloc_maps_size[i]._val = 0;
    _alloc_maps_limit[i] = (int) ((_alloc_maps_mask[i] + 1) * MAX_ALLOC_MAP_LOAD);
    _alloc_maps[i] = (AllocEntry**) _funcs->calloc(_alloc_maps_mask[i] + 1, sizeof(AllocEntry*));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the alloc map!");
      cleanup();

      return false;
    }
  }

  if (_funcs == NULL) {
    return false;
  }

  _enabled = true;
  return true;
}

bool MallocStatisticImpl::disable(outputStream* st) {
  initialize();
  Locker lock(_malloc_stat_lock);

  if (!_enabled) {
    if (st != NULL) {
      st->print_raw_cr("Malloc statistic is already disabled!");
    }

    return false;
  }

  _enabled = false;
  setup_hooks(NULL, st);
  cleanup();
  _funcs = NULL;

  return true;
}


static char const* mem_prefix[] = {"k", "M", "G", "T", NULL};

static void print_percentage(outputStream* st, double f) {
  if (f <= 0) {
    st->print("0.00 %%");
  } else if (f < 0.01) {
    st->print("< 0.01 %%");
  } else if (f < 10) {
    st->print("%.2f %%", f);
  } else {
    st->print("%.1f %%", f);
  }
}

static void print_mem(outputStream* st, uint64_t mem, uint64_t total = 0) {
  uint64_t k = 1024;
  double perc = 0.0;
  if (total > 0) {
    perc = 100.0 * mem / total;
  }

  if ((int64_t) mem < 0) {
    mem = -((int64_t) mem);
    st->print("*neg* ");
  }

  if (mem < 1000) {
    if (total > 0) {
      st->print("%'" PRId64 " (", (uint64_t) mem);
      print_percentage(st, perc);
      st->print_raw(")");
    } else {
      st->print("%'" PRId64, (uint64_t) mem);
    }
  } else {
    int idx =0;
    uint64_t curr = mem;
    double f = 1.0 / k;

    while (mem_prefix[idx] != NULL) {
      if (curr < 1000 * k) {
        if (curr < 100 * k) {
          if (total > 0) {
            st->print("%'" PRId64 " (%.1f %s, ", mem, f * curr, mem_prefix[idx]);
            print_percentage(st, perc);
            st->print_raw(")");
          } else {
            st->print("%'" PRId64 " (%.1f %s)", mem, f * curr, mem_prefix[idx]);
          }
        } else {
          if (total > 0) {
            st->print("%'" PRId64 " (%d %s, ", mem, (int) (curr / k), mem_prefix[idx]);
            print_percentage(st, perc);
            st->print_raw(")");
          } else {
            st->print("%'" PRId64 " (%d %s)", mem, (int) (curr / k), mem_prefix[idx]);
          }
        }

        return;
      }

      curr /= k;
      idx += 1;
    }

    st->print("%'" PRId64 " (%'" PRId64 "%s)", mem, curr, mem_prefix[idx - 1]);
  }
}

static void print_count(outputStream* st, uint64_t count, uint64_t total = 0) {
  st->print("%'" PRId64, (int64_t) count);

  if (total > 0) {
    double perc = 100.0 * count / total;

    st->print_raw(" (");
    print_percentage(st, perc);
    st->print_raw(")");
  }
}

static void print_frame(outputStream* st, address frame) {
  char tmp[256];

  if (os::print_function_and_library_name(st, frame, tmp, sizeof(tmp), true, true, false)) {
    st->cr();
  } else {
    CodeBlob* blob = CodeCache::find_blob((void*) frame);

    if (blob != NULL) {
      st->print_raw(" ");
      blob->print_value_on(st);
    } else {
      st->print_raw_cr(" <unknown code>");
    }
  }
}

bool MallocStatisticImpl::dump_entry(outputStream* st, StatEntryCopy* entry, int index,
                                     uint64_t total_size, uint64_t total_count, int total_entries,
                                     char const* filter, AddressHashSet* filter_cache) {
  // Use a temp buffer since the output stream might use unbuffered I/O.
  char ss_tmp[4096];
  stringStream ss(ss_tmp, sizeof(ss_tmp));

  // Check if we should print this stack.
  if (is_non_empty_string(filter)) {
    bool found = false;

    for (int i = 0; i < entry->_entry->nr_of_frames(); ++i) {
      address frame = entry->_entry->frames()[i];

      if (filter_cache->contains(frame)) {
        continue;
      }

      print_frame(&ss, frame);

      if (strstr(ss.base(), filter) != NULL) {
        found = true;
        ss.reset();

        break;
      } else {
        filter_cache->add(frame);
      }

      ss.reset();
    }

    if (!found) {
      return false;
    }
  }

  // We use int64_t here to easy see if values got negative (instead of seeing
  // an insanely large number).
  ss.print("Stack %d of %d: ", index, total_entries);
  print_mem(&ss, entry->_size, total_size);
  ss.print_raw(" bytes, ");
  print_count(&ss, entry->_count, total_count);
  ss.print_cr(" allocations");

  for (int i = 0; i < entry->_entry->nr_of_frames(); ++i) {
    address frame = entry->_entry->frames()[i];
    ss.print("  [" PTR_FORMAT "]  ", p2i(frame));
    print_frame(&ss, frame);

    // Flush the temp buffer if we are near the end.
    if (sizeof(ss_tmp) - ss.size() < 512) {
      st->write(ss_tmp, ss.size());
      ss.reset();
    }
  }

  if (entry->_entry->nr_of_frames() == 0) {
    ss.print_raw_cr("  <no stack>");
  }

  st->write(ss_tmp, ss.size());

  return true;
}

static void print_allocation_stats(outputStream* st, Allocator** allocs, int* masks, Padded<int>* sizes,
                                   Lock* locks, int nr_of_maps, char const* type) {
  uint64_t allocated = 0;
  uint64_t unused = 0;
  uint64_t total_entries = 0;
  uint64_t total_slots = 0;

  for (int i = 0; i < nr_of_maps; ++i) {
    Locker lock(locks[i]);
    allocated     += (masks[i] + 1) * sizeof(void*);
    total_entries += sizes[i]._val;
    total_slots   += masks[i] + 1;
    allocated     += allocs[i]->allocated();
    unused        += allocs[i]->unused();
  }

  st->cr();
  st->print_cr("Statistic for %s:", type);
  st->print_raw("Allocated memory: ");
  print_mem(st, allocated);
  st->cr();
  st->print_raw("Unused memory   : ");
  print_mem(st, unused);
  st->cr();
  st->print_cr("Average load    : %.2f", total_entries / (double) total_slots);
  st->print_cr("Nr. of entries  : %'" PRId64, total_entries);
}

static int sort_by_size(const void* p1, const void* p2) {
  StatEntryCopy* e1 = (StatEntryCopy*) p1;
  StatEntryCopy* e2 = (StatEntryCopy*) p2;

  if (e1->_size > e2->_size) {
    return -1;
  }

  if (e1->_size < e2->_size) {
    return 1;
  }

  // For consistent sorting.
  if (e1->_entry < e2->_entry) {
    return -1;
  }

  return 1;
}

static int sort_by_count(const void* p1, const void* p2) {
  StatEntryCopy* e1 = (StatEntryCopy*) p1;
  StatEntryCopy* e2 = (StatEntryCopy*) p2;

  if (e1->_count > e2->_count) {
    return -1;
  }

  if (e1->_count < e2->_count) {
    return 1;
  }

  // For consistent sorting.
  if (e1->_entry < e2->_entry) {
    return -1;
  }

  return 1;
}

bool MallocStatisticImpl::dump(outputStream* msg_stream, outputStream* dump_stream, DumpSpec const& spec) {
  bool used_rainy_day_fund = false;

  if (!spec._on_error) {
    initialize();
  } else if (_initialized) {
    // Make sure all other threads don't allocate memory anymore
    if (Atomic::cmpxchg(&_rainy_day_hooks_ptr, &_rainy_day_hooks, (registered_hooks_t*) NULL) != NULL) {
      used_rainy_day_fund = true;
    } else {
      return false;
    }
  } else {
    return false;
  }

  Locker locker(_rainy_day_fund_lock, !used_rainy_day_fund);

  if (used_rainy_day_fund) {
    setup_hooks(&_rainy_day_hooks, NULL);

    // Free rainy day fund so we have some memory to use.
    _funcs->free(_rainy_day_fund);
    _rainy_day_fund = NULL;
    msg_stream->print_raw_cr("Emergency dump of malloc trace statistic ...");
  }

  // Hide allocations done by this thread during dumping if requested.
  // Note that we always track  frees or we might end up trying to add
  // an allocation with a pointer which is already in the aloc maps.
  pthread_setspecific(_malloc_suspended, spec._hide_dump_allocs ? (void*) 1 : NULL);

  // We need to avoid having the trace disabled concurrently.
  Locker lock(_malloc_stat_lock, spec._on_error);

  if (!_enabled) {
    msg_stream->print_raw_cr("Malloc statistic not enabled!");
    pthread_setspecific(_malloc_suspended, NULL);

    return false;
  }

  if (_backtrace != NULL) {
    dump_stream->print_cr("Stacks were collected via %s.", _backtrace_name);
  } else {
    dump_stream->print_cr("Stacks were collected via the fallback mechanism.");
  }

  if (_track_free) {
    dump_stream->print_raw_cr("Contains the currently allocated memory since enabling.");
  } else {
    dump_stream->print_raw_cr("Contains every allocation done since enabling.");
  }

  bool uses_filter = is_non_empty_string(spec._filter);

  if (uses_filter) {
    dump_stream->print_cr("Only printing stacks in which frames contain '%s'.", spec._filter);
  }

  // We make a copy of each hash map, since we don't want to lock for the whole operation.
  StatEntryCopy* entries[NR_OF_STACK_MAPS];
  int nr_of_entries[NR_OF_STACK_MAPS];

  bool failed_alloc = false;
  uint64_t total_count = 0;
  uint64_t total_size = 0;
  int total_entries = 0;
  int total_non_empty_entries = 0;
  int max_entries = MAX2(1, spec._dump_fraction > 0 ? INT_MAX : spec._max_entries);
  int max_printed_entries = max_entries;

  if (uses_filter) {
    max_entries = INT_MAX;
  }

  elapsedTimer totalTime;
  elapsedTimer lockedTime;

  totalTime.start();

  for (int idx = 0; idx < NR_OF_STACK_MAPS; ++idx) {
    lockedTime.start();

    {
      Locker locker(_stack_maps_lock[idx]);

      int expected_size = _stack_maps_size[idx]._val;
      int pos = 0;

      entries[idx] = (StatEntryCopy*) _funcs->malloc(sizeof(StatEntryCopy) * expected_size);

      if (entries[idx] != NULL) {
        StatEntry** map = _stack_maps[idx];
        StatEntryCopy* copies = entries[idx];
        int nr_of_slots = _stack_maps_mask[idx] + 1;

        for (int slot = 0; slot < nr_of_slots; ++slot) {
          StatEntry* entry = map[slot];

          while (entry != NULL) {
            assert(pos < expected_size, "To many entries");

            if (entry->count() > 0) {
              copies[pos]._entry = entry;
              copies[pos]._size  = entry->size();
              copies[pos]._count = entry->count();

              total_size += entry->size();
              total_count += entry->count();

              pos += 1;
            }

            entry = entry->next();
          }
        }

        assert(pos <= expected_size, "Size must be correct");
      } else {
        failed_alloc = true;
      }

      lockedTime.stop();

      // See if it makes sense to trim. We have to shave of enough and don't
      // trim anyway after sorting.
      if ((entries[idx] != NULL) && (pos < expected_size - 16) && (pos < max_entries)) {
        void* result = _funcs->realloc(entries[idx], pos * sizeof(StatEntryCopy));

        if (result != NULL) {
          entries[idx] = (StatEntryCopy*) result;
        }
      }

      nr_of_entries[idx] = pos;
      total_entries += expected_size;
      total_non_empty_entries += pos;
    }

    if (entries[idx] != NULL) {
      // Now sort so we might be able to trim the array to only contain the
      // maximum possible entries.
      if (spec._sort_by_count) {
          qsort(entries[idx], nr_of_entries[idx], sizeof(StatEntryCopy), sort_by_count);
      } else {
          qsort(entries[idx], nr_of_entries[idx], sizeof(StatEntryCopy), sort_by_size);
      }

      // Free up some memory if possible.
      if (nr_of_entries[idx] > max_entries) {
        void* result = _funcs->realloc(entries[idx], max_entries * sizeof(StatEntryCopy));

        if (result == NULL)  {
          // No problem, since the original memory is still there. Should not happen
          // in reality.
        } else {
          entries[idx] = (StatEntryCopy*) result;
        }

        nr_of_entries[idx] = max_entries;
      }
    } else {
      nr_of_entries[idx] = 0;
      failed_alloc = true;
    }
  }

  uint64_t size_limit = total_size;
  uint64_t count_limit = total_count;

  if (spec._dump_fraction > 0) {
    if (spec._sort_by_count) {
      count_limit = (uint64_t) (0.01 * total_count *  spec._dump_fraction);
    } else {
      size_limit = (uint64_t) (0.01 * total_size *  spec._dump_fraction);
    }
  }

  AddressHashSet filter_cache(_funcs, !spec._on_error);

  int curr_pos[NR_OF_STACK_MAPS];
  memset(curr_pos, 0, NR_OF_STACK_MAPS * sizeof(int));

  uint64_t printed_size = 0;
  uint64_t printed_count = 0;
  int printed_entries = 0;

  for (int i = 0; i < max_entries; ++i) {
    int max_pos = -1;
    StatEntryCopy* max = NULL;

    // Find the largest entry not currently printed.
    if (spec._sort_by_count) {
      for (int j = 0; j < NR_OF_STACK_MAPS; ++j) {
        if (curr_pos[j] < nr_of_entries[j]) {
          if ((max == NULL) || (max->_count < entries[j][curr_pos[j]]._count)) {
            max = &entries[j][curr_pos[j]];
            max_pos = j;
          }
        }
      }
    } else {
      for (int j = 0; j < NR_OF_STACK_MAPS; ++j) {
        if (curr_pos[j] < nr_of_entries[j]) {
          if ((max == NULL) || (max->_size < entries[j][curr_pos[j]]._size)) {
            max = &entries[j][curr_pos[j]];
            max_pos = j;
          }
        }
      }
    }

    if (max == NULL) {
      // Done everything we can.
      break;
    }

    curr_pos[max_pos] += 1;

    if (dump_entry(dump_stream, max, i + 1, total_size, total_count,
                   total_non_empty_entries, spec._filter, &filter_cache)) {
      printed_size += max->_size;
      printed_count += max->_count;
      printed_entries += 1;

      if (printed_entries >= max_printed_entries) {
        break;
      }
    }

    if (printed_size > size_limit) {
      break;
    }

    if (printed_count > count_limit) {
      break;
    }
  }

  for (int i = 0; i < NR_OF_STACK_MAPS; ++i) {
    _funcs->free(entries[i]);
  }

  dump_stream->cr();
  dump_stream->print_cr("Printed %'d stacks", printed_entries);

  if (_track_free) {
    dump_stream->print_cr("Total unique stacks: %'d (%'d including stacks with no alive allocations)",
                          total_non_empty_entries, total_entries);
  } else {
    dump_stream->print_cr("Total unique stacks: %'d", total_non_empty_entries);
  }

  dump_stream->print_raw("Total allocated bytes: ");
  print_mem(dump_stream, total_size);
  dump_stream->cr();
  dump_stream->print_raw("Total allocation count: ");
  print_count(dump_stream, total_count);
  dump_stream->cr();
  dump_stream->print_raw("Total printed bytes: ");
  print_mem(dump_stream, printed_size, total_size);
  dump_stream->cr();
  dump_stream->print_raw("Total printed count: ");
  print_count(dump_stream, printed_count, total_count);
  dump_stream->cr();

  totalTime.stop();

  if (failed_alloc) {
    dump_stream->print_cr("Failed to alloc memory during dump, so it might be incomplete!");
  }

  if (spec._internal_stats && _detailed_stats) {
    uint64_t per_stack = _stack_walk_time / MAX2(_stack_walk_count, (uint64_t) 1);
    msg_stream->cr();
    msg_stream->print_cr("Sampled %'" PRId64 " stacks, took %'" PRId64 " ns per stack on average.",
                         _stack_walk_count, per_stack);
    msg_stream->print_cr("Sampling took %.2f seconds in total", _stack_walk_time * 1e-9);
    msg_stream->print_cr("Tracked allocations  : %'" PRId64, _tracked_ptrs);
    msg_stream->print_cr("Untracked allocations: %'" PRId64, _not_tracked_ptrs);
    msg_stream->print_cr("Untracked frees      : %'" PRId64, _failed_frees);

    if ((_to_track_mask > 0) && (_tracked_ptrs > 0)) {
      double frac = 100.0 * _tracked_ptrs / (_tracked_ptrs + _not_tracked_ptrs);
      double rate = 100.0 / frac;
      int target = (int) (0.5 + (_to_track_mask + 1) / (double) _to_track_limit);
      msg_stream->print_cr("%.2f %% of the allocations were tracked, about every %.2f allocations " \
                           "(target %d)", frac, rate, target);
    }
  }

  if (spec._internal_stats) {
    print_allocation_stats(msg_stream, _stack_maps_alloc, _stack_maps_mask, _stack_maps_size,
                           _stack_maps_lock, NR_OF_STACK_MAPS, "stack maps");

    if (_track_free) {
      print_allocation_stats(msg_stream, _alloc_maps_alloc, _alloc_maps_mask, _alloc_maps_size,
                             _alloc_maps_lock, NR_OF_ALLOC_MAPS, "alloc maps");
    }

    if (uses_filter) {
      msg_stream->cr();
      msg_stream->print_raw_cr("Statistic for filter cache:");
      msg_stream->print("Allocated memory: ");
      print_mem(dump_stream, filter_cache.allocated(), 0);
      msg_stream->cr();
      msg_stream->print_cr("Load factor     : %.3f",  filter_cache.load());
    }
  }

  msg_stream->cr();
  msg_stream->print_cr("Dumping done in %.3f s (%.3f s of that locked)",
                       totalTime.milliseconds() * 0.001,
                       lockedTime.milliseconds() * 0.001);

  pthread_setspecific(_malloc_suspended, NULL);

  if (used_rainy_day_fund) {
    setup_hooks(&_malloc_stat_hooks, NULL);
  }

  return true;
}

void MallocStatisticImpl::shutdown() {
  _shutdown = true;

  if (_initialized) {
    _enabled = false;

    if (register_hooks != NULL) {
      register_hooks(NULL);
    }
  }
}

static void dump_from_flags(bool on_error) {
  DumpSpec spec;
  char const* file = MallocTraceDumpOutput;
  spec._on_error = on_error;
  spec._filter = MallocTraceDumpFilter;
  spec._sort_by_count = MallocTraceDumpSortByCount;
  spec._max_entries = MallocTraceDumpMaxEntries;
  spec._dump_fraction = MallocTraceDumpFraction;
  spec._hide_dump_allocs = MallocTraceDumpHideDumpAlllocs;
  spec._internal_stats = MallocTraceDumpInternalStats;

  if (is_non_empty_string(file)) {
    if (strcmp("stdout", file) == 0) {
      fdStream fds(1);
      mallocStatImpl::MallocStatisticImpl::dump(&fds, &fds, spec);
    } else if (strcmp("stderr", file) == 0) {
      fdStream fds(2);
      mallocStatImpl::MallocStatisticImpl::dump(&fds, &fds, spec);
    } else {
      char const* pid_tag = strstr(file, "@pid");

      if (pid_tag != NULL) {
        size_t len = strlen(file);
        size_t first = pid_tag - file;
        char buf[32768];
        jio_snprintf(buf, sizeof(buf), "%.*s" UINTX_FORMAT "%s",
                    first, file, os::current_process_id(), pid_tag + 4);
        fileStream fs(buf, "at");
        mallocStatImpl::MallocStatisticImpl::dump(&fs, &fs, spec);
      } else {
        fileStream fs(file, "at");
        mallocStatImpl::MallocStatisticImpl::dump(&fs, &fs, spec);
      }
    }
  } else {
    stringStream ss;
    mallocStatImpl::MallocStatisticImpl::dump(&ss, &ss, spec);
  }
}

class MallocTraceDumpPeriodicTask : public PeriodicTask {
private:
  int _left;

public:
  MallocTraceDumpPeriodicTask(uint64_t delay) :
    PeriodicTask(1000 * delay),
    _left(MallocTraceDumpCount - 1) {
  }

  virtual void task();
};

void MallocTraceDumpPeriodicTask::task() {
  dump_from_flags(false);
  --_left;

  if (_left == 0) {
    disenroll();
  }
}

class MallocTraceDumpInitialTask : public PeriodicTask {

public:
  MallocTraceDumpInitialTask(uint64_t delay) :
    PeriodicTask(1000 * delay) {
  }

  virtual void task();
};

void MallocTraceDumpInitialTask::task() {
  dump_from_flags(false);

  if (MallocTraceDumpCount > 1) {
    uint64_t delay = MAX2((uint64_t) 1, parse_timespan(MallocTraceDumpInterval));

    mallocStatImpl::MallocTraceDumpPeriodicTask* task = new mallocStatImpl::MallocTraceDumpPeriodicTask(delay);
    task->enroll();
  }

  disenroll();
}

void enable_from_flags() {
  TraceSpec spec;
  stringStream ss;

  spec._stack_depth = (int) MallocTraceStackDepth;
  spec._use_backtrace = MallocTraceUseBacktrace;
  spec._only_nth = (int) MallocTraceOnlyNth;
  spec._track_free = MallocTraceTrackFree;
  spec._detailed_stats = MallocTraceDetailedStats;

  if (MallocTraceDumpOnError) {
    spec._rainy_day_fund = (int) MallocTraceRainyDayFund;
  }

  if (!MallocStatistic::enable(&ss, spec) && MallocTraceExitIfFail) {
    fprintf(stderr, "Could not enable malloc trace via -XX:+MallocTraceAtStartup: %s", ss.base());
    os::exit(1);
  }
}

static void enable_delayed_dump() {
  if (MallocTraceDumpCount > 0) {
    uint64_t delay = MAX2((uint64_t) 1, parse_timespan(MallocTraceDumpDelay));
    mallocStatImpl::MallocTraceDumpInitialTask* task = new mallocStatImpl::MallocTraceDumpInitialTask(delay);
    task->enroll();
  }
}

class MallocTraceEnablePeriodicTask : public PeriodicTask {

public:
  MallocTraceEnablePeriodicTask(uint64_t delay) :
    PeriodicTask(1000 * delay) {
  }

  virtual void task();
};

void MallocTraceEnablePeriodicTask::task() {
  enable_from_flags();
  enable_delayed_dump();
}

} // namespace mallocStatImpl

void MallocStatistic::initialize() {
  // Don't enable this if the other malloc trace is on.
#if defined(LINUX)
  if (EnableMallocTrace) {
    return;
  }
#endif

  // Remove the hooks from the preload env, so we don't
  // preload mallochooks for spawned programs.
  mallocStatImpl::remove_malloc_hooks_from_env();

  // We have to make sure the child process of a fork doesn't run with
  // enabled malloc hooks before forking.
  pthread_atfork(NULL, NULL, mallocStatImpl::after_child_fork);

  mallocStatImpl::MallocStatisticImpl::initialize();

  if (MallocTraceAtStartup) {
#define CHECK_TIMESPAN_ARG(argument) \
    char const* error_##argument; \
    parse_timespan(argument, &error_##argument); \
    if (error_##argument != NULL) { \
      fprintf(stderr, "Could not parse argument '%s' of -XX:" #argument ": %s\n", argument, error_##argument); \
      os::exit(1); \
    }

    // Check interval specs now, so we don't fail later.
    CHECK_TIMESPAN_ARG(MallocTraceEnableDelay);
    CHECK_TIMESPAN_ARG(MallocTraceDumpDelay);
    CHECK_TIMESPAN_ARG(MallocTraceDumpInterval);

    uint64_t delay = parse_timespan(MallocTraceEnableDelay);

    if (delay > 0) {
      mallocStatImpl::MallocTraceEnablePeriodicTask* task = new mallocStatImpl::MallocTraceEnablePeriodicTask(delay);
      task->enroll();
    } else {
      mallocStatImpl::enable_from_flags();
      mallocStatImpl::enable_delayed_dump();
    }
  }
}

bool MallocStatistic::enable(outputStream* st, TraceSpec const& spec) {
  return mallocStatImpl::MallocStatisticImpl::enable(st, spec);
}

bool MallocStatistic::disable(outputStream* st) {
  return mallocStatImpl::MallocStatisticImpl::disable(st);
}

bool MallocStatistic::dump(outputStream* st, DumpSpec const& spec) {
  const char* dump_file = spec._dump_file;

  if (is_non_empty_string(dump_file)) {
    int fd;

    if (strcmp("stderr", dump_file) == 0) {
      fd = 2;
    } else if (strcmp("stdout", dump_file) == 0) {
      fd = 1;
    } else {
      fd = ::open(dump_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

      if (fd < 0) {
        st->print_cr("Could not open '%s' for output.", dump_file);

        return false;
      }
    }

    fdStream dump_stream(fd);
    bool result = mallocStatImpl::MallocStatisticImpl::dump(st, &dump_stream, spec);

    if ((fd != 1) && (fd != 2)) {
      ::close(fd);
    }

    return fd;
  }

  return mallocStatImpl::MallocStatisticImpl::dump(st, st, spec);
}

void MallocStatistic::emergencyDump() {
  // Check enabled at all or already done.
  if (!MallocTraceDumpOnError || mallocStatImpl::MallocStatisticImpl::rainy_day_fund_used()) {
    return;
  }

  mallocStatImpl::dump_from_flags(true);
}

void MallocStatistic::shutdown() {
  mallocStatImpl::MallocStatisticImpl::shutdown();
}

MallocTraceEnableDCmd::MallocTraceEnableDCmd(outputStream* output, bool heap) :
  DCmdWithParser(output, heap),
  _stack_depth("-stack-depth", "The maximum stack depth to track", "INT", false, "12"),
  _use_backtrace("-use-backtrace", "If true we try to use the backtrace() method to sample " \
                 "the stack traces.", "BOOLEAN", false, "false"),
  _only_nth("-only-nth", "If > 1 we only track about every n'th allocation. Note that we round " \
            "the given number to the closest power of 2.", "INT", false, "1"),
  _force("-force", "If the trace is already enabled, we disable it first.", "BOOLEAN", false, "false"),
  _track_free("-track-free", "If true we also track frees, so we know the live memory consumption " \
              "and not just the total allocated amount. This costs some performance and memory.",
              "BOOLEAN", false, "false"),
  _detailed_stats("-detailed-stats", "Collect more detailed statistics. This will costs some " \
                  "CPU time, but no memory.", "BOOLEAN", false, "false") {
  _dcmdparser.add_dcmd_option(&_stack_depth);
  _dcmdparser.add_dcmd_option(&_use_backtrace);
  _dcmdparser.add_dcmd_option(&_only_nth);
  _dcmdparser.add_dcmd_option(&_force);
  _dcmdparser.add_dcmd_option(&_track_free);
  _dcmdparser.add_dcmd_option(&_detailed_stats);
}

void MallocTraceEnableDCmd::execute(DCmdSource source, TRAPS) {
  // Need to switch to native or the long operations block GCs.
  ThreadToNativeFromVM ttn(THREAD);

  TraceSpec spec;
  spec._stack_depth = (int) _stack_depth.value();
  spec._use_backtrace = _use_backtrace.value();
  spec._only_nth = (int) _only_nth.value();
  spec._force = _force.value();
  spec._track_free = _track_free.value();
  spec._detailed_stats = _detailed_stats.value();

  if (MallocStatistic::enable(_output, spec)) {
    _output->print_raw_cr("Malloc statistic enabled");
  }
}

MallocTraceDisableDCmd::MallocTraceDisableDCmd(outputStream* output, bool heap) :
  DCmdWithParser(output, heap) {
}

void MallocTraceDisableDCmd::execute(DCmdSource source, TRAPS) {
  // Need to switch to native or the long operations block GCs.
  ThreadToNativeFromVM ttn(THREAD);

  if (MallocStatistic::disable(_output)) {
    _output->print_raw_cr("Malloc statistic disabled.");
  }
}

MallocTraceDumpDCmd::MallocTraceDumpDCmd(outputStream* output, bool heap) :
  DCmdWithParser(output, heap),
  _dump_file("-dump-file", "If given the dump command writes the result to the given file. " \
             "Note that the filename is interpreted by the target VM. You can use " \
             "'stdout' or 'stderr' as filenames to dump via stdout or stderr of " \
             "the target VM", "STRING", false),
  _filter("-filter", "If given we only print a stack if it contains a function matching " \
          "the given string.", "STRING", false),
  _max_entries("-max-entries", "The maximum number of entries to dump.", "INT", false, "10"),
  _dump_fraction("-fraction", "If > 0 we dunp the given fraction of allocated bytes " \
                 "(or allocated objects if sorted by count). In that case the -max-entries " \
                 "option is ignored", "INT", false, "0"),
  _sort_by_count("-sort-by-count", "If given the stacks are sorted according to the number " \
                 "of allocations. Otherwise they are orted by the number of allocated bytes.",
                 "BOOLEAN", false),
  _internal_stats("-internal-stats", "If given some internal statistics about the overhead of " \
                  "the trace is included in the output", "BOOLEAN", false) {
  _dcmdparser.add_dcmd_option(&_dump_file);
  _dcmdparser.add_dcmd_option(&_filter);
  _dcmdparser.add_dcmd_option(&_max_entries);
  _dcmdparser.add_dcmd_option(&_dump_fraction);
  _dcmdparser.add_dcmd_option(&_sort_by_count);
  _dcmdparser.add_dcmd_option(&_internal_stats);
}

void MallocTraceDumpDCmd::execute(DCmdSource source, TRAPS) {
  // Need to switch to native or the long operations block GCs.
  ThreadToNativeFromVM ttn(THREAD);

  DumpSpec spec;
  spec._dump_file = _dump_file.value();
  spec._filter = _filter.value();
  spec._max_entries = _max_entries.value();
  spec._dump_fraction = _dump_fraction.value();
  spec._on_error = false;
  spec._sort_by_count = _sort_by_count.value();
  spec._internal_stats = _internal_stats.value();

  MallocStatistic::dump(_output, spec);
}

} // namespace sap

#endif // defined(LINUX) || defined(__APPLE__)

