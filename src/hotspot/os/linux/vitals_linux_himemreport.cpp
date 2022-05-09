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
#include "services/memBaseline.hpp"
#include "services/memReporter.hpp"
#include "services/memTracker.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "vitals/vitals_internals.hpp"

#include <unistd.h>

// We print to the stderr stream directly in this code (since we want to bypass ttylock)
static fdStream stderr_stream(2);

#ifdef ASSERT
#define LOG_HERE_F(msg, ...)  { printf("[%d] ", (int)::getpid()); ::printf(msg, __VA_ARGS__); printf("\n"); fflush(stdout); }
#define LOG_HERE(msg)         { printf("[%d] ", (int)::getpid()); ::printf("%s", msg); printf("\n"); fflush(stdout); }
#else
#define LOG_HERE_F(msg, ...)
#define LOG_HERE(msg)
#endif

namespace sapmachine_vitals {


//////////// pretty printing stuff //////////////////////////////////

static void print_date_and_time(outputStream *st, time_t t) {
  char buf[32];
  struct tm* timeinfo;
  timeinfo = ::localtime(&t);
  size_t rc = ::strftime(buf, sizeof(buf), "%F %T", timeinfo);
  assert(rc < 32, "sanity strftime");
  st->print_raw(buf);
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
  static const int _alvl_perc[5]; // = { 0, 66, 75, 90, 100 };
  //                                     0  2   3   4,  5

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
    while (_alvl_perc[i] < 100 && percentage >= _alvl_perc[i + 1]) i ++;
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

const int AlertState::_alvl_perc[5] = { 0, 66, 75, 90, 100 };

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
  case compare_type::compare_rss_vs_phys: s = "total physical memory"; break;
  case compare_type::compare_rss_vs_manual_limit: s = "HiMemReportMaximum"; break;
  default: ShouldNotReachHere();
  }
  return s;
}

//////////// NMT stuff //////////////////////////////////////////////

// NMT is nice, but the interface is unnecessary convoluted. For now, to keep merge surface small,
// we work with what we have

class NMTStuff : public AllStatic {

  static MemBaseline _baseline;
  static time_t _baseline_time;

  // Fill a given baseline
  static bool fill_baseline(MemBaseline& baseline) {
    const NMT_TrackingLevel lvl = MemTracker::tracking_level();
    if (lvl >= NMT_summary) {
      const bool summary_only = (lvl == NMT_summary);
      if (baseline.baseline(summary_only)) {
        return true;
      }
    }
    return true;
  }

public:

  // Capture a baseline right now
  static bool capture_baseline() {
    if (fill_baseline(_baseline)) {
      time(&_baseline_time);
      return true;
    }
    return false;
  }

  // Do the best possible report with the given NMT tracking level.
  // If we are at summary level, do a summary level report
  // If we are at detail level, do a detail level report
  // If we have a baseline captured, do a diff level report
  static bool report_as_best_as_possible(outputStream* st) {

    const NMT_TrackingLevel lvl = MemTracker::tracking_level();
    if (lvl >= NMT_summary) {

      // Get the state now
      MemBaseline baseline_now;
      if (!fill_baseline(baseline_now)) {
        st->print_cr("failed.");
        return false;
      }

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

    return true;
  }

  // If the situation calmed down, reset (clear the base line)
  static void reset() {
    _baseline_time = 0;
    _baseline.reset();
  }

};

MemBaseline NMTStuff::_baseline;
time_t NMTStuff::_baseline_time = 0;


//////////// Reporting //////////////////////////////////////////////

class ReportDir : public CHeapObj<mtInternal> {
  // absolute, always ends with slash
  stringStream _dir;

public:

  const char* path() const { return _dir.base(); }

  ReportDir(const char* d) {
    assert(d != NULL && strlen(d) > 0, "sanity");
    if (HiMemReportDir[0] != '/') { // relative?
      char* p = ::get_current_dir_name();
      _dir.print("%s/", p);
      ::free(p); // [sic]
    }
    _dir.print_raw(d);
    const size_t l = ::strlen(HiMemReportDir);
    if (d[l - 1] != '/') {
      _dir.put('/');
    }
  }

  bool create_if_needed() {
    // Create the report directory (just the leaf dir, I don't bother creating the whole hierarchy)
    log_info(os)("create/validate HiMemReportDir \"%s\"...", path());
    struct stat s;
    if (::stat(path(), &s) == -1) {
      if (::mkdir(path(), S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) == -1) {
        log_warning(os)("HiMemReportDir: Failed to create report directory \"%s\" (%d)", path(), errno);
        return false;
      }
    } else if (!S_ISDIR(s.st_mode)) {
      log_warning(os)("HiMemReportDir: \"%s\" exists, but its not a directory.", path());
      return false;
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

static void print_high_memory_report_header(outputStream* st, const char* message) {
  char tmp[255];
  st->print_cr("############");
  st->print_cr("#");
  st->print_cr("# High Memory Report:");
  st->print_cr("# pid: %d thread id: " INTX_FORMAT, os::current_process_id(), os::current_thread_id());
  st->print_cr("# %s", message);
  st->print_raw("# "); print_current_date_and_time(st); st->cr();
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

  st->cr();
  st->print_cr("--- NMT report ---");
  NMTStuff::report_as_best_as_possible(st);
  st->print_cr("--- /NMT report ---");

  st->cr();
  st->cr();
  st->flush();

  st->print_cr("#");
  st->print_cr("# END: High Memory Report");
  st->print_cr("#");

  st->flush();
}

// Create a file name into the report directory: <dir>/<name>.pid<pid>_<spike>_<percentage>.<suffix>
// (leave dir NULL to just get a file name)
static void print_file_name(stringStream* ss, const char* dir, const char* name, int pid, int spikeno, int percentage, const char* suffix) {
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
  ss->print("%s_pid%d_%d_%d%s",
      name, pid, spikeno, percentage, suffix);
}

///////////////////// JCmd support //////////////////////////////////////////
//
// if the standard report is not sufficient, the VM can fire additional reports
// against itself.

class JCmdInfo : public CHeapObj<mtInternal> {
  stringStream _cmd;
  stringStream _args;
  const JCmdInfo* _next;
public:
  JCmdInfo() : _next(NULL) {}
  stringStream& cmdstream() {
    return _cmd;
  }
  stringStream& args_stream() {
    return _args;
  }
  const char* command() const { return _cmd.base(); }
  const char* args() const { return _args.base(); }
  const JCmdInfo* next() const { return _next; }
  void set_next(const JCmdInfo* cmd) {
    _next = cmd;
  }
};

class LilJcmdParser {
  JCmdInfo* _cur;
  JCmdInfo* _last;
  JCmdInfo* _first;
  enum { expect_cmd, read_cmd, read_args } _state;

  void finish() {
    if (_cur != NULL && _cur->cmdstream().size() > 0) {
      if (_first == NULL) {
        _first = _cur;
      }
      if (_last != NULL) {
        _last->set_next(_cur);
      }
      _last = _cur;
      _cur = NULL;
    }
  }

  void parse(const char* input) {
    assert(_first == NULL && _cur == NULL && _last == NULL && _state == expect_cmd,
           "just use once");
    const char* p = input;
    while (*p != '\0') {
      switch (_state) {
      case expect_cmd:
        if (*p != ';' && *p != ' ') {
          _state = read_cmd;
          _cur = new JCmdInfo();
          continue;
        }
        break;
      case read_cmd:
        if (*p == ' ') {
          _state = read_args;
        } else if (*p == ';') {
          finish();
          _state = expect_cmd;
        } else {
          _cur->cmdstream().put(*p);
        }
        break;
      case read_args:
        if (*p == ';') {
          finish();
          _state = expect_cmd;
        } else {
          _cur->args_stream().put(*p);
        }
        break;
      default: {
        ShouldNotReachHere();
        }
      }
      p++;
    }
    finish();
  }

  LilJcmdParser(const char* input) :
    _cur(NULL), _last(NULL),
    _first(NULL), _state(expect_cmd) {
    parse(input);
  }

public:

  const JCmdInfo* first() const { return _first; }

  static const JCmdInfo* parse_exec_string(const char* input) {
    LilJcmdParser parser(input);
    return parser.first();
  }
};

static const JCmdInfo* g_jcmds = NULL;

static void call_jcmds(int spikeno, int percentage) {
  assert(HiMemReportExec != NULL && g_report_dir != NULL, "Sanity");

  const int pid = os::current_process_id();
  const char* java_home = Arguments::get_java_home();

  for (const JCmdInfo* Cmd = g_jcmds; Cmd != NULL; Cmd = Cmd->next()) {
    const char* cmd = Cmd->command();
    const char* args = Cmd->args();

    // Assemble command
    stringStream ss;
    ss.print("/bin/bash -c 'cd %s; %s/bin/jcmd %d ", g_report_dir->path(), java_home, pid);

    // Handle some special cases:
    if (::strcmp(cmd, "heapdump") == 0) {
      // "heapdump" : do a heap dump with the correctly named dump file (path needs to be absolute)
      stringStream heapdumpFile;
      print_file_name(&heapdumpFile, g_report_dir->path(), "heapdump", pid, spikeno, percentage, ".dump");
      ss.print("GC.heap_dump -gz=1 %s", heapdumpFile.base());
    } // .... Add more if needed
    else {
      // Generic case
      ss.print("%s %s", cmd, args);
    }
    // We dump jcmd stdout stderr  to .out and .err files respectively (relative path is fine since child will
    // cd into the report dir before)
    ss.print(" > ");
    print_file_name(&ss, NULL, cmd, pid, spikeno, percentage, ".out");
    ss.print(" 2> ");
    print_file_name(&ss, NULL, cmd, pid, spikeno, percentage, ".err");
    ss.print("'");

    stderr_stream.print_cr("HiMemReport: executing \"%s\" ...", ss.base());
    // Note: fork_and_exec uses posix_spawn, which in turn *should* use clone(VM_VFORK) or possibly vfork,
    // so we should have no problem forking in high mem pressure scenarios.
    int rc = os::fork_and_exec(ss.base());
    stderr_stream.print_cr("HiMemReport: Done (%d).", rc);
  }
}

//////////////////// alert handling and reporting ///////////////////////////////////////////////////////////////

static int g_num_alerts = 0;

static void trigger_high_memory_report(int alvl, int spikeno, int percentage, size_t triggering_size) {

  g_num_alerts ++;

  stringStream reason;
  reason.print("rss+swap (" SIZE_FORMAT " K) larger than %d%% of %s (" SIZE_FORMAT " K).",
               triggering_size / K, percentage, describe_maximum_by_compare_type(g_compare_what),
               g_alert_state->maximum() / K);
  const char* message = reason.base();
  const int pid = os::current_process_id();

  bool printed = false;

  print_high_memory_report_header(&stderr_stream, message);

  if (g_report_dir != NULL) {
    // Dump to file in report dir
    stringStream ss;
    print_file_name(&ss, HiMemReportDir, "sapmachine_himemalert", pid, spikeno, percentage, ".log");
    fileStream fs(ss.base());
    if (fs.is_open()) {
      stderr_stream.print_cr("# Printing to %s", ss.base());
      print_high_memory_report_header(&fs, message);
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

  if (g_jcmds != NULL) {
    call_jcmds(spikeno, percentage);
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
      // If we passed 66% mark, do a NMT baseline
      if (AlertState::alert_level_percentage(old_alvl) < 66 && new_percentage >= 66) {
        if (NMTStuff::capture_baseline()) {
          stderr_stream.print_cr("HiMemoryReport: ... captured NMT baseline");
        }
      }
    } else if (old_alvl > 0 && new_alvl == 0){
      // Memory usage recovered, and we hit the decay time, and now all is well again.
      stderr_stream.print_cr("HiMemoryReport: rss+swap=" SIZE_FORMAT " K - seems we recovered. Resetting alert level.",
                             rss_swap / K);
      NMTStuff::reset();
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

  // Note:
  // unrecoverable errors:
  //  - errors the user can easily correct (bad arguments) cause exit right away
  //  - errors which are subject to environment and cannot be dealt with/are unpredictable
  //    cause facility to be disabled (with UL warning)

  assert(HiMemReport, "only call for +HiMemReport");

  assert(g_compare_what == compare_type::compare_none && g_alert_state == NULL, "Only initialize once");

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
    log_info(os)("Vitals HiMemReport: Setting limit to HiMemReportMax (" SIZE_FORMAT " K).", limit / K);
  } else {
    OSWrapper::update_if_needed();
    if (OSWrapper::syst_cgro_lim() != INVALID_VALUE) {
      // limit against cgroup limit
      g_compare_what = compare_type::compare_rss_vs_cgroup_limit;
      limit = (size_t)OSWrapper::syst_cgro_lim();
      log_info(os)("Vitals HiMemReport: Setting limit to cgroup memory limit (" SIZE_FORMAT " K).", limit / K);
    } else if (OSWrapper::syst_phys() != INVALID_VALUE) {
      // limit against total physical memory
      g_compare_what = compare_type::compare_rss_vs_phys;
      limit = (size_t)OSWrapper::syst_phys();
      log_info(os)("Vitals HiMemReport: Setting limit to total physical memory (" SIZE_FORMAT " K).", limit / K);
    }
  }

  if (limit == 0) {
    log_warning(os)("Vitals HiMemReport: limit could not be established; will disable high memory reports "
                    "(specify HiMemReportMax to establish a manual limit).");
    FLAG_SET_ERGO(HiMemReport, false);
    return;
  }

  // HiMemReportDir:
  // We fix up the report directory when VM starts, so if its relative, it refers to the initial current directory.
  // If it cannot be established, we treat it as predictable argument error and exit the VM.
  if (HiMemReportDir != NULL && ::strlen(HiMemReportDir) > 0) {
    g_report_dir = new ReportDir(HiMemReportDir);
    if (!g_report_dir->create_if_needed()) {
      log_warning(os)("Vitals: Cannot access HiMemReportDir %s.", g_report_dir->path());
      vm_exit_during_initialization("Vitals HiMemReport: Failed to create or access HiMemReportDir.");
      return;
    }
  }

  g_alert_state = new AlertState(limit);

  if (HiMemReportExec != NULL) {
    if (HiMemReportDir == NULL) {
      vm_exit_during_initialization("Vitals HiMemReport: HiMemReportExec requires HiMemReportDir.");
    }
    g_jcmds = LilJcmdParser::parse_exec_string(HiMemReportExec);
#ifdef ASSERT
    if (g_jcmds != NULL) {
      g_jcmds->print_on(&stderr_stream);
    }
#endif
  }

  if (!initialize_reporter_thread()) {
    log_warning(os)("Vitals HiMemReport: Failed to start monitor thread. Will disable.");
    FLAG_SET_ERGO(HiMemReport, false);
    return;
  }

  log_info(os)("Vitals: HiMemReport subsystem initialized.");

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

