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
// Class SafeOutputStream
//
//
//
//

// A output stream which can use the real allocation functions
// given by malloc hooks to avoid triggering the hooks.
class SafeOutputStream : public outputStream {

private:
	real_funcs_t* _funcs;
	char*         _buffer;
	size_t        _buffer_size;
	size_t        _used;
	bool          _failed;

public:
	// funcs contains the 'real' malloc functions and is optained when
	// initializing the malloc hooks.
	SafeOutputStream(real_funcs_t* funcs) :
		_funcs(funcs),
		_buffer(NULL),
		_buffer_size(0),
		_used(0),
		_failed(false) {
	}

	virtual ~SafeOutputStream();

	virtual void write(const char* c, size_t len);

	// Copies the buffer to the given stream.
	void copy_to(outputStream* st);
};

SafeOutputStream::~SafeOutputStream() {
	_funcs->free(_buffer);
}

void SafeOutputStream::write(const char* c, size_t len) {
	if (_failed) {
		return;
	}

	if (_used + len > _buffer_size) {
		size_t to_add = 10 * 1024 + _buffer_size / 2;

		if (to_add + _buffer_size < len) {
			to_add = len;
		}

		char* new_buffer = (char*) _funcs->realloc(_buffer, _buffer_size + to_add);

		if (new_buffer == NULL) {
			_failed = true;
		} else {
			_buffer_size += to_add;
		}
	}

	memcpy(_buffer + _used, c, len);
	_used += len;
}

void SafeOutputStream::copy_to(outputStream* st) {
	if (_buffer == NULL) {
		st->print_cr("<empty>");
	} else {
		st->write(_buffer, _buffer_size);
	}

	if (_failed) {
		st->cr();
		st->print_raw_cr("*** Error during writing. Output might be truncated.");
	}
}


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
	if (pthread_mutex_lock(_mutex) != 0) {
		fatal("Could not lock mutex");
	}
}

PthreadLocker::~PthreadLocker() {
	if (pthread_mutex_unlock(_mutex) != 0) {
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
class MallocStatisticEntry {
private:
	MallocStatisticEntry* _next;
	uint32_t              _hash;
	int                   _nr_of_frames;
	size_t                _size;
	size_t                _nr_of_allocations;
	address		      _frames[1];

public:
	MallocStatisticEntry(uint32_t hash, size_t size, int nr_of_frames, address* frames) :
		_next(NULL),
		_hash(hash),
		_nr_of_frames(nr_of_frames),
		_size(size),
		_nr_of_allocations(1) {
		memcpy(_frames, frames, sizeof(address) * nr_of_frames);
	}

	void add_allocation(size_t size) {
		_size += size;
		_nr_of_allocations += 1;
	}

	uint32_t hash() {
		return _hash;
	}

	size_t size() {
		return _size;
	}

	int nr_of_frames() {
		return _nr_of_frames;
	}

	address* frames() {
		return _frames;
	}

	MallocStatisticEntry* next() {
		return _next;
	}

	void set_next(MallocStatisticEntry* next) {
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


// A pthread mutex usable in arrays.
struct CacheLineSafeLock {
	pthread_mutex_t _lock;
	char            _pad[DEFAULT_CACHE_LINE_SIZE - sizeof(pthread_mutex_t)];
};

#define NR_OF_MAPS 16

class MallocStatisticImpl : public AllStatic {
private:

	static real_funcs_t*      _funcs;
	static volatile bool      _initialized;
	static bool               _enabled;
	static bool               _track_free;
	static int                _max_frames;
	static registered_hooks_t _malloc_stat_hooks;
	static CacheLineSafeLock  _malloc_stat_lock;
	static CacheLineSafeLock  _hash_map_locks[NR_OF_MAPS];
	static void*              _map[NR_OF_MAPS];
	static SafeAllocator*     _allocators[NR_OF_MAPS];

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

public:
	static void initialize(outputStream* st);

	static bool enable(outputStream* st);

	static bool disable(outputStream* st);

	static bool reset(outputStream* st);

	static bool dump(outputStream* st, bool on_error);

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

real_funcs_t*      MallocStatisticImpl::_funcs;
volatile bool      MallocStatisticImpl::_initialized;
bool               MallocStatisticImpl::_enabled;
bool               MallocStatisticImpl::_track_free;
int                MallocStatisticImpl::_max_frames;
CacheLineSafeLock  MallocStatisticImpl::_malloc_stat_lock;
CacheLineSafeLock  MallocStatisticImpl::_hash_map_locks[NR_OF_MAPS];
void*              MallocStatisticImpl::_map[NR_OF_MAPS];
SafeAllocator*     MallocStatisticImpl::_allocators[NR_OF_MAPS];


#define MAX_FRAMES 32

#define CAPTURE_STACK \
	address frames[MAX_FRAMES]; \
	int nr_of_frames = 0; \
	frame fr = os::current_frame(); \
	while (fr.pc() && nr_of_frames < _max_frames) { \
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

	if (result != NULL) {
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

	if (result != NULL) {
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

	if (result != NULL) {
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
	if ((ptr != NULL) &&_track_free) {
		CAPTURE_STACK;
		record_free(ptr, real_malloc_size(ptr), nr_of_frames, frames);
	}

	real_free(ptr);
}

int MallocStatisticImpl::posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size) {
	int result =  real_posix_memalign(ptr, align, size);

	if (result == 0) {
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

	if (result != NULL) {
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

	if (result != NULL) {
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

	if (result != NULL) {
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

	if (result != NULL) {
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

void MallocStatisticImpl::record_allocation_size(size_t to_add, int nr_of_frames, address* frames) {
	assert(!_track_free, "Only used for summary tracking");
}

void MallocStatisticImpl::record_allocation(void* ptr, size_t size, int nr_of_frames, address* frames) {
	assert(_track_free, "Only used for detailed tracking");
}

void MallocStatisticImpl::record_free(void* ptr, size_t size, int nr_of_frames, address* frames) {
	assert(_track_free, "Only used for detailed tracking");
}

void MallocStatisticImpl::initialize(outputStream* st) {
	if (!_initialized) {
		_initialized = true;

		if (pthread_mutex_init(&_malloc_stat_lock._lock, NULL) != 0) {
			fatal("Could not initialize lock");
		}

		for (int i = 0; i < NR_OF_MAPS; ++i) {
			if (pthread_mutex_init(&_hash_map_locks[i]._lock, NULL) != 0) {
				fatal("Could not initialize lock");
			}
		}
	}
}

bool MallocStatisticImpl::enable(outputStream* st) {
	initialize(st);
	PthreadLocker lock(&_malloc_stat_lock._lock);

	if (_enabled) {
		st->print_raw_cr("malloc statistic is already enabled!");

		return false;
	}

	_track_free = false;
	_max_frames = MAX_FRAMES;
	_funcs = setup_hooks(&_malloc_stat_hooks, st);

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

	setup_hooks(NULL, st);
	_funcs = NULL;
	_enabled = false;

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

bool MallocStatisticImpl::dump(outputStream* st, bool on_error) {
	if (!on_error) {
		initialize(st);

		if (pthread_mutex_lock(&_malloc_stat_lock._lock) != 0) {
			st->print_raw_cr("Could not dump because locking failed");

			return false;
		}
	}

	bool result = true;

	if (!_enabled) {
		st->print_raw_cr("malloc statistic not enabled!");
		result = false;
	}

	if (!on_error) {
		pthread_mutex_unlock(&_malloc_stat_lock._lock);
	}

	return result;
}




void MallocStatistic::initialize() {
	MallocStatisticImpl::initialize(NULL);
}

bool MallocStatistic::enable(outputStream* st) {
	return MallocStatisticImpl::enable(st);
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
	_suboption("suboption", "see option", "STRING", false) {
	_dcmdparser.add_dcmd_argument(&_cmd);
	_dcmdparser.add_dcmd_argument(&_suboption);
}

void MallocStatisticDCmd::execute(DCmdSource source, TRAPS) {
	const char* const cmd = _cmd.value();

	if (strcmp(cmd, "enable") == 0) {
		if (MallocStatistic::enable(_output)) {
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

