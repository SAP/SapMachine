#ifndef OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP
#define OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP

#include "services/diagnosticCommand.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace sap {

// The spec we use for configuring the dump.
struct DumpSpec {
  const char* _dump_file;
  const char* _sort;
  int         _size_fraction;
  int         _count_fraction;
  int         _max_entries;

  DumpSpec() :
    _dump_file(NULL),
    _sort(NULL),
    _size_fraction(100),
    _count_fraction(100),
    _max_entries(0) {
  }
};

// Traces where allocations take place. Sums up the allocations by stack and total
// size. It is cheaper than a full trace, since it doesn't have to record frees
// and doesn't have to store data for each individual allocation.
class MallocStatistic : public AllStatic {

public:

  // Called early to initialize the class.
  static void initialize();

  // Enables the tracing. Returns true if enabled.
  static bool enable(outputStream* st, int stack_depth, bool use_bracktrace, int to_track_mask);

  // Disables the tracing. Returns true if disabled.
  static bool disable(outputStream* st);

  // Resets the statistic.
  static bool reset(outputStream* st);

  // Dumps the statistic.
  static bool dump(outputStream* st, DumpSpec const& spec, bool on_error);

  // Shuts down the statistic on error.
  static void shutdown();
};


class MallocStatisticDCmd : public DCmdWithParser {
private:

  DCmdArgument<char*> _cmd;
  DCmdArgument<jlong> _stack_depth;
  DCmdArgument<bool>  _use_backtrace;
  DCmdArgument<jlong> _skip_allocations;
  DCmdArgument<char*> _dump_file;
  DCmdArgument<jlong> _size_fraction;
  DCmdArgument<jlong> _count_fraction;
  DCmdArgument<jlong> _max_entries;
  DCmdArgument<char*> _sort;

public:
  static int num_arguments() {
    return 9;
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


