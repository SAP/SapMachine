#ifndef OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP
#define OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP

#include "services/diagnosticCommand.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace sap {

// The spec we use for configuring the trace
struct TraceSpec {
  int _stack_depth;
  bool _use_backtrace;
  int _skip_exp;
  bool _force;
  bool _track_free;
  bool _detailed_stats;

  TraceSpec() :
    _stack_depth(10),
    _use_backtrace(true),
    _skip_exp(0),
    _force(false),
    _track_free(false),
    _detailed_stats(false) {
  }
};

// The spec we use for configuring the dump.
struct DumpSpec {
  const char* _dump_file;
  const char* _sort;
  int         _size_fraction;
  int         _count_fraction;
  int         _max_entries;
  bool        _hide_dump_allocs;
  bool        _on_error;

  DumpSpec() :
    _dump_file(NULL),
    _sort(NULL),
    _size_fraction(100),
    _count_fraction(100),
    _max_entries(0),
    _hide_dump_allocs(true),
    _on_error(false) {
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
  static bool enable(outputStream* st, TraceSpec const& spec);

  // Disables the tracing. Returns true if disabled.
  static bool disable(outputStream* st);

  // Dumps the statistic.
  static bool dump(outputStream* st, DumpSpec const& spec);

  // Shuts down the statistic on error.
  static void shutdown();
};

class MallocTraceEnableDCmd : public DCmdWithParser {
  DCmdArgument<jlong> _stack_depth;
  DCmdArgument<bool>  _use_backtrace;
  DCmdArgument<jlong> _skip_allocations;
  DCmdArgument<bool>  _force;
  DCmdArgument<bool>  _track_free;
  DCmdArgument<bool>  _detailed_stats;

public:
  static int num_arguments() {
    return 6;
  }

  MallocTraceEnableDCmd(outputStream* output, bool heap);

  static const char* name() {
    return "MallocTrace.enable";
  }

  static const char* description() {
    return "Enables tracing memory allocations";
  }

  static const char* impact() {
    return "High";
  }

  static const JavaPermission permission() {
    JavaPermission p = { "java.lang.management.ManagementPermission", "control", NULL };
    return p;
  }

  virtual void execute(DCmdSource source, TRAPS);
};

class MallocTraceDisableDCmd : public DCmdWithParser {

public:
  static int num_arguments() {
    return 0;
  }

  MallocTraceDisableDCmd(outputStream* output, bool heap);

  static const char* name() {
    return "MallocTrace.disable";
  }

  static const char* description() {
    return "Disables tracing memory allocations";
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

class MallocTraceDumpDCmd : public DCmdWithParser {
private:

  DCmdArgument<char*> _dump_file;
  DCmdArgument<jlong> _size_fraction;
  DCmdArgument<jlong> _count_fraction;
  DCmdArgument<jlong> _max_entries;
  DCmdArgument<char*> _sort;

public:
  static int num_arguments() {
    return 5;
  }

  MallocTraceDumpDCmd(outputStream* output, bool heap);

  static const char* name() {
    return "MallocTrace.dump";
  }

  static const char* description() {
    return "Dumps the currently running malloc trace";
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


