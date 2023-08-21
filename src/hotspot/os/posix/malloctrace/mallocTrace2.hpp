#ifndef OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP
#define OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP

#include "services/diagnosticCommand.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace sap {

// Traces where allocations take place. Sums up the allocations by stack and total
// size. It is cheaper than a full trace, since it doesn't have to record frees
// and doesn't have to store data for each individual allocation.
class MallocStatistic : public AllStatic {

public:

	// Called early to initialize the class.
	static void initialize();

	// Enables the tracing. Returns true if enabled.
	static bool enable(outputStream* st);

	// Disables the tracing. Returns true if disabled.
	static bool disable(outputStream* st);

	// Resets the statistic.
	static void reset(outputStream* st);

	// Prints the statistic
	static void print(outputStream* st);
};


class MallocStatisticDCmd : public DCmdWithParser {
private:

	DCmdArgument<char*> _option;
	DCmdArgument<char*> _suboption;

public:
	static int num_arguments() {
		return 2;
	}

	MallocStatisticDCmd(outputStream* output, bool heap);

	static const char* name() {
		return "System.mallocstatistic";
	}

	static const char* description() {
		return "Trace malloc call sites";
	}

	static const char* impact() {
		return "Low";
	}

	static const JavaPermission permission() {
		JavaPermission p = { "java.lang.management.ManagementPermission", "control", NULL };
		return p;
	}

	virtual void execute(DCmdSource source, TRAPS);
};

}

#endif


