/*
 * Copyright (c) 2022 SAP SE. All rights reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
 *
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

#include "precompiled.hpp"

#include "jvm_io.h"
#include "vitals_linux_himemreport.hpp"
#include "vitals_linux_oswrapper.hpp"
#include "logging/log.hpp"
#include "memory/allStatic.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/os.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "runtime/vm_version.hpp"
#include "services/memBaseline.hpp"
#include "services/memReporter.hpp"
#include "services/memTracker.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"
#include "vitals/vitals_internals.hpp"

#include <unistd.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

// Newer JDKS: NMT is always on and this macro does not exist
// Older JDKs: NMT can be off at compile time; in that case INCLUDE_NMT
//  will be defined=0 via CFLAGS; or on, in that case it will be defined=1 in macros.hpp.
#ifndef INCLUDE_NMT
#define INCLUDE_NMT 1
#endif

// Logging and output:
// We log during initialization phase to UL using the "vitals" tag.
// In the high memory detection thread itself, when triggering the report, we write strictly to
// stderr, directly. We don't use tty since we want to bypass ttylock. Sub command output also
// gets written to stderr.

// We print to the stderr stream directly in this code (since we want to bypass ttylock)
static fdStream stderr_stream(2);

namespace sapmachine_vitals {

static const int HiMemReportDecaySeconds = 60 * 5;

//////////// pretty printing stuff //////////////////////////////////

#define STRFTIME_FROM_TIME_T(st, fmt, t)                    \
  char buf[32];                                             \
  struct tm timeinfo;                                       \
  if (::localtime_r(&t, &timeinfo) != NULL &&               \
      ::strftime(buf, sizeof(buf), fmt, &timeinfo) != 0) {  \
    st->print_raw(buf);                                     \
  } else {                                                  \
    st->print_raw("unknown_date");                          \
  }


static void print_date_and_time(outputStream *st, time_t t) {
  STRFTIME_FROM_TIME_T(st, "%F %T", t);
}

// For use in file names
static void print_date_and_time_underscored(outputStream *st, time_t t) {
  STRFTIME_FROM_TIME_T(st, "%Y_%m_%d_%H_%M_%S", t);
}

static void print_current_date_and_time(outputStream *st) {
  time_t t;
  time(&t);
  print_date_and_time(st, t);
}

//////////// Alert state ////////////////////////////////////////////

class AlertState : public CHeapObj<mtInternal> {

  // this is 100%
  const size_t _maximum;

  // Alert percentages per level
  static const int _alvl_perc[5];

  // alert level: 0: all is well, 1..6: we are at x percent
  int _alvl;

  // time when alert level was increased last (for decay)
  time_t _last_avlv_increase;

  // We count spikes. A spike is a single increase to at least the lowest
  // alert level, followed by a reset because we recovered.
  int _spike_no;

  int calc_percentage(size_t size) const {
    return (int)((100.0f * (double)size)/(double)_maximum);
  }

  int calc_alvl(int percentage) const {
    int i = 0;
    while ((_alvl_perc[i + 1] != -1) && (_alvl_perc[i + 1] <= percentage)) {
      i ++;
    }
    return i;
  }

public:

  AlertState(size_t maximum) :
    _maximum(maximum), _alvl(0), _last_avlv_increase(0), _spike_no(0) {
    assert(_maximum > 0, "sanity");
  }

  size_t maximum() const {
    return _maximum;
  }

  int current_spike_no() const {
    return _spike_no;
  }

  int current_alert_level() const {
    return _alvl;
  }

  static int alert_level_percentage(int alvl) {
    assert(alvl >= 0 && alvl < (int)(sizeof(_alvl_perc) / sizeof(int)), "oob");
    return _alvl_perc[alvl];
  }

  int current_alert_level_percentage() const {
    return alert_level_percentage(_alvl);
  }

  // Update the state.
  // If we changed the alert level (either increased it or reset it after decay),
  // return true.
  bool update(size_t current_size) {
    const int percentage = calc_percentage(current_size);
    const int new_alvl = calc_alvl(percentage);
    // If we reached a new alert level, hold information and inform caller.
    if (new_alvl > _alvl) {
      // If we increased from zero, it means we entered a new spike, so
      // increase spike number
      if (_alvl == 0) {
        _spike_no ++;
      }
      _alvl = new_alvl;
      ::time(&_last_avlv_increase);
      return true;
    }
    // If all is well now, but we had an alert situation before, and enough
    // time has passed, reset alert level
    if (new_alvl == 0 && _alvl > 0) {
      time_t t;
      time(&t);
      if ((t - _last_avlv_increase) >= HiMemReportDecaySeconds) {
        _alvl = 0;
        _last_avlv_increase = 0;
        return true;
      }
    }
    return false;
  }

};

const int AlertState::_alvl_perc[5] = { 0, 66, 75, 90, -1 };

static AlertState* g_alert_state = NULL;

// What do we test?
enum class compare_type {
  compare_rss_vs_phys = 0,          // We compare rss+swap vs total physical memory
  compare_rss_vs_cgroup_limit = 1,  // We compare rss+swap vs the cgroup limit
  compare_rss_vs_manual_limit = 2,  // HiMemReportMaximum is set, we compare rss+swap with that limit
  compare_none
};

static compare_type g_compare_what = compare_type::compare_none;

static const char* describe_maximum_by_compare_type(compare_type t) {
  const char* s = "";
  switch (g_compare_what) {
  case compare_type::compare_rss_vs_cgroup_limit: s = "cgroup memory limit"; break;
  case compare_type::compare_rss_vs_phys: s = "the half of total physical memory"; break;
  case compare_type::compare_rss_vs_manual_limit: s = "HiMemReportMaximum"; break;
  default: ShouldNotReachHere();
  }
  return s;
}

//////////// NMT stuff //////////////////////////////////////////////

// NMT is nice, but the interface is unnecessary convoluted. For now, to keep merge surface small,
// we work with what we have

#if INCLUDE_NMT
class NMTStuff : public AllStatic {

  static MemBaseline _baseline;
  static time_t _baseline_time;

  // Fill a given baseline
  static void fill_baseline(MemBaseline& baseline) {
    const NMT_TrackingLevel lvl = MemTracker::tracking_level();
    if (lvl >= NMT_summary) {
      const bool summary_only = (lvl == NMT_summary);
      baseline.baseline(summary_only);
    }
  }

public:

  static bool is_enabled() {
    const NMT_TrackingLevel lvl = MemTracker::tracking_level();
    // Note: I avoid assumptions about numerical values of NMT_TrackingLevel
    // (e.g. "lvl >= NMT_summary") since their order changed over time and we
    // want to be JDK-version-agnostic here.
    return lvl == NMT_summary || lvl == NMT_detail;
  }

  // Capture a baseline right now
  static void capture_baseline() {
    fill_baseline(_baseline);
    time(&_baseline_time);
  }

  // Do the best possible report with the given NMT tracking level.
  // If we are at summary level, do a summary level report
  // If we are at detail level, do a detail level report
  // If we have a baseline captured, do a diff level report
  static void report_as_best_as_possible(outputStream* st) {

    if (NMTStuff::is_enabled()) {

      // Get the state now
      MemBaseline baseline_now;
      fill_baseline(baseline_now);

      // prepare and print suitable report
      if (_baseline.baseline_type() == baseline_now.baseline_type()) {
        // We already captured a baseline, and its type fits us (nobody changed NMT levels inbetween calls)
        time_t t;
        time(&t);
        st->print("(diff against baseline taken at ");
        print_date_and_time(st, _baseline_time);
        st->print_cr(", %d seconds ago)", (int)(t - _baseline_time));
        st->cr();
        const bool summary_only = (baseline_now.baseline_type() == MemBaseline::Summary_baselined);
        if (summary_only) {
          MemSummaryDiffReporter rpt(_baseline, baseline_now, st, K);
          rpt.report_diff();
        } else {
          MemDetailDiffReporter rpt(_baseline, baseline_now, st, K);
          rpt.report_diff();
        }
      } else {
        // We don't have a baseline yet. Just report the raw numbers
        const bool summary_only = (baseline_now.baseline_type() == MemBaseline::Summary_baselined);
        if (summary_only) {
          MemSummaryReporter rpt(baseline_now, st, K);
          rpt.report();
        } else {
          MemDetailReporter rpt(baseline_now, st, K);
          rpt.report();
        }
      }
    } else {
      st->print_cr("NMT is disabled, nothing to print");
    }

  }

  // If the situation calmed down, reset (clear the base line)
  static void reset() {
    _baseline_time = 0;
    _baseline.reset();
  }

};

MemBaseline NMTStuff::_baseline;
time_t NMTStuff::_baseline_time = 0;
#endif // INCLUDE_NMT

//////////// Reporting //////////////////////////////////////////////

class ReportDir : public CHeapObj<mtInternal> {
  // absolute, always ends with slash
  stringStream _dir;

public:

  const char* path() const { return _dir.base(); }

  ReportDir(const char* d) {
    assert(d != NULL && strlen(d) > 0, "sanity");
    if (d[0] != '/') { // relative?
      char* p = ::get_current_dir_name();
      _dir.print("%s/", p);
      ::free(p); // [sic]
    }
    _dir.print_raw(d);
    const size_t l = ::strlen(d);
    if (d[l - 1] != '/') {
      _dir.put('/');
    }
  }

  bool create_if_needed() {
    // Create the report directory (just the leaf dir, I don't bother creating the whole hierarchy)
    struct stat s;
    if (::stat(path(), &s) == -1) {
      if (::mkdir(path(), S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) != -1) {
        log_info(vitals)("HiMemReportDir: Created report directory \"%s\"", path());
      } else {
        log_warning(vitals)("HiMemReportDir: Failed to create report directory \"%s\" (%d)", path(), errno);
        return false;
      }
    } else {
      if (S_ISDIR(s.st_mode)) {
        log_info(vitals)("HiMemReportDir: Found existing report directory at \"%s\"", path());
      } else {
        log_warning(vitals)("HiMemReportDir: \"%s\" exists, but its not a directory.", path());
        return false;
      }
    }
    // Test access by touching a file in this dir. For convenience, we leave the touched file in it
    // and write the VM start time and some other info into it.
    stringStream testfile;
    testfile.print("%sVM_start.pid%d.log", path(), os::current_process_id());
    fileStream fs(testfile.base());
    if (!fs.is_open()) {
      log_warning(os)("HiMemReportDir: Cannot write to \"%s\" (%d)", testfile.base(), errno);
      return false;
    }
    print_current_date_and_time(&fs);
    return true;
  }
};

static ReportDir* g_report_dir = NULL;

static void print_high_memory_report_header(outputStream* st, const char* message, int pid, time_t t) {
  char tmp[255];
  st->print_cr("############");
  st->print_cr("#");
  st->print_cr("# High Memory Report:");
  st->print_cr("# pid: %d thread id: " INTX_FORMAT, pid, os::current_thread_id());
  st->print_cr("# %s", message);
  st->print_raw("# "); print_date_and_time(st, t); st->cr();
  st->print_cr("# Spike number: %d", g_alert_state->current_spike_no());
  st->print_cr("#");
  st->flush();
}

static void print_high_memory_report(outputStream* st) {

  // Note that this report may be interrupted by VM death, e.g. OOM killed.
  // Therefore we frequently flush, and print the most important things first.

  char buf[O_BUFLEN];

  st->print_cr("vm_info: %s", VM_Version::internal_vm_info_string());

  st->cr();
  st->cr();
  st->flush();

  Arguments::print_summary_on(st);
  st->cr();
  st->cr();
  st->flush();

  st->print_cr("--- Vitals ---");
  sapmachine_vitals::print_info_t info;
  sapmachine_vitals::default_settings(&info);
  info.sample_now = true;
  info.no_legend = true;
  sapmachine_vitals::print_report(st, &info);
  st->print_cr("--- /Vitals ---");

  st->cr();
  st->cr();
  st->flush();

#if INCLUDE_NMT
  st->cr();
  st->print_cr("--- NMT report ---");
  NMTStuff::report_as_best_as_possible(st);
  st->print_cr("--- /NMT report ---");
#endif

  st->cr();
  st->cr();
  st->flush();

  st->print_cr("#");
  st->print_cr("# END: High Memory Report");
  st->print_cr("#");

  st->flush();
}

// Create a file name into the report directory: <reportdir or cwd>/<name>.<pid>_<spike>_<percentage>.<suffix>
// (leave dir NULL to just get a file name)
static void print_file_name(stringStream* ss, const char* name, int pid, time_t timestamp, const char* suffix) {
  const char* dir = g_report_dir != NULL ? g_report_dir->path() : NULL;
  if (dir != NULL) {
    if (dir[0] != '/') {
      char* cwd = ::get_current_dir_name(); // glibc speciality, return ptr is malloced
      ss->print("%s/", cwd);
      ::free(cwd); // yes, use raw free here
    }
    ss->print("%s", dir);
    if (dir[::strlen(dir) - 1] != '/') {
      ss->put('/');
    }
  }
  ss->print("%s_pid%d_", name, pid);
  print_date_and_time_underscored(ss, timestamp);
  ss->print("%s", suffix);
}

///////////////////// JCmd support //////////////////////////////////////////

class ParsedCommand {

  stringStream _name; // command name without args
  stringStream _args; // arguments

public:

  ParsedCommand(const char* command) {
    // trim front
    const char* p = command;
    while (isspace(*p)) {
      p ++;
    }
    if ((*p) != '\0') {
      // read name
      while (!isspace(*p) && (*p) != '\0') {
        _name.put(*p);
        p++;
      }
      // find start of args; read args
      while (isspace(*p)) {
        p ++;
      }
      _args.print_raw(p);
    }
  }

  bool is_empty() const { return _name.size() == 0; }
  const char* name() const { return _name.base(); }

  bool has_arguments() const { return _args.size() > 0; }
  const char* args() const { return _args.base(); }

  // Unfortunately, the DCmd framework lacks the ability to check DCmd without
  // executing them. Here, we do some simple basic checks. Failing them will
  // exit the VM right away, but passing them does still not mean the command
  // is well formed since we don't check the arguments.
  bool is_valid() const {
    static const char* valid_prefixes[] = { "Compiler", "GC", "JFR", "JVMTI",
                                            "Management", "System", "Thread",
                                            "VM",  "help", NULL };
    if (_name.size() > 0) {
      for (const char** p = valid_prefixes; (*p) != NULL; p ++) {
        if (::strncasecmp(_name.base(), *p, ::strlen(*p)) == 0) {
          return true;
        }
      }
    }
    return false;
  }
};

// Helper structures for posix_spawn_file_actions_t and posix_spawnattr_t where
// cleanup depends on successful initialization.
// Helper structures for posix_spawn_file_actions_t and posix_spawnattr_t where
// cleanup depends on successful initialization.
struct PosixSpawnFileActions {
  posix_spawn_file_actions_t v;
  const bool ok;
  PosixSpawnFileActions() : ok(::posix_spawn_file_actions_init(&v) == 0) {}
  ~PosixSpawnFileActions() { ok && ::posix_spawn_file_actions_destroy(&v); }
};

struct PosixSpawnAttr {
  posix_spawnattr_t v;
  const bool ok;
  PosixSpawnAttr() : ok(::posix_spawnattr_init(&v) == 0) {}
  ~PosixSpawnAttr() { ok && ::posix_spawnattr_destroy(&v); }
};

// Call jcmd. If outFile and errFile are not Null, redirect stdout and stderr, otherwise
// print both stdout and stderr to VMs stderr.
// Returns true if command was executed successfully and exitcode was 0, false otherwise.
// If command failed, err_msg will contain an error string.
static bool spawn_command(const char** argv, const char* outFile, const char* errFile, stringStream* err_msg) {

  // I want vfork, but use posix_spawn, since vfork() is becoming obsolete and compilers
  // will warn. Its also safer, and with modern glibcs it is as cheap as vfork.
  PosixSpawnFileActions fa;
  PosixSpawnAttr atr;

  bool rc = fa.ok && atr.ok;

  if (outFile != NULL) { // Redirect stdout, stderr to files
        assert(errFile != NULL, "Require both");
    rc = rc && (::posix_spawn_file_actions_addopen(&fa.v, 1, outFile, O_WRONLY | O_CREAT | O_TRUNC, 0664) == 0) &&
               (::posix_spawn_file_actions_addopen(&fa.v, 2, errFile, O_WRONLY | O_CREAT | O_TRUNC, 0664) == 0);
  } else { // Dup stdout to stderr
    rc = rc && (::posix_spawn_file_actions_adddup2 (&fa.v, 2, 1) == 0);
  }
  pid_t child_pid = -1;

  // Hint toward vfork. Note that newer glibcs (2.24+) will ignore this, but they use clone(),
  // so its alright.
  rc = rc && (posix_spawnattr_setflags(&atr.v, POSIX_SPAWN_USEVFORK) == 0);

  if (rc == false) {
    err_msg->print("Error during posix_spawn setup");
    return false;
  }

  // Note about inheriting file descriptors: in theory, posix_spawn should close all stray descriptors:
  // "If file_actions is a null pointer, then file descriptors open in the calling process shall remain open
  //  in the child process, except for those whose close-on- exec flag FD_CLOEXEC is set (see fcntl)."
  // (https://pubs.opengroup.org/onlinepubs/9699919799/functions/posix_spawnp.html)
  // - which I assume means they get closed if we specify a file actions object, which we do.
  rc = rc && (::posix_spawn(&child_pid, argv[0], &fa.v, &atr.v, (char**)argv, os::get_environ()) == 0);

  if (rc) {
    int status;
    rc = (::waitpid(child_pid, &status, 0) != -1) &&
         (WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (rc == false) {
      err_msg->print("Command failed or crashed");
    }
  } else {
    err_msg->print("posix_spawn failed (%s)", os::strerror(errno));
  }

  return rc;
}

// Calls a single jcmd via posix_spawn. Output is written to <report-dir>/<command name>-<pid>-<spike>-<percentage>
// if HiMemReportDir is given; to stdout if not.
static void call_single_jcmd(const ParsedCommand* cmd, int pid, time_t t) {

  // if report dir is given, calc .out and .err file names
  const char* out_file = NULL;
  const char* err_file = NULL;
  stringStream out_file_ss, err_file_ss;
  if (g_report_dir != NULL) {
    // output files are named <command name>_pid<pid>_<timestamp>.(out|err), e.g. "VM.info_4711_2022_08_01_07_52_22.out".
    print_file_name(&out_file_ss, cmd->name(), pid, t, ".out");
    out_file = out_file_ss.base();
    print_file_name(&err_file_ss, cmd->name(), pid, t, ".err");
    err_file = err_file_ss.base();
  }

  stringStream jcmd_executable;
  jcmd_executable.print("%s/bin/jcmd", Arguments::get_java_home());

  stringStream target_pid;
  target_pid.print("%d", pid);

  stringStream jcmd_command;
  jcmd_command.print_raw(cmd->name());
  if (cmd->has_arguments()) {
    jcmd_command.put(' ');
    jcmd_command.print_raw(cmd->args());
  }

  // Special consideration for GC.heap_dump: if the command was given without arguments, we append
  // a file name for the heap dump ("<reportdir>/heapdump_pid<pid>_<timestamp>.dump")
  if (!cmd->has_arguments() && ::strcmp(cmd->name(), "GC.heap_dump") == 0) {
    jcmd_command.put(' ');
    print_file_name(&jcmd_command, "GC.heap_dump", pid, t, ".dump");
  }

  const char* argv[4];
  argv[0] = jcmd_executable.base();
  argv[1] = target_pid.base();
  argv[2] = jcmd_command.base();
  argv[3] = NULL;

  stringStream err_msg;
  const jlong t1 = os::javaTimeNanos();
  if (spawn_command(argv, out_file, err_file, &err_msg)) {
    const jlong t2 = os::javaTimeNanos();
    const int command_time_ms = (t2 - t1) / (1000 * 1000);
    stderr_stream.print("HiMemReport: Successfully executed \"%s\" (%d ms)", jcmd_command.base(), command_time_ms);
    if (out_file != NULL) {
      stderr_stream.print(", output redirected to report dir");
    }
    stderr_stream.cr();
  } else {
    stderr_stream.print("HiMemReport: Failed to execute \"%s\" (%s)", jcmd_command.base(), err_msg.base());
  }
}

// Helper, trims string
static char* trim_string(char* s) {
  char* p = s;
  while (::isspace(*p)) p++;
  char* p2 = p + ::strlen(p) - 1;
  while (p2 > p && ::isspace(*p2)) {
    *p2 = '\0';
    p2--;
  }
  return p;
}

struct JcmdClosure {
  virtual bool do_it(const char* cmd) = 0;
};

static bool iterate_exec_string(const char* exec_string, JcmdClosure* closure) {
  char* exec_copy = os::strdup(exec_string);
  char* save = NULL;
  for (char* tok = strtok_r(exec_copy, ";", &save);
       tok != NULL; tok = ::strtok_r(NULL, ";", &save)) {
    const char* p = trim_string(tok);
    if (::strlen(p) > 0 && !closure->do_it(p)) {
      os::free(exec_copy);
      return false;
    }
  }
  os::free(exec_copy);
  return true;
}

class CallJCmdClosure : public JcmdClosure {
  const pid_t _pid;
  const time_t _time;
public:
  CallJCmdClosure(int pid, time_t time) : _pid(pid), _time(time) {}
  bool do_it(const char* command_string) override {
    ParsedCommand cmd(command_string);
    assert(cmd.is_valid(), "Invalid command");
    call_single_jcmd(&cmd, _pid, _time);
    return true;
  }
};

struct VerifyJCmdClosure : public JcmdClosure {
  bool do_it(const char* command_string) override {
    log_info(vitals)("HiMemReportExec: storing command \"%s\".", command_string);
    if (!ParsedCommand(command_string).is_valid()) {
      // We print a warning here, fingerpointing the specific command that failed, then exit the VM later.
      log_warning(vitals)("HiMemReportExec: Command \"%s\" invalid.", command_string);
      return false;
    }
    return true;
  }
};

//////////////////// alert handling and reporting ///////////////////////////////////////////////////////////////

static int g_num_alerts = 0;

// We don't want to flood the report directory if the footprint of the VM wobbles strongly. We will give up
// after a reasonable amount of reports have been printed.
static const int max_spikes = 32;

static void trigger_high_memory_report(int alvl, int spikeno, int percentage, size_t triggering_size) {

  if (spikeno >= max_spikes) {
    if (spikeno == max_spikes) {
      stderr_stream.print_cr("# HiMemReport: Too many spikes encountered. Further reports will be omitted.");
    }
    return;
  }

  g_num_alerts ++;

  stringStream reason;
  reason.print("rss+swap (" SIZE_FORMAT " K) larger than %d%% of %s (" SIZE_FORMAT " K).",
               triggering_size / K, percentage, describe_maximum_by_compare_type(g_compare_what),
               g_alert_state->maximum() / K);
  const char* message = reason.base();

  const int pid = os::current_process_id();
  time_t t;
  time(&t);

  bool printed = false;

  print_high_memory_report_header(&stderr_stream, message, pid, t);

  if (g_report_dir != NULL) {
    // Dump to file in report dir
    stringStream ss;
    print_file_name(&ss, "sapmachine_himemalert", pid, t, ".log");
    fileStream fs(ss.base());
    if (fs.is_open()) {
      stderr_stream.print_cr("# Printing to %s", ss.base());
      print_high_memory_report_header(&fs, message, pid, t);
      print_high_memory_report(&fs);
      printed = true;
    } else {
      stderr_stream.print_cr("# Failed to open %s. Printing to stderr instead.", ss.base());
      stderr_stream.cr();
    }
    stderr_stream.flush();
  }

  if (!printed) {
    print_high_memory_report(&stderr_stream);
  }

  stderr_stream.print_cr("# Done.");
  stderr_stream.print_raw("#");
  stderr_stream.cr();
  stderr_stream.flush();

  if (HiMemReportExec != NULL) {
    CallJCmdClosure closure(pid, t);
    iterate_exec_string(HiMemReportExec, &closure);
  }

}

///////////////// Monitor thread /////////////////////////////////////////

void pulse_himem_report() {
  assert(HiMemReport, "only call for +HiMemReport");
  assert(g_compare_what != compare_type::compare_none && g_alert_state != NULL, "Not initialized");

  OSWrapper::update_if_needed();

  const value_t rss = OSWrapper::proc_rss_all();
  const value_t swap = OSWrapper::proc_swdo();
  if (rss != INVALID_VALUE && swap != INVALID_VALUE) {
    const size_t rss_swap = (size_t)rss + (size_t)swap;
    const int old_alvl = g_alert_state->current_alert_level();
    g_alert_state->update(rss_swap);
    const int new_alvl = g_alert_state->current_alert_level();
    const int spikeno = g_alert_state->current_spike_no();

    if (new_alvl > old_alvl) {
      const int new_percentage = g_alert_state->current_alert_level_percentage();
      stderr_stream.print_cr("HiMemoryReport: rss+swap=" SIZE_FORMAT " K - alert level increased to %d (>=%d%%).",
                              rss_swap / K, new_alvl, new_percentage);
      int skipped = 0;
      for (int i = old_alvl + 1; i < new_alvl; i ++) {
        skipped ++;
        // We may have missed some intermediary steps because the pulse interval was too large.
        stderr_stream.print_cr("HiMemoryReport: ... seems we passed alert level %d (%d%%) without noticing.",
                                i, AlertState::alert_level_percentage(i));
      }
      // If the alert level increased to a new value, trigger a new report
      trigger_high_memory_report(new_alvl, spikeno, new_percentage, rss_swap);
#if INCLUDE_NMT
      // Upon first alert, do a NMT baseline
      if (old_alvl == 0 && new_alvl > 0) {
        if (NMTStuff::is_enabled()) {
          NMTStuff::capture_baseline();
          stderr_stream.print_cr("HiMemoryReport: ... captured NMT baseline");
        }
      }
#endif // INCLUDE_NMT
    } else if (old_alvl > 0 && new_alvl == 0){
      // Memory usage recovered, and we hit the decay time, and now all is well again.
      stderr_stream.print_cr("HiMemoryReport: rss+swap=" SIZE_FORMAT " K - seems we recovered. Resetting alert level.",
                             rss_swap / K);
#if INCLUDE_NMT
      NMTStuff::reset();
#endif
    }
  }
}

class HiMemReportThread: public NamedThread {

  static const int interval_seconds = 2;

public:

  HiMemReportThread() : NamedThread() {
    this->set_name("himem reporter");
  }

  virtual void run() {
    record_stack_base_and_size();
    for (;;) {
      pulse_himem_report();
      os::naked_sleep(interval_seconds * 1000);
    }
  }

};

static HiMemReportThread* g_reporter_thread = NULL;

static bool initialize_reporter_thread() {
  g_reporter_thread = new HiMemReportThread();
  if (g_reporter_thread != NULL) {
    if (os::create_thread(g_reporter_thread, os::os_thread)) {
      os::start_thread(g_reporter_thread);
    }
    return true;
  }
  return false;
}

///////////////// Externals //////////////////////////////////////////////

extern void initialize_himem_report_facility() {

  static bool initialized = false;
  assert(initialized == false, "HiMemReport already initialized");
  initialized = true;

  // Note:
  // unrecoverable errors:
  //  - errors the user can easily correct (bad arguments) cause exit right away
  //  - errors which are subject to environment and cannot be dealt with/are unpredictable
  //    cause facility to be disabled (with UL warning)

  assert(HiMemReport, "only call for +HiMemReport");

  assert(g_compare_what == compare_type::compare_none && g_alert_state == NULL, "Only initialize once");

  // Verify the exec string
  VerifyJCmdClosure closure;
  if (HiMemReportExec != NULL && iterate_exec_string(HiMemReportExec, &closure) == false) {
    vm_exit_during_initialization("Vitals HiMemReportExec: One or more Exec commands were invalid");
  }

  // We need to decide what we will compare with what. To do that, we get the current system values.
  // - If user manually specified a maximum, we will compare rss+swap with that maximum
  // - If we live inside a cgroup with a memory limit, we will compare process rss+swap vs this limit
  //   (snapshotted at VM start; maybe later we can react to dynamic limit changes, but for the moment I don't care)
  // - If we do not live in a cgroup, or in a cgroup with no limit, compare process rss+swap vs the
  //   physical memory of the machine.
  size_t limit = 0;
  if (HiMemReportMax != 0) {
    g_compare_what = compare_type::compare_rss_vs_manual_limit;
    limit = HiMemReportMax;
    log_info(vitals)("Vitals HiMemReport: Setting limit to HiMemReportMax (" SIZE_FORMAT " K).", limit / K);
  } else {
    OSWrapper::update_if_needed();
    if (OSWrapper::syst_cgro_lim() != INVALID_VALUE) {
      // limit against cgroup limit
      g_compare_what = compare_type::compare_rss_vs_cgroup_limit;
      limit = (size_t)OSWrapper::syst_cgro_lim();
      log_info(vitals)("Vitals HiMemReport: Setting limit to cgroup memory limit (" SIZE_FORMAT " K).", limit / K);
    } else if (OSWrapper::syst_phys() != INVALID_VALUE) {
      // limit against total physical memory
      g_compare_what = compare_type::compare_rss_vs_phys;
      limit = (size_t)OSWrapper::syst_phys() / 2;
      log_info(vitals)("Vitals HiMemReport: Setting limit to half of total physical memory (" SIZE_FORMAT " K).", limit / K);
    }
  }

  if (limit == 0) {
    log_warning(vitals)("Vitals HiMemReport: limit could not be established; will disable high memory reports "
                    "(specify -XX:HiMemReportMax=<size> to establish a manual limit).");
    FLAG_SET_ERGO(HiMemReport, false);
    return;
  }

  // HiMemReportDir:
  // We fix up the report directory when VM starts, so if its relative, it refers to the initial current directory.
  // If it cannot be established, we treat it as predictable argument error and exit the VM.
  if (HiMemReportDir != NULL && ::strlen(HiMemReportDir) > 0) {
    g_report_dir = new ReportDir(HiMemReportDir);
    if (!g_report_dir->create_if_needed()) {
      log_warning(vitals)("Vitals: Cannot access HiMemReportDir %s.", g_report_dir->path());
      vm_exit_during_initialization("Vitals HiMemReport: Failed to create or access HiMemReportDir \"%s\".", g_report_dir->path());
      return;
    }
  }

  g_alert_state = new AlertState(limit);

  if (!initialize_reporter_thread()) {
    log_warning(vitals)("Vitals HiMemReport: Failed to start monitor thread. Will disable.");
    FLAG_SET_ERGO(HiMemReport, false);
    return;
  }

  log_info(vitals)("Vitals: HiMemReport subsystem initialized.");

}

extern void print_himemreport_state(outputStream* st) {
  if (g_alert_state != NULL) {
    st->print("HiMemReport: monitoring rss+swap vs %s (" SIZE_FORMAT " K)",
              describe_maximum_by_compare_type(g_compare_what),
              g_alert_state->maximum() / K);
    if (g_alert_state->current_alert_level() == 0) {
      st->print(", all is well");
    } else {
      st->print(", current level: %d (%d%%)", g_alert_state->current_alert_level(),
                g_alert_state->current_alert_level_percentage());
    }
    st->print(", spikes: %d, alerts: %d", g_alert_state->current_spike_no(), g_num_alerts);
  } else {
    st->print("HiMemReport: not monitoring.");
  }
}

// For printing in thread lists only.
extern const Thread* himem_reporter_thread() { return g_reporter_thread; }

} // namespace sapmachine_vitals

