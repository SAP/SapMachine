#include "precompiled.hpp"

#include "jvm_io.h"
#include "mallochooks.h"
#include "malloctrace/mallocTrace2.hpp"

#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/timer.hpp"
#include "utilities/ostream.hpp"
#include "utilities/ticks.hpp"

#include <pthread.h>

// Some defines for added debug/info functionality.
#define TIME_STACK_WALK
#define ALLOCATION_TRACKING_STATS
#define MARK_SHORT_STACKS

namespace sap {


//
//
//
//
// Class SafeAllocator
//
//
//
//
class SafeAllocator {
private:

  real_funcs_t* _funcs;
  size_t        _allocation_size;
  int           _entries_per_chunk;
  void**        _chunks;
  int           _nr_of_chunks;
  void**        _free_list;

public:
  SafeAllocator(size_t allocation_size, real_funcs_t* funcs);
  ~SafeAllocator();

  void* allocate();
  void free(void* ptr);
};

SafeAllocator::SafeAllocator(size_t allocation_size, real_funcs_t* funcs) :
  _funcs(funcs),
  _allocation_size(align_up(allocation_size, 8)), // We need no stricter alignment
  _entries_per_chunk(16384),
  _chunks(NULL),
  _nr_of_chunks(0),
  _free_list(NULL) {
}

SafeAllocator::~SafeAllocator() {
  for (int i = 0; i < _nr_of_chunks; ++i) {
    _funcs->free(_chunks[i]);
  }
}

void* SafeAllocator::allocate() {
  if (_free_list != NULL) {
    void* result = _free_list;
    _free_list = (void**) ((void**) result)[0];

    return result;
  }

  // We need a new chunk.
  char* new_chunk = (char*) _funcs->malloc(_entries_per_chunk * _allocation_size);

  if (new_chunk == NULL) {
    return NULL;
  }

  _nr_of_chunks += 1;
  void** new_chunks = (void**) _funcs->realloc(_chunks, sizeof(void**) * _nr_of_chunks);

  if (new_chunks == NULL) {
    return NULL;
  }

  new_chunks[_nr_of_chunks - 1] = new_chunk;
  _chunks = new_chunks;

  for (int i = 0; i < _entries_per_chunk; ++i) {
    free(new_chunk + i * _allocation_size);
  }

  return allocate();
}

void SafeAllocator::free(void* ptr) {
  if (ptr != NULL) {
    void** as_array = (void**) ptr;
    as_array[0] = (void*) _free_list;
    _free_list = as_array;
  }
}



class PthreadLocker : public StackObj {
private:
  pthread_mutex_t* _mutex;

public:
  PthreadLocker(pthread_mutex_t* mutex);
  ~PthreadLocker();
};

PthreadLocker::PthreadLocker(pthread_mutex_t* mutex) :
  _mutex(mutex) {
  if ((_mutex != NULL) && (pthread_mutex_lock(_mutex) != 0)) {
    fatal("Could not lock mutex");
  }
}

PthreadLocker::~PthreadLocker() {
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

// Must be a power of two minus 1.
#define MAX_FRAMES 31
#define FRAMES_TO_SKIP 0
// Must be a power of two.
#define NR_OF_STACK_MAPS 16
#define NR_OF_ALLOC_MAPS 16

class StatEntry {
private:
  StatEntry* _next;
  uint64_t              _hash_and_nr_of_frames;
  size_t                _size;
  size_t                _count;
  address                _frames[1];

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

  size_t size() {
    return _size;
  }

  size_t count() {
    return _count;
  }

  int nr_of_frames() {
    return _hash_and_nr_of_frames & MAX_FRAMES;
  }

  address* frames() {
    return _frames;
  }
};


// The entry for a single allocation. Note that we don't store the pointer itself
// but use the hash code instead. Our hash function is invertible, so this is OK.
class AllocEntry {
private:
  uint64_t    _hash;
  StatEntry*  _entry;
  AllocEntry* _next;

public:
  AllocEntry(uint64_t hash, StatEntry* entry) :
    _hash(hash),
    _entry(entry),
    _next(NULL) {
  }

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
};



static register_hooks_t* register_hooks;

static real_funcs_t* setup_hooks(registered_hooks_t* hooks, outputStream* st) {
  if (register_hooks == NULL) {
    register_hooks  = (register_hooks_t*) dlsym((void*) RTLD_DEFAULT, REGISTER_HOOKS_NAME);
  }

  if (register_hooks == NULL) {
    st->print_raw_cr("Could not find register_hooks function. Make sure to preload the malloc hooks library.");
    return NULL;
  }

  return register_hooks(hooks);
}

typedef int backtrace_func_t(void** stacks, int max_depth);

// A pthread mutex usable in arrays.
struct CacheLineSafeLock {
  pthread_mutex_t _lock;
  char            _pad[DEFAULT_CACHE_LINE_SIZE - sizeof(pthread_mutex_t)];
};

class MallocStatisticImpl : public AllStatic {
private:

  static real_funcs_t*      _funcs;
  static backtrace_func_t*  _backtrace;
  static bool               _use_backtrace;
  static volatile bool      _initialized;
  static bool               _enabled;
  static bool               _shutdown;
  static bool               _forbid_resizes;
  static bool               _track_free;
  static int                _max_frames;
  static registered_hooks_t _malloc_stat_hooks;
  static CacheLineSafeLock  _malloc_stat_lock;
  static pthread_key_t      _malloc_suspended;

  static StatEntry**        _stack_maps[NR_OF_STACK_MAPS];
  static CacheLineSafeLock  _stack_maps_lock[NR_OF_STACK_MAPS];
  static int                _stack_maps_mask[NR_OF_STACK_MAPS];
  static int                _stack_maps_size[NR_OF_STACK_MAPS];
  static int                _stack_maps_limit[NR_OF_STACK_MAPS];
  static SafeAllocator*     _stack_maps_alloc[NR_OF_STACK_MAPS];
  static int                _entry_size;

  static AllocEntry**       _alloc_maps[NR_OF_ALLOC_MAPS];
  static CacheLineSafeLock  _alloc_maps_lock[NR_OF_ALLOC_MAPS];
  static int                _alloc_maps_mask[NR_OF_ALLOC_MAPS];
  static int                _alloc_maps_size[NR_OF_ALLOC_MAPS];
  static int                _alloc_maps_limit[NR_OF_ALLOC_MAPS];
  static SafeAllocator*     _alloc_maps_alloc[NR_OF_ALLOC_MAPS];

  static int                _to_track_mask;

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

  static StatEntry* record_allocation_size(size_t to_add, int nr_of_frames, address* frames);
  static void record_allocation(uint64_t hash, size_t size, int nr_of_frames, address* frames);
  static void record_free(uint64_t hash, size_t size, int nr_of_frames, address* frames);

  static uint64_t ptr_hash(void* ptr);
  static bool     should_track(uint64_t hash);

  static void resize_stack_map(int map, int new_mask);
  static void cleanup_for_stack_map(int map);
  static void cleanup_for_alloc_map(int map);
  static void cleanup();

  static void dump_entry(outputStream* st, StatEntry* entry);
  static void create_statistic(bool for_siez, size_t* bins);

public:
  static void initialize(outputStream* st);
  static bool enable(outputStream* st, TraceSpec const& spec);
  static bool disable(outputStream* st);
  static bool reset(outputStream* st);
  static bool dump(outputStream* msg_stream, outputStream* dump_stream, DumpSpec const& spec, bool on_error);
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

real_funcs_t*     MallocStatisticImpl::_funcs;
backtrace_func_t* MallocStatisticImpl::_backtrace;
bool              MallocStatisticImpl::_use_backtrace;
volatile bool     MallocStatisticImpl::_initialized;
bool              MallocStatisticImpl::_enabled;
bool              MallocStatisticImpl::_shutdown;
bool              MallocStatisticImpl::_forbid_resizes;
bool              MallocStatisticImpl::_track_free;
int               MallocStatisticImpl::_max_frames;
CacheLineSafeLock MallocStatisticImpl::_malloc_stat_lock;
pthread_key_t     MallocStatisticImpl::_malloc_suspended;
StatEntry**       MallocStatisticImpl::_stack_maps[NR_OF_STACK_MAPS];
CacheLineSafeLock MallocStatisticImpl::_stack_maps_lock[NR_OF_STACK_MAPS];
int               MallocStatisticImpl::_stack_maps_mask[NR_OF_STACK_MAPS];
int               MallocStatisticImpl::_stack_maps_size[NR_OF_STACK_MAPS];
int               MallocStatisticImpl::_stack_maps_limit[NR_OF_STACK_MAPS];
SafeAllocator*    MallocStatisticImpl::_stack_maps_alloc[NR_OF_STACK_MAPS];
int               MallocStatisticImpl::_entry_size;
AllocEntry**      MallocStatisticImpl::_alloc_maps[NR_OF_ALLOC_MAPS];
CacheLineSafeLock MallocStatisticImpl::_alloc_maps_lock[NR_OF_ALLOC_MAPS];
int               MallocStatisticImpl::_alloc_maps_mask[NR_OF_ALLOC_MAPS];
int               MallocStatisticImpl::_alloc_maps_size[NR_OF_ALLOC_MAPS];
int               MallocStatisticImpl::_alloc_maps_limit[NR_OF_ALLOC_MAPS];
SafeAllocator*    MallocStatisticImpl::_alloc_maps_alloc[NR_OF_ALLOC_MAPS];
int               MallocStatisticImpl::_to_track_mask;


#define MAX_STACK_MAP_LOAD 0.5
#define MAX_ALLOC_MAP_LOAD 0.5

#if defined(TIME_STACK_WALK)
static volatile uint64_t stack_walk_time = 0;
static volatile uint64_t stack_walk_count = 0;

#define TIME_STACK_WALK_BEGIN \
   uint64_t ticks = Ticks::now().nanoseconds()

#define TIME_STACK_WALK_END \
  Atomic::add(&stack_walk_time, Ticks::now().nanoseconds() - ticks); \
  Atomic::add(&stack_walk_count, (uint64_t) 1)

#else
#define TIME_STACK_WALK_BEGIN
#define TIME_STACK_WALK_END
#endif

#if defined(ALLOCATION_TRACKING_STATS)
static volatile uint64_t tracked_ptrs = 0;
static volatile uint64_t not_tracked_ptrs = 0;
#endif

#if defined(MARK_SHORT_STACKS)

#define MARK_STACK_AND_FILL_WITH_BACKTRACE \
  frames[nr_of_frames] = (address) caller_address; \
  nr_of_frames += 1; break_here(); \
  if (_backtrace != NULL) { \
    nr_of_frames = _backtrace((void**) frames, _max_frames + FRAMES_TO_SKIP); \
    frames[0] = (address) break_here; \
  }

static void break_here() {
}

#else

#define MARK_STACK_AND_FILL_WITH_BACKTRACE \
  frames[nr_of_frames] = (address) caller_address; \
  nr_of_frames += 1;

#endif

#define CAPTURE_STACK \
  address frames[MAX_FRAMES + FRAMES_TO_SKIP]; \
  TIME_STACK_WALK_BEGIN; \
  int nr_of_frames = 0; \
  if (_use_backtrace) { \
    nr_of_frames = _backtrace((void**) frames, _max_frames + FRAMES_TO_SKIP); \
  } else { \
    frame fr = os::current_frame(); \
    while (fr.pc() && nr_of_frames < _max_frames + FRAMES_TO_SKIP) { \
      frames[nr_of_frames] = fr.pc(); \
      nr_of_frames += 1; \
      if (fr.fp() == NULL || fr.cb() != NULL || fr.sender_pc() == NULL || os::is_first_C_frame(&fr)) \
        break; \
      fr = os::get_sender_for_C_frame(&fr); \
    } \
    /* We know at least the caller addreess */ \
    if (nr_of_frames < 2) { \
      MARK_STACK_AND_FILL_WITH_BACKTRACE; \
    } \
  } \
  TIME_STACK_WALK_END

uint64_t MallocStatisticImpl::ptr_hash(void* ptr) {
  if (!_track_free && (_to_track_mask == 0)) {
    return 0;
  }

  uint64_t hash = (uint64_t) ptr;
#if 1
  hash = (~hash) + (hash << 21);
  hash = hash ^ (hash >> 24);
  hash = hash + hash * 265;
  hash = hash ^ (hash >> 14);
  hash = hash + hash * 21;
  hash = hash ^ (hash >> 28);
  hash = hash + (hash << 31);
#else
  hash = (~hash) + (hash << 18); // key = (key << 18) - key - 1;
  hash = hash ^ (hash >> 31);
  hash = hash * 21; // key = (key + (key << 2)) + (key << 4);
  hash = hash ^ (hash >> 11);
  hash = hash + (hash << 6);
  hash = hash ^ (hash >> 22);
#endif

  return hash;
}

bool MallocStatisticImpl::should_track(uint64_t hash) {
#if defined(ALLOCATION_TRACKING_STATS)
  if ((hash & _to_track_mask) == 0) {
    Atomic::add(&tracked_ptrs, (uint64_t) 1);
  } else {
    Atomic::add(&not_tracked_ptrs, (uint64_t) 1);
  }
#endif
  return (hash & _to_track_mask) == 0;
}

void* MallocStatisticImpl::malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) {
  void* result = real_malloc(size);
  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      record_allocation(hash, real_malloc_size(result), nr_of_frames, frames);
    } else {
      record_allocation_size(size, nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::calloc_hook(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size) {
  void* result = real_calloc(elems, size);
  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      record_allocation(hash, real_malloc_size(result), nr_of_frames, frames);
    } else {
      record_allocation_size(elems * size, nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::realloc_hook(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size) {
  size_t old_size = ptr != NULL ? real_malloc_size(ptr) : 0;
  uint64_t old_hash = ptr_hash(ptr);
  void* result = real_realloc(ptr, size);
  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && (should_track(old_hash) || should_track(hash)) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      if (should_track(old_hash)) {
        record_free(old_hash, old_size, nr_of_frames, frames);
      }

      if (should_track(hash)) {
        record_allocation(hash, real_malloc_size(result), nr_of_frames, frames);
      }
    } else if ((old_size < size) && should_track(hash)) {
      // Track the additional allocate bytes. This is somewhat wrong, since
      // we don't know the requested size of the original allocation and
      // old_size might be greater.
      record_allocation_size(size - old_size, nr_of_frames, frames);
    }
  } else if ((size == 0) && _track_free && should_track(old_hash)) {
    // Treat as free.
    CAPTURE_STACK;
    record_free(hash, old_size, nr_of_frames, frames);
  }

  return result;
}

void MallocStatisticImpl::free_hook(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size) {
  uint64_t hash = ptr_hash(ptr);

  if ((ptr != NULL) &&_track_free && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;
    record_free(hash, real_malloc_size(ptr), nr_of_frames, frames);
  }

  real_free(ptr);
}

int MallocStatisticImpl::posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size) {
  int result = real_posix_memalign(ptr, align, size);
  uint64_t hash = ptr_hash(*ptr);

  if ((result == 0) && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      record_allocation(hash, real_malloc_size(*ptr), nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(*ptr), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::memalign_hook(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size) {
  void* result = real_memalign(align, size);
  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      record_allocation(hash, real_malloc_size(result), nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::aligned_alloc_hook(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size) {
  void* result = real_aligned_alloc(align, size);
  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      record_allocation(hash, real_malloc_size(result), nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size) {
  void* result = real_valloc(size);
  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      record_allocation(hash, real_malloc_size(result), nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
}

void* MallocStatisticImpl::pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size) {
  void* result = real_pvalloc(size);
  uint64_t hash = ptr_hash(result);

  if ((result != NULL) && should_track(hash) && (pthread_getspecific(_malloc_suspended) == NULL)) {
    CAPTURE_STACK;

    if (_track_free) {
      record_allocation(hash, real_malloc_size(result), nr_of_frames, frames);
    } else {
      // Here we track the really allocated size, since it might be very different
      // from the requested one.
      record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
    }
  }

  return result;
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
    result = result * 31 + ((frame_addr & 0xfffffff0) >> 4) + 127 * (frame_addr >> 36);
  }

  // Avoid more bits than we can store in the entry.
  return (MAX_FRAMES + 1) * result/ (MAX_FRAMES + 1);
}

StatEntry*  MallocStatisticImpl::record_allocation_size(size_t to_add, int nr_of_frames, address* frames) {
  assert(!_track_free, "Only used for summary tracking");

  // Skip the top frame since it is always from the hooks.
  nr_of_frames = MAX2(nr_of_frames - FRAMES_TO_SKIP, 0);
  frames += FRAMES_TO_SKIP;

  assert(nr_of_frames <= _max_frames, "Overflow");

  size_t hash = hash_for_frames(nr_of_frames, frames);
  int idx = hash & (NR_OF_STACK_MAPS - 1);
  assert((idx >= 0) && (idx < NR_OF_STACK_MAPS), "invalid map index");

  PthreadLocker locker(&_stack_maps_lock[idx]._lock);

  if (!_enabled) {
    return NULL;
  }

  int slot = hash & _stack_maps_mask[idx];
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
    // First set the next pointer, so we can iterate the chain in parallel when we insert it into
    // the array in the next step.
    entry->set_next(_stack_maps[idx][slot]);
    // We need a fence here to guarantee that a paralle thread will see the full entry when
    // it sees the pointer in the array. Should not be very costly, since the we don't add often here.
    OrderAccess::fence();
    _stack_maps[idx][slot] = entry;
    _stack_maps_size[idx] += 1;

    if (!_forbid_resizes && (_stack_maps_size[idx] > _stack_maps_limit[idx])) {
      resize_stack_map(idx, _stack_maps_mask[idx] * 2 + 1);
    }

    return entry;
  }

  return NULL;
}

void MallocStatisticImpl::record_allocation(uint64_t hash, size_t size, int nr_of_frames, address* frames) {
  assert(_track_free, "Only used for detailed tracking");

  StatEntry* stat_entry = record_allocation_size(size, nr_of_frames, frames);

  if (stat_entry == NULL) {
    return;
  }
}

void MallocStatisticImpl::record_free(uint64_t hash, size_t size, int nr_of_frames, address* frames) {
  assert(_track_free, "Only used for detailed tracking");
}

void MallocStatisticImpl::cleanup_for_stack_map(int idx) {
  PthreadLocker locker(&_stack_maps_lock[idx]._lock);

  if (_stack_maps_alloc[idx] != NULL) {
    _stack_maps_alloc[idx]->~SafeAllocator();
    _funcs->free(_stack_maps_alloc[idx]);
    _stack_maps_alloc[idx] = NULL;
  }

  if (_stack_maps[idx] != NULL) {
    _funcs->free(_stack_maps[idx]);
    _stack_maps[idx] = NULL;
  }
}

void MallocStatisticImpl::cleanup_for_alloc_map(int idx) {
  PthreadLocker locker(&_alloc_maps_lock[idx]._lock);

  if (_alloc_maps_alloc[idx] != NULL) {
    _alloc_maps_alloc[idx]->~SafeAllocator();
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

void MallocStatisticImpl::initialize(outputStream* st) {
  if (!_initialized) {
    _initialized = true;

    if (pthread_mutex_init(&_malloc_stat_lock._lock, NULL) != 0) {
      fatal("Could not initialize lock");
    }

    if (pthread_key_create(&_malloc_suspended, NULL) != 0) {
      fatal("Could not initialize key");
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

    _backtrace = (backtrace_func_t*) dlsym(RTLD_DEFAULT, "backtrace");

    if (dlerror() != NULL) {
      _backtrace = NULL;
    }

    if (_backtrace != NULL) {
      // Trigger initialization needed.
      void* tmp[1];
      _backtrace(tmp, 1);
    }
  }
}

bool MallocStatisticImpl::enable(outputStream* st, TraceSpec const& spec) {
  initialize(st);
  PthreadLocker lock(&_malloc_stat_lock._lock);

  if (_enabled) {
    if (spec._force) {
      _enabled = false;
      setup_hooks(NULL, st);
      cleanup();
      _funcs = NULL;

      st->print_raw_cr("Disabling already running trace first.");
    } else {
      st->print_raw_cr("malloc statistic is already enabled!");

      return false;
    }
  }

  if (_shutdown) {
    st->print_raw_cr("malloc statistic is already shut down!");

    return false;
  }

  if (spec._stack_depth < 1 || spec._stack_depth > MAX_FRAMES) {
    st->print_cr("The given stack depth %d is outside of the valid range [%d, %d]",
                 spec._stack_depth, 1, MAX_FRAMES);

    return false;
  }

  _track_free = false;
  _to_track_mask = (1 << spec._skip_exp) - 1;

  if (_to_track_mask != 0) {
    st->print_cr("Tracking about every %d allocations.", _to_track_mask + 1);
  }

  _use_backtrace = spec._use_backtrace && (_backtrace != NULL);

#if defined(ALLOCATION_TRACKING_STATS)
  tracked_ptrs = 0;
  not_tracked_ptrs = 0;
#endif

  if (_use_backtrace && spec._use_backtrace) {
    st->print_raw_cr("Using backtrace() to sample stacks.");
  } else if (spec._use_backtrace) {
    st->print_raw_cr("Using fallback mechanism to sample stacks, since backtrace() was not available.");
  } else {
    st->print_raw_cr("Using fallback mechanism to sample stacks.");
  }

  _max_frames = spec._stack_depth;
  _funcs = setup_hooks(&_malloc_stat_hooks, st);

  if (_funcs == NULL) {
    return false;
  }

  _entry_size = sizeof(StatEntry) + sizeof(address) * (_max_frames - 1);

  for (int i = 0; i < NR_OF_STACK_MAPS; ++i) {
    void* mem = _funcs->malloc(sizeof(SafeAllocator));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the allocator");
      cleanup();

      return false;
    }

    _stack_maps_alloc[i] = new (mem) SafeAllocator(sizeof(StatEntry) + sizeof(address) * (_max_frames - 1), _funcs);
    _stack_maps_mask[i] = 127;
    _stack_maps_size[i] = 0;
    _stack_maps_limit[i] = (int) ((_stack_maps_mask[i] + 1) * MAX_STACK_MAP_LOAD);
    _stack_maps[i] = (StatEntry**) _funcs->calloc(_stack_maps_mask[i] + 1, sizeof(StatEntry*));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the stack map");
      cleanup();

      return false;
    }
  }

  for (int i = 0; i < NR_OF_ALLOC_MAPS; ++i) {
    void* mem = _funcs->malloc(sizeof(SafeAllocator));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the allocator");
      cleanup();

      return false;
    }

    _alloc_maps_alloc[i] = new (mem) SafeAllocator(sizeof(AllocEntry), _funcs);
    _alloc_maps_mask[i] = 127;
    _alloc_maps_size[i] = 0;
    _alloc_maps_limit[i] = (int) ((_alloc_maps_mask[i] + 1) * MAX_ALLOC_MAP_LOAD);
    _alloc_maps[i] = (AllocEntry**) _funcs->calloc(_alloc_maps_mask[i] + 1, sizeof(AllocEntry*));

    if (mem == NULL) {
      st->print_raw_cr("Could not allocate the alloc map");
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
  initialize(st);
  PthreadLocker lock(&_malloc_stat_lock._lock);

  if (!_enabled) {
    st->print_raw_cr("malloc statistic is already disabled!");

    return false;
  }

  _enabled = false;
  setup_hooks(NULL, st);
  cleanup();
  _funcs = NULL;

  return true;
}

bool MallocStatisticImpl::reset(outputStream* st) {
  initialize(st);
  PthreadLocker lock(&_malloc_stat_lock._lock);

  if (!_enabled) {
    st->print_raw_cr("malloc statistic not enabled!");

    return false;
  }

  return true;  
}

int fast_log2(unsigned long long v) {
#if defined(__GNUC__) || defined(_AIX) || defined(__APPLE__)
  return 63 - __builtin_clzll(v);
#else
  int result = 0;

  if ((v >> 32) != 0) {
    result += 32;
    v >>= 32;
  }

  if ((v >> 16) != 0) {
    result += 16;
    v >>= 16;
  }

  if ((v >> 8) != 0) {
    result += 8;
    v >>= 8;
  }

  if ((v >> 4) != 0) {
    result += 4;
    v >>= 4;
  }

  if ((v >> 2) != 0) {
    result += 2;
    v >>= 2;
  }

  if ((v >> 1) != 0) {
    result += 1;
    v >>= 1;
  }

  return result;
#endif
}

int get_index_for_size(size_t size) {
  int base = fast_log2((unsigned long long) size);

  return 2 * base + ((size >> (base - 1)) & 1);
}

uint64_t get_size_for_index(int index) {
  uint64_t base = ((uint64_t) 1) << (index / 2);

  if (index & 1) {
    return base + base / 2;
  }

  return base;
}

#define NR_OF_BINS 127

void MallocStatisticImpl::create_statistic(bool for_size, size_t* bins) {
  for (int i = 0; i < NR_OF_BINS; ++i) {
    bins[i] = 0;
  }

  for (int idx = 0; idx < NR_OF_STACK_MAPS; ++idx) {
    StatEntry** map = _stack_maps[idx];

    for (int slot = 0; slot <= _stack_maps_mask[idx]; ++slot) {
      StatEntry* entry = map[slot];

      // Needed to make sure we see the full content of the entry.
      if (entry != NULL) {
        OrderAccess::fence();
      }

      while (entry != NULL) {
        if (for_size) {
          bins[get_index_for_size(entry->size())] += 1;
        } else {
          bins[get_index_for_size(entry->count())] += 1;
        }

        entry = entry->next();
      }
    }
  }
}

size_t calc_min_from_statistic(size_t* bins, double factor) {
  size_t total = 0;

  for (int i = 0; i < NR_OF_BINS; ++i) {
    total += bins[i] * get_size_for_index(i);
  }

  size_t sum = 0;

  for (int i = NR_OF_BINS - 1; i >= 0; --i) {
    sum += bins[i] * get_size_for_index(i);

    if (sum > factor * total) {
      return get_size_for_index(i);
    }
  }

  return 0;
}

void MallocStatisticImpl::dump_entry(outputStream* st, StatEntry* entry) {
  // Use a temp buffer since the output stream might use unbuffered I/O.
  char ss_tmp[4096];
  stringStream ss(ss_tmp, sizeof(ss_tmp));

  ss.print_cr("Allocated bytes : " UINT64_FORMAT, (uint64_t) entry->size());
  ss.print_cr("Allocated objects: " UINT64_FORMAT, (uint64_t) entry->count());
  ss.print_cr("Stack (%d frames):", entry->nr_of_frames());

  char tmp[256];

  for (int i = 0; i < entry->nr_of_frames(); ++i) {
    address frame = entry->frames()[i];
    ss.print("  %p  ", frame);

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

    // Flush the temp buffer if we are near the end.
    if (sizeof(ss_tmp) - ss.size() < sizeof(tmp)) {
      st->write(ss_tmp, ss.size());
      ss.reset();
    }
  }

  if (entry->nr_of_frames() == 0) {
    ss.print_raw_cr("  <no stack>");
  }

  st->write(ss_tmp, ss.size());
}

static int sort_by_size(const void* p1, const void* p2) {
  StatEntry* e1 = *(StatEntry**) p1;
  StatEntry* e2 = *(StatEntry**) p2;

  if (e1->size() > e2->size()) {
    return -1;
  }

  if (e1->size() < e2->size()) {
    return 1;
  }

  return 0;
}

static int sort_by_count(const void* p1, const void* p2) {
  StatEntry* e1 = *(StatEntry**) p1;
  StatEntry* e2 = *(StatEntry**) p2;

  if (e1->count() > e2->count()) {
    return -1;
  }

  if (e1->count() < e2->count()) {
    return 1;
  }

  return 0;
}

bool MallocStatisticImpl::dump(outputStream* msg_stream, outputStream* dump_stream, DumpSpec const& spec, bool on_error) {
  if (!on_error) {
    initialize(msg_stream);
  }

  // Handle recusrive allocations by just performing them without tracking.
  pthread_setspecific(_malloc_suspended, (void*) 1);

  // We need to avoid having the trace disabled concurrently.
  PthreadLocker lock(on_error ? NULL : &_malloc_stat_lock._lock);

  if (!_enabled) {
    msg_stream->print_raw_cr("malloc statistic not enabled!");
    pthread_setspecific(_malloc_suspended, NULL);

    return false;
  }

  const char* sort = NULL;
  int (*sort_algo)(const void*, const void*) = NULL;
  int added_entries = 0;
  int max_entries = 1024;
  StatEntry** to_sort = NULL;

  if (spec._max_entries > 0) {
    // Makes only sense if we sort.
    sort = "size";
  }

  sort = spec._sort == NULL ? sort : spec._sort;

  if (sort != NULL) {
    if (strcmp("size", sort) == 0) {
      sort_algo = sort_by_size;
    } else if (strcmp("count", sort) == 0) {
      sort_algo = sort_by_count;
    } else {
      msg_stream->print_cr("Invalid sorting argument '%s'. Muste be 'size' or 'count'.", sort);
      pthread_setspecific(_malloc_suspended, NULL);

      return false;
    }

    // The colde below handles a failed allocation.
    to_sort = (StatEntry**) _funcs->calloc(max_entries, sizeof(StatEntry*));
  }

  if (_backtrace != NULL) {
    dump_stream->print_raw_cr("Stacks collected via backtrace().");
  }

  elapsedTimer timer;
  timer.start();

  // Forbid resizes, since we don't want the chaining of the entries to change.
  // Should be no big deal, since the next addition would trigger the resize.
  _forbid_resizes = true;

  // Get the lock for each map, so we are sure the add-code will see the _forbid_reiszes
  // field.
  for (int i = 0; i < NR_OF_STACK_MAPS; ++i) {
    PthreadLocker locker(&_stack_maps_lock[i]._lock);
  }

  size_t min_size = 0;
  size_t min_count = 0;

  // Approximately determine a min size and count to only display the requested fractions.
  if (spec._size_fraction < 100) {
    size_t bins[NR_OF_BINS];
    create_statistic(true, bins);
    min_size = calc_min_from_statistic(bins, spec._size_fraction * 0.01);
  }

  if (spec._count_fraction < 100) {
    size_t bins[NR_OF_BINS];
    create_statistic(false, bins);
    min_count = calc_min_from_statistic(bins, spec._count_fraction * 0.01);
  }

  size_t total_size = 0;
  size_t total_count = 0;
  size_t total_stacks = 0;
  int stacks_dumped = 0;

  for (int idx = 0; idx < NR_OF_STACK_MAPS; ++idx) {
    StatEntry** map = _stack_maps[idx];

    for (int slot = 0; slot <= _stack_maps_mask[idx]; ++slot) {
      StatEntry* entry = map[slot];

      while (entry != NULL) {
        total_size += entry->size();
        total_count += entry->count();
        total_stacks += 1;

        if ((entry->size() < min_size) || (entry->count() < min_count)) {
          // We don't track this.
        } else if (to_sort == NULL) {
          dump_entry(dump_stream, entry);
          stacks_dumped += 1;
        } else {
          to_sort[added_entries] = entry;
          added_entries += 1;

          if (added_entries >= max_entries) {
            max_entries += 1024;
            StatEntry** new_to_sort = (StatEntry** ) _funcs->realloc(to_sort, max_entries * sizeof(StatEntry*));

            if (new_to_sort == NULL) {
              for (int i = 0; i < added_entries; ++i) {
                dump_entry(dump_stream, to_sort[i]);
                stacks_dumped += 1;
              }

              _funcs->free(to_sort);
              to_sort = NULL;
            } else {
              to_sort = new_to_sort;
            }
          }
        }

        entry = entry->next();
      }
    }
  }

  if (sort_algo != NULL) {
    int to_print = MIN2(added_entries, spec._max_entries);

    msg_stream->print_cr("%d stacks sort by %s", to_print, sort);
    qsort(to_sort, added_entries, sizeof(StatEntry*), sort_algo);

    for (int i = 0; i < to_print; ++i) {
      dump_entry(dump_stream ,to_sort[i]);
      stacks_dumped += 1;
    }

    _funcs->free(to_sort);
  }

  dump_stream->print_cr("Total allocation size  : " UINT64_FORMAT, (uint64_t) total_size);
  dump_stream->print_cr("Total allocations count: " UINT64_FORMAT, (uint64_t) total_count);
  dump_stream->print_cr("Total unique stacks    : " UINT64_FORMAT, (uint64_t) total_stacks);

  timer.stop();
  msg_stream->print_cr("Dump finished in %.1f seconds (%.3f stacks per second).", timer.seconds(),
      stacks_dumped  / timer.seconds());

#if defined(TIME_STACK_WALK)
  msg_stream->print_cr("Sampled " UINT64_FORMAT " stacks using " UINT64_FORMAT " ns per stack",
                       stack_walk_count, stack_walk_time / MAX2(stack_walk_count, (uint64_t) 1));
  msg_stream->print_cr("Sampling took %.1f seconds in total", stack_walk_time * 1e-9);
#endif

#if defined(ALLOCATION_TRACKING_STATS)
  msg_stream->print_cr("tracked alllocations " UINT64_FORMAT ", untracked allocations " UINT64_FORMAT,
                       tracked_ptrs, not_tracked_ptrs);

  if (tracked_ptrs > 0) {
    msg_stream->print_cr("%.2f %% of the allocations were tracked, about every %.2f allocations " \
                         "(target %d)", 100.0 * tracked_ptrs / (tracked_ptrs + not_tracked_ptrs),
                         (tracked_ptrs + not_tracked_ptrs) / ((float) tracked_ptrs), (int) (_to_track_mask + 1));
  }
#endif

  pthread_setspecific(_malloc_suspended, NULL);
  _forbid_resizes = false;

  return true;
}

void MallocStatisticImpl::shutdown() {
  _shutdown = true;

  if (_initialized) {
    _enabled = false;

    if (register_hooks == NULL) {
      register_hooks(NULL);
    }
  }
}

void MallocStatistic::initialize() {
  MallocStatisticImpl::initialize(NULL);
}

bool MallocStatistic::enable(outputStream* st, TraceSpec const& spec) {
  return MallocStatisticImpl::enable(st, spec);
}

bool MallocStatistic::disable(outputStream* st) {
  return MallocStatisticImpl::disable(st);
}

bool MallocStatistic::reset(outputStream* st) {
  return MallocStatisticImpl::reset(st);
}

bool MallocStatistic::dump(outputStream* st, DumpSpec const& spec, bool on_error) {
  const char* dump_file = spec._dump_file;

  if ((dump_file != NULL) && (strlen(dump_file) > 0)) {
    int fd;

    if (strcmp("stderr", dump_file) == 0) {
      fd = 2;
    } else if (strcmp("stdout", dump_file) == 0) {
      fd = 1;
    } else {
      fd = ::open(dump_file, O_WRONLY | O_CREAT | O_TRUNC);

      if (fd < 0) {
        st->print_cr("Could not open '%s' for output.", dump_file);

        return false;
      }
    }

    fdStream dump_stream(fd);
    bool result = MallocStatisticImpl::dump(st, &dump_stream, spec, on_error);

    if ((fd != 1) && (fd != 2)) {
      ::close(fd);
    }

    return fd;
  }

  return MallocStatisticImpl::dump(st, st, spec, on_error);
}

void MallocStatistic::shutdown() {
  MallocStatisticImpl::shutdown();
}

//
//
//
//
// Class MallocStatisticDCmd
//
//
//
//
MallocStatisticDCmd::MallocStatisticDCmd(outputStream* output, bool heap) :
  DCmdWithParser(output, heap),
  _cmd("cmd", "enable,disable,reset,dump,test", "STRING", true),
  _stack_depth("-stack-depth", "The maximum stack depth to track", "INT", false, "5"),
  _use_backtrace("-use-backtrace", "If true we try to use the backtrace() method to sample " \
                 "the stack traces.", "BOOLEAN", false, "true"),
  _skip_allocations("-skip-allocations", "If > 0 we only track about every 2^N allocation.",
                    "INT", false, "0"),
  _force("-force", "If the trace is already enabled, we disable it first.", "BOOLEAN", false, "false"),
  _dump_file("-dump-file", "If given the dump command writes the result to the given file. " \
             "Note that the filename is interpreted by the target VM. You can use " \
             "'stdout' or 'stderr' as filenames to dump via stdout or stderr of " \
             "the target VM", "STRING", false),
  _size_fraction("-size-fraction", "The fraction in percent of the total size the output " \
                 "must contain.", "INT", false, "100"),
  _count_fraction("-count-fraction", "The fraction in percent of the total allocation count " \
                  "the output must contain.", "INT", false, "100"),
  _max_entries("-max-entries", "The maximum number of entries to dump.", "INT", false, "-1"),
  _sort("-sort", "If given the stacks are sorted. If the argument is 'size' they are " \
       "sorted by size and if the argument is 'count' the are sorted by allocation " \
       "count.", "STRING", false) {
  _dcmdparser.add_dcmd_argument(&_cmd);
  _dcmdparser.add_dcmd_option(&_stack_depth);
  _dcmdparser.add_dcmd_option(&_use_backtrace);
  _dcmdparser.add_dcmd_option(&_skip_allocations);
  _dcmdparser.add_dcmd_option(&_force);
  _dcmdparser.add_dcmd_option(&_dump_file);
  _dcmdparser.add_dcmd_option(&_size_fraction);
  _dcmdparser.add_dcmd_option(&_count_fraction);
  _dcmdparser.add_dcmd_option(&_max_entries);
  _dcmdparser.add_dcmd_option(&_sort);
}

void MallocStatisticDCmd::execute(DCmdSource source, TRAPS) {
  // Need to switch to native or the long operations block GCs.
  ThreadToNativeFromVM ttn(THREAD);

  const char* const cmd = _cmd.value();

  if (strcmp(cmd, "enable") == 0) {
    TraceSpec spec;
    spec._stack_depth = (int) _stack_depth.value();
    spec._use_backtrace = _use_backtrace.value();
    spec._skip_exp = (int) _skip_allocations.value();
    spec._force = _force.value();

    if (MallocStatistic::enable(_output, spec)) {
      _output->print_raw_cr("mallocstatistic enabled");
    }
  } else if (strcmp(cmd, "disable") == 0) {
    if (MallocStatistic::disable(_output)) {
      _output->print_raw_cr("mallocstatistic disabled");
    }
  } else if (strcmp(cmd, "reset") == 0) {
    MallocStatistic::reset(_output);
  } else if (strcmp(cmd, "dump") == 0) {
    DumpSpec spec;
    spec._dump_file = _dump_file.value();
    spec._sort = _sort.value();
    spec._size_fraction = _size_fraction.value();
    spec._count_fraction = _count_fraction.value();
    spec._max_entries = _max_entries.value();

    MallocStatistic::dump(_output, spec, false);
  } else if (strcmp(cmd, "test") == 0) {
    real_funcs_t* funcs = setup_hooks(NULL, _output);
    static void* results[1024 * 1024];

    for (int r = 0; r < 10; ++r) {

      for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        results[i] = NULL;
      }

      SafeAllocator alloc(96, funcs);
      for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        results[i] = alloc.allocate();
        alloc.free(results[(317 * (int64_t) i) & (sizeof(results) / sizeof(results[0]) - 1)]);
      }
    }

    for (int i = 0; i < 63; ++i) {
      size_t base = ((size_t) 1) << i;
      _output->print_cr(UINT64_FORMAT " -> %d", (uint64_t) base - 1, get_index_for_size(base - 1));
      _output->print_cr(UINT64_FORMAT " -> %d", (uint64_t) base, get_index_for_size(base));
      _output->print_cr(UINT64_FORMAT " -> %d", (uint64_t) base + 1, get_index_for_size(base + 1));
      base = base + base / 2;
      _output->print_cr(UINT64_FORMAT " -> %d", (uint64_t) base - 1, get_index_for_size(base - 1));
      _output->print_cr(UINT64_FORMAT " -> %d", (uint64_t) base, get_index_for_size(base));
      _output->print_cr(UINT64_FORMAT " -> %d", (uint64_t) base + 1, get_index_for_size(base + 1));
      _output->cr();
    }
  } else {
    _output->print_cr("Unknown command '%s'", cmd);
  }
}

}

