#ifndef OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP
#define OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP

#include "mallochook.h"
#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace sap {

// Traces where allocations take place. Sums up the allocations by stack and total
// size. It is cheaper than a full trace, since it doesn't have to record frees
// and doesn't have to store data for each individual allocation.
class MallocStatistic : public AllStatic {

public:

	// Called early to initialize the class.
	static bool initialize();

	// Enables the tracing. Returns true if enabled.
	static bool enable(outputStream* st);

	// Disables the tracing. Returns true if disabled.
	static bool disable(outputStream* st);

	// Resets the statistic.
	static void reset(outputStream* st);

	// Prints the statistic
	static void print(outputStream* st);
};

}

#endif


