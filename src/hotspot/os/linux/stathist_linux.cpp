/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2019 SAP SE. All rights reserved.
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

#include "runtime/os.hpp"
#include "services/stathist_internals.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <fcntl.h>
#include <string.h>
#include <errno.h>

namespace StatisticsHistory {

class ProcFile {
  char* _buf;

  // To keep the code simple, I just use a fixed sized buffer.
  enum { bufsize = 4*K };

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

  const char* get_prefixed_line(const char* prefix) const {
    return ::strstr(_buf, prefix);
  }

  value_t parsed_prefixed_value(const char* prefix, size_t scale = 1) const {
    value_t value = INVALID_VALUE;
    const char* const s = get_prefixed_line(prefix);
    if (s != NULL) {
      errno = 0;
      const char* p = s + ::strlen(prefix);
      char* endptr = NULL;
      value = (value_t)::strtoll(p, &endptr, 10);
      if (p == endptr || errno != 0) {
        value = INVALID_VALUE;
      } else {
        value *= scale;
      }
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
  int user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  int num = ::sscanf(line,
      "cpu %d %d %d %d %d %d %d %d %d %d",
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

  int do_print(outputStream* st, value_t value, value_t last_value,
      int last_value_age, const print_info_t* pi) const {
    // CPU values may overflow, so the delta may be negative.
    if (last_value > value) {
      return 0;
    }
    int l = 0;
    if (value != INVALID_VALUE && last_value != INVALID_VALUE) {

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

//static Column* g_col_system_memtotal = NULL;
static Column* g_col_system_memfree = NULL;
static Column* g_col_system_memavail = NULL;
static Column* g_col_system_swap = NULL;

static Column* g_col_system_pages_swapped_in = NULL;
static Column* g_col_system_pages_swapped_out = NULL;

static Column* g_col_system_num_procs_running = NULL;
static Column* g_col_system_num_procs_blocked = NULL;

static Column* g_col_system_cpu_user = NULL;
static Column* g_col_system_cpu_system = NULL;
static Column* g_col_system_cpu_idle = NULL;
static Column* g_col_system_cpu_waiting = NULL;
static Column* g_col_system_cpu_steal = NULL;
static Column* g_col_system_cpu_guest = NULL;

static Column* g_col_process_virt = NULL;
static Column* g_col_process_rss = NULL;
static Column* g_col_process_rssanon = NULL;
static Column* g_col_process_rssfile = NULL;
static Column* g_col_process_rssshmem = NULL;
static Column* g_col_process_swapped_out = NULL;

static Column* g_col_process_cpu_user = NULL;
static Column* g_col_process_cpu_system = NULL;

static Column* g_col_process_num_of = NULL;
static Column* g_col_process_io_bytes_read = NULL;
static Column* g_col_process_io_bytes_written = NULL;

static Column* g_col_process_num_threads = NULL;

bool platform_columns_initialize() {

  // Order matters!
//  g_col_system_memtotal = new MemorySizeColumn("system", NULL, "total", "Total physical memory.");

  // Since free and avail are kind of redundant, only display free if avail is not available (very old kernels)
  bool have_avail = false;
  {
    ProcFile bf;
    if (bf.read("/proc/meminfo")) {
      have_avail = (bf.parsed_prefixed_value("MemAvailable:", 1) != INVALID_VALUE);
    }
  }

  if (have_avail) {
    g_col_system_memavail = new MemorySizeColumn("system", NULL, "avail", "Memory available without swapping (>=3.14)");
  } else {
    g_col_system_memfree = new MemorySizeColumn("system", NULL, "free", "Unused memory");
  }

  g_col_system_swap = new MemorySizeColumn("system", NULL, "swap", "Swap space used");

  g_col_system_pages_swapped_in = new DeltaValueColumn("system", NULL, "si", "Number of pages swapped in");

  g_col_system_pages_swapped_out = new DeltaValueColumn("system", NULL, "so", "Number of pages pages swapped out");

  g_col_system_num_procs_running = new PlainValueColumn("system", NULL, "pr", "Number of tasks running");
  g_col_system_num_procs_blocked = new PlainValueColumn("system", NULL, "pb", "Number of tasks blocked");

  g_col_system_cpu_user =     new CPUTimeColumn("system", "cpu", "us", "Global cpu user time");
  g_col_system_cpu_system =   new CPUTimeColumn("system", "cpu", "sy", "Global cpu system time");
  g_col_system_cpu_idle =     new CPUTimeColumn("system", "cpu", "id", "Global cpu idle time");
  g_col_system_cpu_waiting =  new CPUTimeColumn("system", "cpu", "wa", "Global cpu time spent waiting for IO");
  g_col_system_cpu_steal =    new CPUTimeColumn("system", "cpu", "st", "Global cpu time stolen");
  g_col_system_cpu_guest =    new CPUTimeColumn("system", "cpu", "gu", "Global cpu time spent on guest");

  g_col_process_virt = new MemorySizeColumn("process", NULL, "virt", "Virtual size");

  bool have_rss_detail_info = false;
  {
    ProcFile bf;
    if (bf.read("/proc/self/status")) {
      have_rss_detail_info = bf.parsed_prefixed_value("RssAnon:", 1) != INVALID_VALUE;
    }
  }
  if (have_rss_detail_info) {
    // Linux 4.5 ++
    g_col_process_rss = new MemorySizeColumn("process", "rss", "all", "Resident set size, total");
    g_col_process_rssanon = new MemorySizeColumn("process", "rss", "anon", "Resident set size, anonymous memory (>=4.5)");
    g_col_process_rssfile = new MemorySizeColumn("process", "rss", "file", "Resident set size, file mappings (>=4.5)");
    g_col_process_rssshmem = new MemorySizeColumn("process", "rss", "shm", "Resident set size, shared memory (>=4.5)");
  } else {
    g_col_process_rss = new MemorySizeColumn("process", NULL, "rss", "Resident set size, total");
  }

  g_col_process_swapped_out = new MemorySizeColumn("process", NULL, "swdo", "Memory swapped out");

  g_col_process_cpu_user = new CPUTimeColumn("process", "cpu", "us", "Process cpu user time");

  g_col_process_cpu_system = new CPUTimeColumn("process", "cpu", "sy", "Process cpu system time");

  g_col_process_num_of = new PlainValueColumn("process", "io", "of", "Number of open files");

  g_col_process_io_bytes_read = new DeltaMemorySizeColumn("process", "io", "rd", "IO bytes read from storage or cache");

  g_col_process_io_bytes_written = new DeltaMemorySizeColumn("process", "io", "wr", "IO bytes written");

  g_col_process_num_threads = new PlainValueColumn("process", NULL, "thr", "Number of native threads");


  return true;
}

static void set_value_in_record(Column* col, record_t* record, value_t val) {
  if (col != NULL) {
    int index = col->index();
    record->values[index] = val;
  }
}

void sample_platform_values(record_t* record) {

  int idx = 0;
  value_t v = 0;

  ProcFile bf;
  if (bf.read("/proc/meminfo")) {

    // All values in /proc/meminfo are in KB
    const size_t scale = K;

    set_value_in_record(g_col_system_memfree, record,
        bf.parsed_prefixed_value("MemFree:", scale));

    set_value_in_record(g_col_system_memavail, record,
        bf.parsed_prefixed_value("MemAvailable:", scale));

    value_t swap_total = bf.parsed_prefixed_value("SwapTotal:", scale);
    value_t swap_free = bf.parsed_prefixed_value("SwapFree:", scale);
    if (swap_total != INVALID_VALUE && swap_free != INVALID_VALUE) {
      set_value_in_record(g_col_system_swap, record, swap_total - swap_free);
    }

  }

  if (bf.read("/proc/vmstat")) {
    set_value_in_record(g_col_system_pages_swapped_in, record, bf.parsed_prefixed_value("pswpin"));
    set_value_in_record(g_col_system_pages_swapped_out, record, bf.parsed_prefixed_value("pswpout"));
  }

  if (bf.read("/proc/stat")) {
    // Read and parse global cpu values
    cpu_values_t values;
    const char* line = bf.get_prefixed_line("cpu");
    parse_proc_stat_cpu_line(line, &values);

    set_value_in_record(g_col_system_cpu_user, record, values.user + values.nice);
    set_value_in_record(g_col_system_cpu_system, record, values.system);
    set_value_in_record(g_col_system_cpu_idle, record, values.idle);
    set_value_in_record(g_col_system_cpu_waiting, record, values.iowait);
    set_value_in_record(g_col_system_cpu_steal, record, values.steal);
    set_value_in_record(g_col_system_cpu_guest, record, values.guest + values.guest_nice);

    set_value_in_record(g_col_system_num_procs_running, record,
        bf.parsed_prefixed_value("procs_running"));
    set_value_in_record(g_col_system_num_procs_blocked, record,
        bf.parsed_prefixed_value("procs_blocked"));
  }

  if (bf.read("/proc/self/status")) {

    set_value_in_record(g_col_process_virt, record, bf.parsed_prefixed_value("VmSize:", K));
    set_value_in_record(g_col_process_swapped_out, record, bf.parsed_prefixed_value("VmSwap:", K));
    set_value_in_record(g_col_process_rss, record, bf.parsed_prefixed_value("VmRSS:", K));

    set_value_in_record(g_col_process_rssanon, record, bf.parsed_prefixed_value("RssAnon:", K));
    set_value_in_record(g_col_process_rssfile, record, bf.parsed_prefixed_value("RssFile:", K));
    set_value_in_record(g_col_process_rssshmem, record, bf.parsed_prefixed_value("RssShmem:", K));

    set_value_in_record(g_col_process_num_threads, record,
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
      set_value_in_record(g_col_process_num_of, record, v);
    }
  }

  if (bf.read("/proc/self/io")) {
    set_value_in_record(g_col_process_io_bytes_read, record,
        bf.parsed_prefixed_value("rchar:"));
    set_value_in_record(g_col_process_io_bytes_written, record,
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
    set_value_in_record(g_col_process_cpu_user, record, cpu_utime);
    set_value_in_record(g_col_process_cpu_system, record, cpu_stime);
  }

}

} // namespace StatisticsHistory
