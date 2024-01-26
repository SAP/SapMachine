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

#ifndef OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP
#define OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP

#include "services/diagnosticCommand.hpp"
#include "utilities/globalDefinitions.hpp"

#if defined(LINUX) || defined(__APPLE__)

class outputStream;

namespace sap {

// The spec we use for configuring the trace
struct TraceSpec {
  int _stack_depth;
  bool _use_backtrace;
  int  _only_nth;
  bool _force;
  bool _track_free;
  bool _detailed_stats;
  int _rainy_day_fund;

  TraceSpec() :
    _stack_depth(10),
    _use_backtrace(true),
    _only_nth(0),
    _force(false),
    _track_free(false),
    _detailed_stats(false),
    _rainy_day_fund(0) {
  }
};

// The spec we use for configuring the dump.
struct DumpSpec {
  const char* _dump_file;
  const char* _filter;
  int         _max_entries;
  bool        _hide_dump_allocs;
  bool        _on_error;
  bool        _sort_by_count;
  int         _dump_percentage;
  bool        _internal_stats;

  DumpSpec() :
    _dump_file(NULL),
    _filter(NULL),
    _max_entries(0),
    _hide_dump_allocs(true),
    _on_error(false),
    _sort_by_count(false),
    _dump_percentage(100),
    _internal_stats(false) {
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

  // Does the emergency dump.
  static void emergencyDump();

  // Shuts down the statistic on error.
  static void shutdown();
};

class MallocTraceEnableDCmd : public DCmdWithParser {
  DCmdArgument<jlong> _stack_depth;
  DCmdArgument<bool>  _use_backtrace;
  DCmdArgument<jlong> _only_nth;
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
  DCmdArgument<char*> _filter;
  DCmdArgument<jlong> _max_entries;
  DCmdArgument<jlong> _dump_percentage;
  DCmdArgument<bool>  _sort_by_count;
  DCmdArgument<bool>  _internal_stats;

public:
  static int num_arguments() {
    return 6;
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

#endif // defined(LINUX) || defined(__APPLE__)

#endif // OS_POSIX_MALLOCTRACE_MALLOCTRACE_HPP
