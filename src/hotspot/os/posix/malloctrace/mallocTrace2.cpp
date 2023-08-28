#include "precompiled.hpp"

#include "jvm_io.h"
#include "mallochooks.h"
#include "malloctrace/mallocTrace2.hpp"
#include "utilities/ostream.hpp"

#include <pthread.h>



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
// Class MallocStatisticEntry
//
//
//
//

// Must be a power of two minus 1.
#define MAX_FRAMES 31

class MallocStatisticEntry {
private:
	MallocStatisticEntry* _next;
	uint64_t              _hash_and_nr_of_frames;
	size_t                _size;
	size_t                _nr_of_allocations;
	address	              _frames[1];

public:
	MallocStatisticEntry(size_t hash, size_t size, int nr_of_frames, address* frames) :
		_next(NULL),
		_hash_and_nr_of_frames((hash * (MAX_FRAMES + 1)) + nr_of_frames),
		_size(size),
		_nr_of_allocations(1) {
		memcpy(_frames, frames, sizeof(address) * nr_of_frames);
		assert(nr_of_frames <= MAX_FRAMES, "too many frames");
	}

	uint64_t hash() {
		return _hash_and_nr_of_frames / (MAX_FRAMES + 1);
	}

	MallocStatisticEntry* next() {
		return _next;
	}

	void set_next(MallocStatisticEntry* next) {
		_next = next;
	}

	void add_allocation(size_t size) {
		_size += size;
		_nr_of_allocations += 1;
	}

	size_t size() {
		return _size;
	}

	size_t nr_of_allocations() {
		return _nr_of_allocations;
	}

	int nr_of_frames() {
		return _hash_and_nr_of_frames & MAX_FRAMES;
	}

	address* frames() {
		return _frames;
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


// A pthread mutex usable in arrays.
struct CacheLineSafeLock {
	pthread_mutex_t _lock;
	char            _pad[DEFAULT_CACHE_LINE_SIZE - sizeof(pthread_mutex_t)];
};

// Must be a power of two.
#define NR_OF_MAPS 16

class MallocStatisticImpl : public AllStatic {
private:

	static real_funcs_t*          _funcs;
	static volatile bool          _initialized;
	static bool                   _enabled;
	static bool                   _shutdown;
	static bool                   _track_free;
	static int                    _max_frames;
	static registered_hooks_t     _malloc_stat_hooks;
	static CacheLineSafeLock      _malloc_stat_lock;
	static pthread_key_t          _malloc_suspended;
	static MallocStatisticEntry** _maps[NR_OF_MAPS];
	static CacheLineSafeLock      _maps_lock[NR_OF_MAPS];
	static int                    _maps_mask[NR_OF_MAPS];
	static int                    _maps_size[NR_OF_MAPS];
	static int                    _maps_limit[NR_OF_MAPS];
	static SafeAllocator*         _allocators[NR_OF_MAPS];
	static int                    _entry_size;

	// The hooks.
	static void* malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) ;
	static void* calloc_hook(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size);
	static void* realloc_hook(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size);
	static void free_hook(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size);
	static int posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size);
	static void* memalign_hook(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size);
	static void* aligned_alloc_hook(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size);
	static void* valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size);
	static void* pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size);

	static void record_allocation_size(size_t to_add, int nr_of_frames, address* frames);
	static void record_allocation(void* ptr, size_t size, int nr_of_frames, address* frames);
	static void record_free(void* ptr, size_t size, int nr_of_frames, address* frames);

	static void resize_map(int map, int new_mask);
	static void cleanup_for_map(int map);
	static void cleanup();

	static void dump_entry(outputStream* st, MallocStatisticEntry* entry);
	static void create_statistic(bool on_error, size_t* size_bins, size_t* allocation_bins);

public:
	static void initialize(outputStream* st);
	static bool enable(outputStream* st, int stack_depth);
	static bool disable(outputStream* st);
	static bool reset(outputStream* st);
	static bool dump(outputStream* st, bool on_error);
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

real_funcs_t*          MallocStatisticImpl::_funcs;
volatile bool          MallocStatisticImpl::_initialized;
bool                   MallocStatisticImpl::_enabled;
bool                   MallocStatisticImpl::_shutdown;
bool                   MallocStatisticImpl::_track_free;
int                    MallocStatisticImpl::_max_frames;
CacheLineSafeLock      MallocStatisticImpl::_malloc_stat_lock;
pthread_key_t          MallocStatisticImpl::_malloc_suspended;
MallocStatisticEntry** MallocStatisticImpl::_maps[NR_OF_MAPS];
CacheLineSafeLock      MallocStatisticImpl::_maps_lock[NR_OF_MAPS];
int                    MallocStatisticImpl::_maps_mask[NR_OF_MAPS];
int                    MallocStatisticImpl::_maps_size[NR_OF_MAPS];
int                    MallocStatisticImpl::_maps_limit[NR_OF_MAPS];
SafeAllocator*         MallocStatisticImpl::_allocators[NR_OF_MAPS];
int                    MallocStatisticImpl::_entry_size;


#define MAX_LOAD 0.5

#define CAPTURE_STACK \
	address frames[MAX_FRAMES + 1]; \
	int nr_of_frames = 0; \
	frame fr = os::current_frame(); \
	while (fr.pc() && nr_of_frames <= _max_frames) { \
		frames[nr_of_frames] = fr.pc(); \
		nr_of_frames += 1; \
		if (fr.fp() == NULL || fr.cb() != NULL || fr.sender_pc() == NULL || os::is_first_C_frame(&fr)) \
			break; \
		if (fr.sender_pc() && !os::is_first_C_frame(&fr)) { \
			fr = os::get_sender_for_C_frame(&fr); \
		} else { \
			break; \
		} \
	}


void* MallocStatisticImpl::malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) {
	void* result = real_malloc(size);

	if ((result != NULL) && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_allocation(result, real_malloc_size(result), nr_of_frames, frames);
		} else {
			record_allocation_size(size, nr_of_frames, frames);
		}
	}

	return result;
}

void* MallocStatisticImpl::calloc_hook(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size) {
	void* result = real_calloc(elems, size);

	if ((result != NULL) && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_allocation(result, real_malloc_size(result), nr_of_frames, frames);
		} else {
			record_allocation_size(elems * size, nr_of_frames, frames);
		}
	}

	return result;
}

void* MallocStatisticImpl::realloc_hook(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size) {
	size_t old_size = ptr != NULL ? real_malloc_size(ptr) : 0;
	void* result = real_realloc(ptr, size);

	if ((result != NULL) && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_free(result, old_size, nr_of_frames, frames);
			record_allocation(result, real_malloc_size(result), nr_of_frames, frames);
		} else if (old_size < size) {
			// Track the additional allocate bytes. This is somewhat wrong, since
			// we don't know the requested size of the original allocation and
			// old_size might be greater.
			record_allocation_size(size - old_size, nr_of_frames, frames);
		}
	} else if ((size == 0) && _track_free) {
		// Treat as free.
		CAPTURE_STACK;
		record_free(result, old_size, nr_of_frames, frames);
	}

	return result;
}

void MallocStatisticImpl::free_hook(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size) {
	if ((ptr != NULL) &&_track_free && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;
		record_free(ptr, real_malloc_size(ptr), nr_of_frames, frames);
	}

	real_free(ptr);
}

int MallocStatisticImpl::posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size) {
	int result =  real_posix_memalign(ptr, align, size);

	if ((result == 0) && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_allocation(*ptr, real_malloc_size(*ptr), nr_of_frames, frames);
		} else {
			// Here we track the really allocated size, since it might be very different
			// from the requested one.
			record_allocation_size(real_malloc_size(*ptr), nr_of_frames, frames);
		}
	}

	return result;
}

void* MallocStatisticImpl::memalign_hook(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size) {
	void* result =  real_memalign(align, size);

	if ((result != NULL) && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_allocation(result, real_malloc_size(result), nr_of_frames, frames);
		} else {
			// Here we track the really allocated size, since it might be very different
			// from the requested one.
			record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
		}
	}

	return result;
}

void* MallocStatisticImpl::aligned_alloc_hook(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size) {
	void* result =  real_aligned_alloc(align, size);

	if ((result != NULL)  && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_allocation(result, real_malloc_size(result), nr_of_frames, frames);
		} else {
			// Here we track the really allocated size, since it might be very different
			// from the requested one.
			record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
		}
	}

	return result;
}

void* MallocStatisticImpl::valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size) {
	void* result =  real_valloc(size);

	if ((result != NULL) && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_allocation(result, real_malloc_size(result), nr_of_frames, frames);
		} else {
			// Here we track the really allocated size, since it might be very different
			// from the requested one.
			record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
		}
	}

	return result;
}

void* MallocStatisticImpl::pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size) {
	void* result =  real_pvalloc(size);

	if ((result != NULL) && (pthread_getspecific(_malloc_suspended) == NULL)) {
		CAPTURE_STACK;

		if (_track_free) {
			record_allocation(result, real_malloc_size(result), nr_of_frames, frames);
		} else {
			// Here we track the really allocated size, since it might be very different
			// from the requested one.
			record_allocation_size(real_malloc_size(result), nr_of_frames, frames);
		}
	}

	return result;
}

static bool is_same_stack(MallocStatisticEntry* to_check, int nr_of_frames, address* frames) {
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

	return result;
}

void MallocStatisticImpl::record_allocation_size(size_t to_add, int nr_of_frames, address* frames) {
	assert(!_track_free, "Only used for summary tracking");

	// Skip the top frame since it is always from the hooks.
	nr_of_frames = MAX2(nr_of_frames - 1, 0);
	frames += 1;

	size_t hash = hash_for_frames(nr_of_frames, frames);
	int idx = hash & (NR_OF_MAPS - 1);
	assert((idx >= 0) && (idx < NR_OF_MAPS), "invalid map index");
	hash /= NR_OF_MAPS;

	PthreadLocker locker(&_maps_lock[idx]._lock);

	if (!_enabled) {
		return;
	}

	int slot = hash & _maps_mask[idx];
	assert((slot >= 0) || (slot <= _maps_mask[idx]), "Invalid slot");
	MallocStatisticEntry* to_check = _maps[idx][slot];

	// Check if we already know this stack.
	while (to_check != NULL) {
		if ((to_check->hash() == hash) && (to_check->nr_of_frames() == nr_of_frames)) {
			if (is_same_stack(to_check, nr_of_frames, frames)) {
				to_check->add_allocation(to_add);

				return;
			}
		}

		to_check = to_check->next();
	}

	// Need a new entry. Fail silently if we don't get the memory.
	//if (nr_of_frames >= 0) return;
	void* mem = _allocators[idx]->allocate();

	if (mem != NULL) {
		MallocStatisticEntry* entry = new (mem) MallocStatisticEntry(hash, to_add, nr_of_frames, frames);
		entry->set_next(_maps[idx][slot]);
		_maps[idx][slot] = entry;
		_maps_size[idx] += 1;

		if (_maps_size[idx] > _maps_limit[idx]) {
			resize_map(idx, _maps_mask[idx] * 2 + 1);
		}
	}
}

void MallocStatisticImpl::record_allocation(void* ptr, size_t size, int nr_of_frames, address* frames) {
	assert(_track_free, "Only used for detailed tracking");
}

void MallocStatisticImpl::record_free(void* ptr, size_t size, int nr_of_frames, address* frames) {
	assert(_track_free, "Only used for detailed tracking");
}

void MallocStatisticImpl::cleanup_for_map(int idx) {
	PthreadLocker locker(&_maps_lock[idx]._lock);

	if (_allocators[idx] != NULL) {
		_allocators[idx]->~SafeAllocator();
		_funcs->free(_allocators[idx]);
		_allocators[idx] = NULL;
	}

	if (_maps[idx] != NULL) {
		_funcs->free(_maps[idx]);
		_maps[idx] = NULL;
	}
}

void MallocStatisticImpl::cleanup() {
	for (int i = 0; i < NR_OF_MAPS; ++i) {
		cleanup_for_map(i);
	}
}

void MallocStatisticImpl::resize_map(int map, int new_mask) {
	MallocStatisticEntry** new_map = (MallocStatisticEntry**) _funcs->calloc(new_mask + 1, sizeof(MallocStatisticEntry*));
	MallocStatisticEntry** old_map = _maps[map];

	// Fail silently if we don't get the memory.
	if (new_map != NULL) {
		for (int i = 0; i < _maps_mask[map]; ++i) {
			MallocStatisticEntry* entry = old_map[i];

			while (entry != NULL) {
				MallocStatisticEntry* next_entry = entry->next();
				int slot = entry->hash() & new_mask;
				entry->set_next(new_map[slot]);
				new_map[slot] = entry;
				entry = next_entry;
			}
		}

		_maps[map] = new_map;
		_maps_mask[map] = new_mask;
		_maps_limit[map] = (int) ((_maps_mask[map] + 1) * MAX_LOAD);
		_funcs->free(old_map);
	}
}

void MallocStatisticImpl::dump_entry(outputStream* st, MallocStatisticEntry* entry) {
	st->print_cr("Allocated bytes: " UINT64_FORMAT, (uint64_t) entry->size());
	st->print_cr("Allocated object: " UINT64_FORMAT, (uint64_t) entry->nr_of_allocations());
	st->print_raw_cr("Stack: ");

	char tmp[256];

	for (int i = 0; i < entry->nr_of_frames(); ++i) {
		st->print_raw("    ");

		if (os::print_function_and_library_name(st, entry->frames()[i], tmp, sizeof(tmp), true, true, false)) {
			st->cr();
		} else {
			st->print_raw_cr("<compiled code>");
		}
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

		for (int i = 0; i < NR_OF_MAPS; ++i) {
			if (pthread_mutex_init(&_maps_lock[i]._lock, NULL) != 0) {
				fatal("Could not initialize lock");
			}
		}
	}
}

bool MallocStatisticImpl::enable(outputStream* st, int stack_depth) {
	initialize(st);
	PthreadLocker lock(&_malloc_stat_lock._lock);

	if (_enabled) {
		st->print_raw_cr("malloc statistic is already enabled!");

		return false;
	}

	if (_shutdown) {
		st->print_raw_cr("malloc statistic is already shut down!");

		return false;
	}

	_track_free = false;
	_max_frames = stack_depth;
	_funcs = setup_hooks(&_malloc_stat_hooks, st);
	_entry_size = sizeof(MallocStatisticEntry) + sizeof(address) * (_max_frames - 1);

	for (int i = 0; i < NR_OF_MAPS; ++i) {
		void* mem = _funcs->malloc(sizeof(SafeAllocator));

		if (mem == NULL) {
			st->print_raw_cr("Could not allocate the allocator");
			cleanup();

			return false;
		}

		_allocators[i] = new (mem) SafeAllocator(sizeof(MallocStatisticEntry) + sizeof(address) * (_max_frames - 1), _funcs);

		_maps_mask[i] = 127;
		_maps_size[i] = 0;
		_maps_limit[i] = (int) ((_maps_mask[i] + 1) * MAX_LOAD);
		_maps[i] = (MallocStatisticEntry**) _funcs->calloc(_maps_mask[i] + 1, sizeof(MallocStatisticEntry*));

		if (mem == NULL) {
			st->print_raw_cr("Could not allocate the map");
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

void MallocStatisticImpl::create_statistic(bool on_error, size_t* size_bins, size_t* allocation_bins) {
	PthreadLocker lock(on_error ? NULL : &_malloc_stat_lock._lock);

	for (int i = 0; i < 63; ++i) {
		size_bins[i] = 0;
		allocation_bins[i] = 0;
	}

	for (int idx = 0; idx < NR_OF_MAPS; ++idx) {
		PthreadLocker lock2(on_error ? NULL : &_maps_lock[idx]._lock);

		for (int slot = 0; slot <= _maps_mask[idx]; ++slot) {
			MallocStatisticEntry* entry = _maps[idx][slot];

			while (entry != NULL) {
				size_bins[fast_log2((unsigned long long) entry->size())] += 1;
				allocation_bins[fast_log2((unsigned long long) entry->nr_of_allocations())] += 1;
				entry = entry->next();
			}
		}
	}
}

bool MallocStatisticImpl::dump(outputStream* st, bool on_error) {
	if (!on_error) {
		initialize(st);
	}

	size_t total_size = 0;
	size_t total_allocations = 0;
	size_t total_stacks = 0;

	pthread_setspecific(_malloc_suspended, (void*) 1);

	size_t size_bins[64];
	size_t allocation_bins[64];
	create_statistic(on_error, size_bins, allocation_bins);

	{
		PthreadLocker lock(on_error ? NULL : &_malloc_stat_lock._lock);

		if (!_enabled) {
			st->print_raw_cr("malloc statistic not enabled!");
			return false;
		}

		for (int idx = 0; idx < NR_OF_MAPS; ++idx) {
			PthreadLocker lock2(on_error ? NULL : &_maps_lock[idx]._lock);

			for (int slot = 0; slot <= _maps_mask[idx]; ++slot) {
				MallocStatisticEntry* entry = _maps[idx][slot];

				while (entry != NULL) {
					total_size += entry->size();
					total_allocations += entry->nr_of_allocations();
					total_stacks += 1;
					dump_entry(st, entry);
					entry = entry->next();
				}
			}
		}
	}

	st->print_cr("Total allocation size      : " UINT64_FORMAT, (uint64_t) total_size);
	st->print_cr("Total number of allocations: " UINT64_FORMAT, (uint64_t) total_allocations);
	st->print_cr("Total unique stacks        : "  UINT64_FORMAT, (uint64_t) total_stacks);

	for (int i = 0; i < 64; ++i) {
		st->print_cr("sizes " UINT64_FORMAT " -> " UINT64_FORMAT ": " UINT64_FORMAT, ((uint64_t) 1) << i,
			((uint64_t) 1) << (i + 1), (uint64_t) size_bins[i]);
		st->print_cr("allocationss " UINT64_FORMAT " -> " UINT64_FORMAT ": " UINT64_FORMAT, 
			((uint64_t) 1) << i, ((uint64_t) 1) << (i + 1), (uint64_t) allocation_bins[i]);
	}

	pthread_setspecific(_malloc_suspended, NULL);

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

bool MallocStatistic::enable(outputStream* st, int stack_depth) {
	return MallocStatisticImpl::enable(st, stack_depth);
}

bool MallocStatistic::disable(outputStream* st) {
	return MallocStatisticImpl::disable(st);
}

bool MallocStatistic::reset(outputStream* st) {
	return MallocStatisticImpl::reset(st);
}

bool MallocStatistic::dump(outputStream* st, bool on_error) {
	return MallocStatisticImpl::dump(st, on_error);
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
        _stack_depth("-stack-depth", "The maximum stack depth to track", "INT", false, "5") {
	_dcmdparser.add_dcmd_argument(&_cmd);
	_dcmdparser.add_dcmd_option(&_stack_depth);
}

void MallocStatisticDCmd::execute(DCmdSource source, TRAPS) {
	const char* const cmd = _cmd.value();

	if (strcmp(cmd, "enable") == 0) {
		if (MallocStatistic::enable(_output, (int) _stack_depth.value())) {
			_output->print_raw_cr("mallocstatistic enabled");
		}
	} else if (strcmp(cmd, "disable") == 0) {
		if (MallocStatistic::disable(_output)) {
			_output->print_raw_cr("mallocstatistic disabled");
		}
	} else if (strcmp(cmd, "reset") == 0) {
		MallocStatistic::reset(_output);
	} else if (strcmp(cmd, "dump") == 0) {
		MallocStatistic::dump(_output, false);
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
	} else {
		_output->print_cr("Unknown command '%s'", cmd);
	}
}

}

