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
// Class MallocHooksSafeOutputStream
//
//
//
//

// A output stream which can use the real allocation functions
// given by malloc hooks to avoid triggering the hooks.
class MallocHooksSafeOutputStream : public outputStream {

private:
	real_funcs_t* _funcs;
	char*         _buffer;
	size_t        _buffer_size;
	size_t        _used;
	bool          _failed;

public:
	// funcs contains the 'real' malloc functions and is optained when
	// initializing the malloc hooks.
	MallocHooksSafeOutputStream(real_funcs_t* funcs) :
		_funcs(funcs),
		_buffer(NULL),
		_buffer_size(0),
		_used(0),
		_failed(false) {
	}

	virtual ~MallocHooksSafeOutputStream();

	virtual void write(const char* c, size_t len);

	// Copies the buffer to the given stream.
	void copy_to(outputStream* st);
};

MallocHooksSafeOutputStream::~MallocHooksSafeOutputStream() {
	_funcs->free(_buffer);
}

void MallocHooksSafeOutputStream::write(const char* c, size_t len) {
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

void MallocHooksSafeOutputStream::copy_to(outputStream* st) {
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
// Class MallocHooksSafeAllocator
//
//
//
//
class MallocHooksSafeAllocator {
private:

	real_funcs_t* _funcs;
	size_t        _allocation_size;
	int           _entries_per_chunk;
	void**        _chunks;
	int           _nr_of_chunks;
	void**        _free_list;

public:
	MallocHooksSafeAllocator(size_t allocation_size, real_funcs_t* funcs);
	~MallocHooksSafeAllocator();

	void* allocate();
	void free(void* ptr);
};

MallocHooksSafeAllocator::MallocHooksSafeAllocator(size_t allocation_size, real_funcs_t* funcs) :
	_funcs(funcs),
	_allocation_size(align_up(allocation_size, 8)), // We need no stricter alignment
	_entries_per_chunk(16384),
	_chunks(NULL),
	_nr_of_chunks(0),
	_free_list(NULL) {
}

MallocHooksSafeAllocator::~MallocHooksSafeAllocator() {
	for (int i = 0; i < _nr_of_chunks; ++i) {
		_funcs->free(_chunks[i]);
	}
}

void* MallocHooksSafeAllocator::allocate() {
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

void MallocHooksSafeAllocator::free(void* ptr) {
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
	static registered_hooks_t _malloc_stat_hooks;
	static CacheLineSafeLock  _malloc_stat_lock;
	static CacheLineSafeLock  _hash_map_locks[NR_OF_MAPS];

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

public:
	static void initialize(outputStream* st);

	static bool enable(outputStream* st);

	static bool disable(outputStream* st);

	static void reset(outputStream* st);

	static void print(outputStream* st);

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
CacheLineSafeLock  MallocStatisticImpl::_malloc_stat_lock;
CacheLineSafeLock  MallocStatisticImpl::_hash_map_locks[NR_OF_MAPS];

void* MallocStatisticImpl::malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) {
	void* result = real_malloc(size);

	return result;
}

void* MallocStatisticImpl::calloc_hook(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size) {
	return real_calloc(elems, size);
}

void* MallocStatisticImpl::realloc_hook(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size) {
	return real_realloc(ptr, size);
}

void MallocStatisticImpl::free_hook(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size) {
	real_free(ptr);
}

int MallocStatisticImpl::posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size) {
	return real_posix_memalign(ptr, align, size);
}

void* MallocStatisticImpl::memalign_hook(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size) {
	return real_memalign(align, size);
}

void* MallocStatisticImpl::aligned_alloc_hook(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size) {
	return real_aligned_alloc(align, size);
}

void* MallocStatisticImpl::valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size) {
	return real_valloc(size);
}

void* MallocStatisticImpl::pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size) {
	return real_pvalloc(size);
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

void MallocStatisticImpl::reset(outputStream* st) {
}

void MallocStatisticImpl::print(outputStream* st) {
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

void MallocStatistic::reset(outputStream* st) {
	MallocStatisticImpl::reset(st);
}

void MallocStatistic::print(outputStream* st) {
	MallocStatisticImpl::print(st);
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
	_option("option", "dummy", "STRING", true),
	_suboption("suboption", "see option", "STRING", false) {
	_dcmdparser.add_dcmd_argument(&_option);
	_dcmdparser.add_dcmd_argument(&_suboption);
}

void MallocStatisticDCmd::execute(DCmdSource source, TRAPS) {
	// For now just do some tests.
	real_funcs_t* funcs = setup_hooks(NULL, _output);

#define MAX_ALLOCS (1024 * 1024)

	static void* results[MAX_ALLOCS];

	for (int r = 0; r < 10; ++r) {

		for (int i = 0; i < MAX_ALLOCS; ++i) {
			results[i] = NULL;
		}

		MallocHooksSafeAllocator alloc(96, funcs);
		for (int i = 0; i < MAX_ALLOCS; ++i) {
			results[i] = alloc.allocate();
			alloc.free(results[(317 * (int64_t) i) & (MAX_ALLOCS - 1)]);
		}
	}

	MallocStatistic::enable(_output);
	MallocStatistic::disable(_output);
	_output->print_raw_cr("Test succeeded.");
}

}

