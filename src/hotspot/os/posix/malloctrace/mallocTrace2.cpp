#include "precompiled.hpp"

#include "jvm_io.h"
#include "malloctrace/mallocTrace2.hpp"
#include "utilities/ostream.hpp"

#include <pthread.h>



namespace sap {

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
	_funcs->real_free(_buffer);
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

		char* new_buffer = (char*) _funcs->real_realloc(_buffer, _buffer_size + to_add);

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


class MallocStatisticImpl : public AllStatic {
private:

	static volatile bool      _initialized;
	static registered_hooks_t _malloc_stat_hooks;
	static pthread_mutex_t    _malloc_stat_lock;


	static void* malloc_hook(size_t size, void* caller_address, malloc_func_t* real_malloc, malloc_size_func_t real_malloc_size) {
		return real_malloc(size);
	}

	static void* calloc_hook(size_t elems, size_t size, void* caller_address, calloc_func_t* real_calloc, malloc_size_func_t real_malloc_size) {
		return real_calloc(elems, size);
	}

	static void* realloc_hook(void* ptr, size_t size, void* caller_address, realloc_func_t* real_realloc, malloc_size_func_t real_malloc_size) {
		return real_realloc(ptr, size);
	}

	static void free_hook(void* ptr, void* caller_address, free_func_t* real_free, malloc_size_func_t real_malloc_size) {
		real_free(ptr);
	}

	static int posix_memalign_hook(void** ptr, size_t align, size_t size, void* caller_address, posix_memalign_func_t* real_posix_memalign, malloc_size_func_t real_malloc_size) {
		return real_posix_memalign(ptr, align, size);
	}

	static void* memalign_hook(size_t align, size_t size, void* caller_address, memalign_func_t* real_memalign, malloc_size_func_t real_malloc_size) {
		return real_memalign(align, size);
	}

	static void* aligned_alloc_hook(size_t align, size_t size, void* caller_address, aligned_alloc_func_t* real_aligned_alloc, malloc_size_func_t real_malloc_size) {
		return real_aligned_alloc(align, size);
	}

	static void* valloc_hook(size_t size, void* caller_address, valloc_func_t* real_valloc, malloc_size_func_t real_malloc_size) {
		return real_valloc(size);
	}

	static void* pvalloc_hook(size_t size, void* caller_address, pvalloc_func_t* real_pvalloc, malloc_size_func_t real_malloc_size) {
		return real_pvalloc(size);
	}

public:
	static bool initialize(outputStream* st);

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

pthread_mutex_t MallocStatisticImpl::_malloc_stat_lock;
volatile bool MallocStatisticImpl::_initialized;

bool MallocStatisticImpl::initialize(outputStream* st) {
	if (_initialized) {
		return true;
	}

	if (pthread_mutex_init(&_malloc_stat_lock, NULL) != 0) {
		if (st != NULL) {
			st->print_raw_cr("Could not initialize pthread lock for malloc statistic!");
		}

		return false;
	}

	_initialized = true;
	return true;
}

bool MallocStatisticImpl::enable(outputStream* st) {
	if (!initialize(st)) {
		return false;
	}

	real_funcs_t* funcs = setup_hooks(&_malloc_stat_hooks, st);

	if (funcs == NULL) {
		return false;
	}

	return true;
}

bool MallocStatisticImpl::disable(outputStream* st) {
	if (!initialize(st)) {
		return false;
	}

	setup_hooks(NULL, st);

	return true;
}

void MallocStatisticImpl::reset(outputStream* st) {
}

void MallocStatisticImpl::print(outputStream* st) {
}




bool MallocStatistic::initialize() {
	return MallocStatisticImpl::initialize(NULL);
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

}

