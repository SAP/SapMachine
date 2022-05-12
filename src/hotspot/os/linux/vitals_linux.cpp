/*
 * Copyright (c) 2019, 2022 SAP SE. All rights reserved.
 * Copyright (c) 2019, 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "osContainer_linux.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "vitals/vitals_internals.hpp"

#include <malloc.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

extern const char* sapmachine_get_memory_controller_path();

namespace sapmachine_vitals {

class ProcFile {
  char* _buf;

  // To keep the code simple, I just use a fixed sized buffer.
  enum { bufsize = 64*K };

public:

  ProcFile() : _buf(NULL) {
    _buf = (char*)os::malloc(bufsize, mtInternal);
  }

  ~ProcFile () {
    os::free(_buf);
  }

  bool read(const char* filename) {

    FILE* f = ::fopen(filename, "r");
    if (f == NULL) {
      return false;
    }

    size_t bytes_read = ::fread(_buf, 1, bufsize, f);
    _buf[bufsize - 1] = '\0';

    ::fclose(f);

    return bytes_read > 0 && bytes_read < bufsize;
  }

  const char* text() const { return _buf; }

  // Utility function; parse a number string as value_t
  static value_t as_value(const char* text, size_t scale = 1) {
    value_t value;
    errno = 0;
    char* endptr = NULL;
    value = (value_t)::strtoll(text, &endptr, 10);
    if (endptr == text || errno != 0) {
      value = INVALID_VALUE;
    } else {
      value *= scale;
    }
    return value;
  }

  // Return the start of the file, as number. Useful for proc files which
  // contain a single number. Returns INVALID_VALUE if value did not parse
  value_t as_value(size_t scale = 1) const {
    return as_value(_buf, scale);
  }

  const char* get_prefixed_line(const char* prefix) const {
    return ::strstr(_buf, prefix);
  }

  value_t parsed_prefixed_value(const char* prefix, size_t scale = 1) const {
    value_t value = INVALID_VALUE;
    const char* const s = get_prefixed_line(prefix);
    if (s != NULL) {
      errno = 0;
      const char* p = s + ::strlen(prefix);
      return as_value(p, scale);
    }
    return value;
  }

};

struct cpu_values_t {
  value_t user;
  value_t nice;
  value_t system;
  value_t idle;
  value_t iowait;
  value_t steal;
  value_t guest;
  value_t guest_nice;
};

void parse_proc_stat_cpu_line(const char* line, cpu_values_t* out) {
  // Note: existence of some of these values depends on kernel version
  out->user = out->nice = out->system = out->idle = out->iowait = out->steal = out->guest = out->guest_nice =
      INVALID_VALUE;
  uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  int num = ::sscanf(line,
      "cpu "
      UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " "
      UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " ",
      &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
  if (num >= 4) {
    out->user = user;
    out->nice = nice;
    out->system = system;
    out->idle = idle;
    if (num >= 5) { // iowait (5) (since Linux 2.5.41)
      out->iowait = iowait;
      if (num >= 8) { // steal (8) (since Linux 2.6.11)
        out->steal = steal;
        if (num >= 9) { // guest (9) (since Linux 2.6.24)
          out->guest = guest;
          if (num >= 10) { // guest (9) (since Linux 2.6.33)
            out->guest_nice = guest_nice;
          }
        }
      }
    }
  }
}


/////// Columns ////////

// A special class to display cpu time
class CPUTimeColumn: public Column {

  long _clk_tck;
  int _num_cores;

  int do_print0(outputStream* st, value_t value, value_t last_value,
      int last_value_age, const print_info_t* pi) const {
    // CPU values may overflow, so the delta may be negative.
    if (last_value > value) {
      return 0;
    }
    int l = 0;
    if (last_value != INVALID_VALUE) {

      // If the last sample is less than one second old, we omit calculating the cpu
      // usage.
      if (last_value_age > 0) {

        // Values are in ticks. Convert to ms.
        const uint64_t value_ms = (value * 1000) / _clk_tck;
        const uint64_t last_value_ms = (last_value * 1000) / _clk_tck;
        const uint64_t delta_ms = value_ms - last_value_ms;

        // Calculate the number of wallclock milliseconds for the delta interval...
        const uint64_t age_ms = last_value_age * 1000;

        // times number of available cores.
        const uint64_t total_cpu_time_ms = age_ms * _num_cores;

        // Put the spent cpu time in reference to the total available cpu time.
        const double percentage = (100.0f * delta_ms) / total_cpu_time_ms;

        char buf[32];
        l = jio_snprintf(buf, sizeof(buf), "%.0f", percentage);
        if (st != NULL) {
          st->print_raw(buf);
        }
      }
    }
    return l;
  }

public:
  CPUTimeColumn(const char* category, const char* header, const char* name, const char* description)
    : Column(category, header, name, description)
  {
    _clk_tck = ::sysconf(_SC_CLK_TCK);
    _num_cores = os::active_processor_count();
  }

};

static Column* g_col_system_memavail = NULL;
static Column* g_col_system_memcommitted = NULL;
static Column* g_col_system_memcommitted_ratio = NULL;
static Column* g_col_system_swap = NULL;

static Column* g_col_system_pages_swapped_in = NULL;
static Column* g_col_system_pages_swapped_out = NULL;

static Column* g_col_system_num_procs = NULL;
static Column* g_col_system_num_threads = NULL;

static Column* g_col_system_num_procs_running = NULL;
static Column* g_col_system_num_procs_blocked = NULL;

static bool g_show_cgroup_info = false;
static Column* g_col_system_cgrp_limit_in_bytes = NULL;
static Column* g_col_system_cgrp_soft_limit_in_bytes = NULL;
static Column* g_col_system_cgrp_usage_in_bytes = NULL;
static Column* g_col_system_cgrp_memsw_limit_in_bytes = NULL;
static Column* g_col_system_cgrp_memsw_usage_in_bytes = NULL;
static Column* g_col_system_cgrp_kmem_usage_in_bytes = NULL;

static Column* g_col_system_cpu_user = NULL;
static Column* g_col_system_cpu_system = NULL;
static Column* g_col_system_cpu_idle = NULL;
static Column* g_col_system_cpu_steal = NULL;
static Column* g_col_system_cpu_guest = NULL;

static Column* g_col_process_virt = NULL;

static bool g_show_rss_detail_info = false;
static Column* g_col_process_rss = NULL;
static Column* g_col_process_rssanon = NULL;
static Column* g_col_process_rssfile = NULL;
static Column* g_col_process_rssshmem = NULL;

static Column* g_col_process_swapped_out = NULL;

static Column* g_col_process_chp_used = NULL;
static Column* g_col_process_chp_free = NULL;

static Column* g_col_process_cpu_user = NULL;
static Column* g_col_process_cpu_system = NULL;

static Column* g_col_process_num_of = NULL;
static Column* g_col_process_io_bytes_read = NULL;
static Column* g_col_process_io_bytes_written = NULL;

static Column* g_col_process_num_threads = NULL;


// Try to obtain mallinfo2. That replacement of mallinf is 64-bit capable and its values won't wrap.
// Only exists in glibc 2.33 and later.
#ifdef __GLIBC__
struct glibc_mallinfo2 {
  size_t arena;
  size_t ordblks;
  size_t smblks;
  size_t hblks;
  size_t hblkhd;
  size_t usmblks;
  size_t fsmblks;
  size_t uordblks;
  size_t fordblks;
  size_t keepcost;
};
typedef struct glibc_mallinfo2 (*mallinfo2_func_t)(void);
static mallinfo2_func_t g_mallinfo2 = NULL;
static void mallinfo2_init() {
  g_mallinfo2 = CAST_TO_FN_PTR(mallinfo2_func_t, dlsym(RTLD_DEFAULT, "mallinfo2"));
}
#endif // __GLIBC__

/////////////// cgroup stuff
// We use part of the hotspot cgroup wrapper, but not all of it.
// The reason:
// - wrapper uses UL heavily, which I don't want to happen in a sampler thread (I only log in initialization, which is ok)
// - wrapper does not expose all metrics I need (eg kmem)
// What the wrapper does very nicely is the parse stuff, which I don't want to re-invent, therefore
// I use the wrapper to get the controller path.

class CGroups : public AllStatic {

  static bool _containerized;
  static const char* _file_usg;
  static const char* _file_usgsw;
  static const char* _file_lim;
  static const char* _file_limsw;
  static const char* _file_slim;
  static const char* _file_kusg;

public:

  static bool initialize() {

    // For the heck of it, I go through with initialization even if we are not
    // containerized, since I like to know controller paths even for those cases.

    _containerized = OSContainer::is_containerized();
    log_info(os)("Vitals cgroup initialization: containerized = %d", _containerized);

    const char* controller_path = sapmachine_get_memory_controller_path();
    if (controller_path == NULL) {
      log_info(os)("Vitals cgroup initialization: controller path NULL");
      return false;
    }
    size_t pathlen = ::strlen(controller_path);
    if (pathlen == 0) {
      log_info(os)("Vitals cgroup initialization: controller path empty?");
      return false;
    }
    stringStream path;
    if (controller_path[pathlen - 1] == '/') {
      path.print("%s", controller_path);
    } else {
      path.print("%s/", controller_path);
    }

    log_info(os)("Vitals cgroup initialization: controller path: %s", path.base());

    // V1 or V2?
    stringStream ss;
    ss.print("%smemory.usage_in_bytes", path.base());
    struct stat s;
    const bool isv1 = os::file_exists(ss.base());
    if (isv1) {
      log_info(os)("Vitals cgroup initialization: v1");
    } else  {
      ss.reset();
      ss.print("%smemory.current", path.base());
      if (os::file_exists(ss.base())) {
        // okay, its v2
        log_info(os)("Vitals cgroup initialization: v2");
      } else {
        log_info(os)("Vitals cgroup initialization: no clue. Giving up.");
      }
    }

    _file_usg = os::strdup(ss.base()); // so, we have that.

#define STORE_PATH(variable, filename) \
  ss.reset(); ss.print("%s%s", path.base(), filename); variable = os::strdup(ss.base());

    if (isv1) {
      STORE_PATH(_file_usgsw, "memory.memsw.usage_in_bytes");
      STORE_PATH(_file_kusg, "memory.kmem.usage_in_bytes");
      STORE_PATH(_file_lim, "memory.limit_in_bytes");
      STORE_PATH(_file_limsw, "memory.memsw.limit_in_bytes");
      STORE_PATH(_file_slim, "memory.soft_limit_in_bytes");
    } else {
      STORE_PATH(_file_usgsw, "memory.swap.current");
      STORE_PATH(_file_kusg, "memory.kmem.usage_in_bytes");
      STORE_PATH(_file_lim, "memory.max");
      STORE_PATH(_file_limsw, "memory.swap.max");
      STORE_PATH(_file_slim, "memory.low");
    }
#undef STORE_PATH

#define LOG_PATH(variable) \
    log_info(os)("Vitals: %s=%s", #variable, variable == NULL ? "<null>" : variable);
    LOG_PATH(_file_usg)
    LOG_PATH(_file_usgsw)
    LOG_PATH(_file_kusg)
    LOG_PATH(_file_lim)
    LOG_PATH(_file_limsw)
    LOG_PATH(_file_slim)
#undef LOG_PATH

    // Initialization went through. We show columns if we are containerized.
    return _containerized;
  }

  struct cgroup_values_t {
    value_t lim;
    value_t limsw;
    value_t slim;
    value_t usg;
    value_t usgsw;
    value_t kusg;
  };

  static bool get_stats(cgroup_values_t* v) {
    v->lim = v->limsw = v->slim = v->usg = v->usgsw = v->kusg = INVALID_VALUE;
    ProcFile pf;
#define GET_VALUE(var) \
  { \
    const char* what = _file_ ## var; \
    if (what != NULL && pf.read(what)) { \
      v-> var = pf.as_value(1); \
    } \
  }
  GET_VALUE(usg);
  GET_VALUE(usgsw);
  GET_VALUE(kusg);
  GET_VALUE(lim);
  GET_VALUE(limsw);
  GET_VALUE(slim);
#undef GET_VALUE
    // Cgroup limits defaults to PAGE_COUNTER_MAX in the kernel; so a very large number means "no limit"
    // Note that on 64-bit, the default is LONG_MAX aligned down to pagesize; but I am not sure this is
    // always true, so I just assume a very high value.
    const size_t practically_infinite = LP64_ONLY(128 * K * G) NOT_LP64(4 * G);
    if (v->lim > practically_infinite)    v->lim = INVALID_VALUE;
    if (v->slim > practically_infinite)   v->slim = INVALID_VALUE;
    if (v->limsw > practically_infinite)  v->limsw = INVALID_VALUE;
    return true;

  } // end: CGroups::get_stats()

}; // end: CGroups

bool CGroups::_containerized = false;
const char* CGroups::_file_usg = NULL;
const char* CGroups::_file_usgsw = NULL;
const char* CGroups::_file_lim = NULL;
const char* CGroups::_file_limsw = NULL;
const char* CGroups::_file_slim = NULL;
const char* CGroups::_file_kusg = NULL;

bool platform_columns_initialize() {

  const char* const system_cat = "system";
  const char* const process_cat = "process";

  Legend::the_legend()->add_footnote("   [host]: values are host-global (not containerized).");
  Legend::the_legend()->add_footnote("   [cgrp]: only shown if containerized");
  Legend::the_legend()->add_footnote("    [krn]: depends on kernel version");

  g_col_system_memavail =
      define_column<MemorySizeColumn>(system_cat, NULL, "avail", "Memory available without swapping [host]", true);
  g_col_system_memcommitted =
      define_column<MemorySizeColumn>(system_cat, NULL, "comm", "Committed memory [host]", true);
  g_col_system_memcommitted_ratio =
      define_column<PlainValueColumn>(system_cat, NULL, "crt", "Committed-to-Commit-Limit ratio (percent) [host]", true);
  g_col_system_swap =
      define_column<MemorySizeColumn>(system_cat, NULL, "swap", "Swap space used [host]", true);

  g_col_system_pages_swapped_in =
      define_column<DeltaValueColumn>(system_cat, NULL, "si", "Number of pages swapped in [host] [delta]", true);
  g_col_system_pages_swapped_out =
      define_column<DeltaValueColumn>(system_cat, NULL, "so", "Number of pages pages swapped out [host] [delta]", true);

  g_col_system_num_procs =
      define_column<PlainValueColumn>(system_cat, NULL, "p", "Number of processes", true);
  g_col_system_num_threads =
      define_column<PlainValueColumn>(system_cat, NULL, "t", "Number of threads", true);

  g_col_system_num_procs_running =
      define_column<PlainValueColumn>(system_cat, NULL, "pr", "Number of processes running", true);
  g_col_system_num_procs_blocked =
      define_column<PlainValueColumn>(system_cat, NULL, "pb", "Number of processes blocked", true);

  g_col_system_cpu_user =
      define_column<CPUTimeColumn>(system_cat, "cpu", "us", "CPU user time [host]", true);
  g_col_system_cpu_system =
      define_column<CPUTimeColumn>(system_cat, "cpu", "sy", "CPU system time [host]", true);
  g_col_system_cpu_idle =
        define_column<CPUTimeColumn>(system_cat, "cpu", "id", "CPU idle time [host]", true);
  g_col_system_cpu_steal =
        define_column<CPUTimeColumn>(system_cat, "cpu", "st", "CPU time stolen [host]", true);
  g_col_system_cpu_guest =
        define_column<CPUTimeColumn>(system_cat, "cpu", "gu", "CPU time spent on guest [host]", true);

  g_show_cgroup_info = CGroups::initialize();
  g_col_system_cgrp_limit_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "lim", "cgroup memory limit [cgrp]", g_show_cgroup_info);
  g_col_system_cgrp_memsw_limit_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "limsw", "cgroup memory+swap limit [cgrp]", g_show_cgroup_info);
  g_col_system_cgrp_soft_limit_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "slim", "cgroup memory soft limit [cgrp]", g_show_cgroup_info);
  g_col_system_cgrp_usage_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "usg", "cgroup memory usage [cgrp]", g_show_cgroup_info);
  g_col_system_cgrp_memsw_usage_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "usgsw", "cgroup memory+swap usage [cgrp]", g_show_cgroup_info);
  g_col_system_cgrp_kmem_usage_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "kusg", "cgroup kernel memory usage (cgroup v1 only) [cgrp]", g_show_cgroup_info);

  // Process

  g_col_process_virt =
    define_column<MemorySizeColumn>(process_cat, NULL, "virt", "Virtual size", true);

  // RSS detail needs kernel >= 4.5
  {
    ProcFile bf;
    if (bf.read("/proc/self/status")) {
      g_show_rss_detail_info = (bf.parsed_prefixed_value("RssAnon:", 1) != INVALID_VALUE);
    }
  }
  g_col_process_rss =
      define_column<MemorySizeColumn>(process_cat, "rss", "all", "Resident set size, total", true);
  g_col_process_rssanon =
      define_column<MemorySizeColumn>(process_cat, "rss", "anon", "Resident set size, anonymous memory [krn]", g_show_rss_detail_info);
  g_col_process_rssfile =
      define_column<MemorySizeColumn>(process_cat, "rss", "file", "Resident set size, file mappings [krn]", g_show_rss_detail_info);
  g_col_process_rssshmem =
      define_column<MemorySizeColumn>(process_cat, "rss", "shm", "Resident set size, shared memory [krn]", g_show_rss_detail_info);

  g_col_process_swapped_out =
      define_column<MemorySizeColumn>(process_cat, NULL, "swdo", "Memory swapped out", true);

  // glibc heap info depends on, obviously, glibc.
  // Also slightly modify the text if only mallinf, not mallinf2, is available on 64-bit
#ifdef __GLIBC__
  mallinfo2_init();
  const bool show_glibc_heap_info = true;
#else
  const bool show_glibc_heap_info = false;
#endif
  g_col_process_chp_used =
      define_column<MemorySizeColumn>(process_cat, "cheap", "usd", "C-Heap, in-use allocations (may be unavailable if RSS > 4G)", show_glibc_heap_info);
  g_col_process_chp_free =
      define_column<MemorySizeColumn>(process_cat, "cheap", "free", "C-Heap, bytes in free blocks (may be unavailable if RSS > 4G)", show_glibc_heap_info);

  g_col_process_cpu_user =
      define_column<CPUTimeColumn>(process_cat, "cpu", "us", "Process cpu user time", true);

  g_col_process_cpu_system =
    define_column<CPUTimeColumn>(process_cat, "cpu", "sy", "Process cpu system time", true);

  g_col_process_num_of =
      define_column<PlainValueColumn>(process_cat, "io", "of", "Number of open files", true);

  g_col_process_io_bytes_read =
    define_column<DeltaMemorySizeColumn>(process_cat, "io", "rd", "IO bytes read from storage or cache", true);

  g_col_process_io_bytes_written =
      define_column<DeltaMemorySizeColumn>(process_cat, "io", "wr", "IO bytes written", true);

  g_col_process_num_threads =
      define_column<PlainValueColumn>(process_cat, NULL, "thr", "Number of native threads", true);

  return true;
}

static void set_value_in_sample(Column* col, Sample* sample, value_t val) {
  if (col != NULL) {
    int index = col->index();
    sample->set_value(index, val);
  }
}

// Helper function, returns true if string is a numerical id
static bool is_numerical_id(const char* s) {
  const char* p = s;
  while(*p >= '0' && *p <= '9') {
    p ++;
  }
  return *p == '\0' ? true : false;
}

void sample_platform_values(Sample* sample) {

  int idx = 0;
  value_t v = 0;

  value_t rss_all = 0;

  ProcFile bf;
  if (bf.read("/proc/meminfo")) {

    // All values in /proc/meminfo are in KB
    const size_t scale = K;

    set_value_in_sample(g_col_system_memavail, sample,
        bf.parsed_prefixed_value("MemAvailable:", scale));

    value_t swap_total = bf.parsed_prefixed_value("SwapTotal:", scale);
    value_t swap_free = bf.parsed_prefixed_value("SwapFree:", scale);
    if (swap_total != INVALID_VALUE && swap_free != INVALID_VALUE) {
      set_value_in_sample(g_col_system_swap, sample, swap_total - swap_free);
    }

    // Calc committed ratio. Values > 100% indicate overcommitment.
    value_t commitlimit = bf.parsed_prefixed_value("CommitLimit:", scale);
    value_t committed = bf.parsed_prefixed_value("Committed_AS:", scale);
    if (commitlimit != INVALID_VALUE && commitlimit != 0 && committed != INVALID_VALUE) {
      set_value_in_sample(g_col_system_memcommitted, sample, committed);
      value_t ratio = (committed * 100) / commitlimit;
      set_value_in_sample(g_col_system_memcommitted_ratio, sample, ratio);
    }
  }

  if (bf.read("/proc/vmstat")) {
    set_value_in_sample(g_col_system_pages_swapped_in, sample, bf.parsed_prefixed_value("pswpin"));
    set_value_in_sample(g_col_system_pages_swapped_out, sample, bf.parsed_prefixed_value("pswpout"));
  }

  if (bf.read("/proc/stat")) {
    // Read and parse global cpu values
    cpu_values_t values;
    const char* line = bf.get_prefixed_line("cpu");
    parse_proc_stat_cpu_line(line, &values);

    set_value_in_sample(g_col_system_cpu_user, sample, values.user + values.nice);
    set_value_in_sample(g_col_system_cpu_system, sample, values.system);
    set_value_in_sample(g_col_system_cpu_idle, sample, values.idle);
    set_value_in_sample(g_col_system_cpu_steal, sample, values.steal);
    set_value_in_sample(g_col_system_cpu_guest, sample, values.guest + values.guest_nice);

    set_value_in_sample(g_col_system_num_procs_running, sample,
        bf.parsed_prefixed_value("procs_running"));
    set_value_in_sample(g_col_system_num_procs_blocked, sample,
        bf.parsed_prefixed_value("procs_blocked"));
  }

  // cgroups business
  if (g_show_cgroup_info) {
    CGroups::cgroup_values_t v;
    if (CGroups::get_stats(&v)) {
      set_value_in_sample(g_col_system_cgrp_usage_in_bytes, sample, v.usg);
      set_value_in_sample(g_col_system_cgrp_memsw_usage_in_bytes, sample, v.usgsw);
      set_value_in_sample(g_col_system_cgrp_kmem_usage_in_bytes, sample, v.kusg);
      set_value_in_sample(g_col_system_cgrp_limit_in_bytes, sample, v.lim);
      set_value_in_sample(g_col_system_cgrp_soft_limit_in_bytes, sample, v.slim);
      set_value_in_sample(g_col_system_cgrp_memsw_limit_in_bytes, sample, v.limsw);
    }
  }

  if (bf.read("/proc/self/status")) {

    set_value_in_sample(g_col_process_virt, sample, bf.parsed_prefixed_value("VmSize:", K));
    set_value_in_sample(g_col_process_swapped_out, sample, bf.parsed_prefixed_value("VmSwap:", K));
    rss_all = bf.parsed_prefixed_value("VmRSS:", K);
    set_value_in_sample(g_col_process_rss, sample, rss_all);

    if (g_show_rss_detail_info) {
      set_value_in_sample(g_col_process_rssanon, sample, bf.parsed_prefixed_value("RssAnon:", K));
      set_value_in_sample(g_col_process_rssfile, sample, bf.parsed_prefixed_value("RssFile:", K));
      set_value_in_sample(g_col_process_rssshmem, sample, bf.parsed_prefixed_value("RssShmem:", K));
    }

    set_value_in_sample(g_col_process_num_threads, sample,
        bf.parsed_prefixed_value("Threads:"));

  }

  // Number of open files: iterate over /proc/self/fd and count.
  {
    DIR* d = ::opendir("/proc/self/fd");
    if (d != NULL) {
      value_t v = 0;
      struct dirent* en = NULL;
      do {
        en = ::readdir(d);
        if (en != NULL) {
          if (::strcmp(".", en->d_name) == 0 || ::strcmp("..", en->d_name) == 0 ||
              ::strcmp("0", en->d_name) == 0 || ::strcmp("1", en->d_name) == 0 || ::strcmp("2", en->d_name) == 0) {
            // omit
          } else {
            v ++;
          }
        }
      } while(en != NULL);
      ::closedir(d);
      set_value_in_sample(g_col_process_num_of, sample, v);
    }
  }

  // Number of processes: iterate over /proc/<pid> and count.
  // Number of threads: read "num_threads" from /proc/<pid>/stat
  {
    DIR* d = ::opendir("/proc");
    if (d != NULL) {
      value_t v_p = 0;
      value_t v_t = 0;
      struct dirent* en = NULL;
      do {
        en = ::readdir(d);
        if (en != NULL) {
          if (is_numerical_id(en->d_name)) {
            v_p ++;
            char tmp[128];
            jio_snprintf(tmp, sizeof(tmp), "/proc/%s/stat", en->d_name);
            if (bf.read(tmp)) {
              const char* text = bf.text();
              // See man proc(5)
              // (20) num_threads  %ld
              long num_threads = 0;
              ::sscanf(text, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %ld", &num_threads);
              v_t += num_threads;
            }
          }
        }
      } while(en != NULL);
      ::closedir(d);
      set_value_in_sample(g_col_system_num_procs, sample, v_p);
      set_value_in_sample(g_col_system_num_threads, sample, v_t);
    }
  }

  if (bf.read("/proc/self/io")) {
    set_value_in_sample(g_col_process_io_bytes_read, sample,
        bf.parsed_prefixed_value("rchar:"));
    set_value_in_sample(g_col_process_io_bytes_written, sample,
        bf.parsed_prefixed_value("wchar:"));
  }

  if (bf.read("/proc/self/stat")) {
    const char* text = bf.text();
    // See man proc(5)
    // (14) utime  %lu
    // (15) stime  %lu
    long unsigned cpu_utime = 0;
    long unsigned cpu_stime = 0;
    ::sscanf(text, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &cpu_utime, &cpu_stime);
    set_value_in_sample(g_col_process_cpu_user, sample, cpu_utime);
    set_value_in_sample(g_col_process_cpu_system, sample, cpu_stime);
  }

#ifdef __GLIBC__
  // Collect some c-heap info using either one of mallinfo or mallinfo2.
  if (g_mallinfo2 != NULL) {
    struct glibc_mallinfo2 mi = g_mallinfo2();
    // (from experiments and glibc source code reading: the closest to "used" would be adding the mmaped data area size
    //  (contains large allocations) to the small block sizes
    set_value_in_sample(g_col_process_chp_used, sample, mi.uordblks + mi.hblkhd);
    set_value_in_sample(g_col_process_chp_free, sample, mi.fordblks);
  } else {
    struct mallinfo mi = mallinfo();
    set_value_in_sample(g_col_process_chp_used, sample, (size_t)(unsigned)mi.uordblks + (size_t)(unsigned)mi.hblkhd);
    set_value_in_sample(g_col_process_chp_free, sample, (size_t)(unsigned)mi.fordblks);
    // In 64-bit mode, omit printing values if we could conceivably have wrapped, since they are misleading.
#ifdef _LP64
    if (rss_all >= 4 * G) {
      set_value_in_sample(g_col_process_chp_used, sample, INVALID_VALUE);
      set_value_in_sample(g_col_process_chp_free, sample, INVALID_VALUE);
    }
#endif
  }
#endif // __GLIBC__

} // end: sample_platform_values

} // namespace sapmachine_vitals
