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
#include "osContainer_linux.hpp"
#include "vitals_linux_oswrapper.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "vitals/vitals_internals.hpp"

namespace sapmachine_vitals {

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
  CPUTimeColumn(const char* category, const char* header, const char* name, const char* description, Extremum extremum)
    : Column(category, header, name, description, extremum)
  {
    _clk_tck = ::sysconf(_SC_CLK_TCK);
    _num_cores = os::active_processor_count();
  }

};

static bool g_show_system_memavail = false;
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

bool platform_columns_initialize() {

  const char* const system_cat = "system";
  const char* const process_cat = "process";

  Legend::the_legend()->add_footnote("   [host]: values are host-global (not containerized).");
  Legend::the_legend()->add_footnote("   [cgrp]: if containerized or running in systemd slice");
  Legend::the_legend()->add_footnote("    [krn]: depends on kernel version");
  Legend::the_legend()->add_footnote("   [glibc]: only shown for glibc-based distros");

  // Update values once, to get up-to-date readings. Some of those we need to decide whether to show or hide certain columns
  OSWrapper::initialize();
  OSWrapper::update_if_needed();

  // syst-avail depends on kernel version.
  g_show_system_memavail = OSWrapper::syst_avail() != INVALID_VALUE;
  g_col_system_memavail =
      define_column<MemorySizeColumn>(system_cat, NULL, "avail", "Memory available without swapping [host] [krn]", g_show_system_memavail, MIN);
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
      define_column<PlainValueColumn>(system_cat, NULL, "tr", "Number of threads running", true);
  g_col_system_num_procs_blocked =
      define_column<PlainValueColumn>(system_cat, NULL, "tb", "Number of threads blocked on disk IO", true);

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

  // I show cgroup information if the container layer thinks we are containerized OR we have limits established
  // (which should come out as the same, but you never know
  g_show_cgroup_info = OSContainer::is_containerized() || (OSWrapper::syst_cgro_lim() != INVALID_VALUE || OSWrapper::syst_cgro_limsw() != INVALID_VALUE);
  g_col_system_cgrp_limit_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "lim", "cgroup memory limit [cgrp]", g_show_cgroup_info, MIN);
  g_col_system_cgrp_soft_limit_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "slim", "cgroup memory soft limit [cgrp]", g_show_cgroup_info, MIN);
  g_col_system_cgrp_usage_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "usg", "cgroup memory usage [cgrp]", g_show_cgroup_info);
  g_col_system_cgrp_kmem_usage_in_bytes =
        define_column<MemorySizeColumn>(system_cat, "cgroup", "kusg", "cgroup kernel memory usage (cgroup v1 only) [cgrp]", g_show_cgroup_info);

  // Process

  g_col_process_virt =
    define_column<MemorySizeColumn>(process_cat, NULL, "virt", "Virtual size", true);

  // RSS detail needs kernel >= 4.5
  g_show_rss_detail_info = OSWrapper::proc_rss_anon() != INVALID_VALUE;
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
#ifdef __GLIBC__
  const bool show_glibc_heap_info = true;
#else
  const bool show_glibc_heap_info = false;
#endif
  g_col_process_chp_used =
      define_column<MemorySizeColumn>(process_cat, "cheap", "usd", "C-Heap, in-use allocations (may be unavailable if RSS > 4G) [glibc]", show_glibc_heap_info);
  g_col_process_chp_free =
      define_column<MemorySizeColumn>(process_cat, "cheap", "free", "C-Heap, bytes in free blocks (may be unavailable if RSS > 4G) [glibc]", show_glibc_heap_info);

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

void sample_platform_values(Sample* sample) {

  int idx = 0;

  OSWrapper::update_if_needed();

  if (g_show_system_memavail) {
    set_value_in_sample(g_col_system_memavail, sample, OSWrapper::syst_avail());
  }
  set_value_in_sample(g_col_system_swap, sample, OSWrapper::syst_swap());

  set_value_in_sample(g_col_system_memcommitted, sample, OSWrapper::syst_comm());
  set_value_in_sample(g_col_system_memcommitted_ratio, sample, OSWrapper::syst_crt());

  set_value_in_sample(g_col_system_pages_swapped_in, sample, OSWrapper::syst_si());
  set_value_in_sample(g_col_system_pages_swapped_out, sample, OSWrapper::syst_so());

  set_value_in_sample(g_col_system_cpu_user, sample, OSWrapper::syst_cpu_us());
  set_value_in_sample(g_col_system_cpu_system, sample, OSWrapper::syst_cpu_sy());
  set_value_in_sample(g_col_system_cpu_idle, sample, OSWrapper::syst_cpu_id());
  set_value_in_sample(g_col_system_cpu_steal, sample, OSWrapper::syst_cpu_st());
  set_value_in_sample(g_col_system_cpu_guest, sample, OSWrapper::syst_cpu_gu());

  set_value_in_sample(g_col_system_num_procs_running, sample, OSWrapper::syst_tr());
  set_value_in_sample(g_col_system_num_procs_blocked, sample, OSWrapper::syst_tb());

  // cgroups business
  if (g_show_cgroup_info) {
    set_value_in_sample(g_col_system_cgrp_usage_in_bytes, sample, OSWrapper::syst_cgro_usg());
    // set_value_in_sample(g_col_system_cgrp_memsw_usage_in_bytes, sample, OSWrapper::syst_cgro_usgsw());
    set_value_in_sample(g_col_system_cgrp_kmem_usage_in_bytes, sample, OSWrapper::syst_cgro_kusg());
    set_value_in_sample(g_col_system_cgrp_limit_in_bytes, sample, OSWrapper::syst_cgro_lim());
    set_value_in_sample(g_col_system_cgrp_soft_limit_in_bytes, sample, OSWrapper::syst_cgro_slim());
    // set_value_in_sample(g_col_system_cgrp_memsw_limit_in_bytes, sample, OSWrapper::syst_cgro_limsw());
  }

  set_value_in_sample(g_col_system_num_procs, sample, OSWrapper::syst_p());
  set_value_in_sample(g_col_system_num_threads, sample, OSWrapper::syst_t());

  set_value_in_sample(g_col_process_virt, sample, OSWrapper::proc_virt());
  set_value_in_sample(g_col_process_swapped_out, sample, OSWrapper::proc_swdo());
  set_value_in_sample(g_col_process_rss, sample, OSWrapper::proc_rss_all());

  if (g_show_rss_detail_info) {
    set_value_in_sample(g_col_process_rssanon, sample, OSWrapper::proc_rss_anon());
    set_value_in_sample(g_col_process_rssfile, sample, OSWrapper::proc_rss_file());
    set_value_in_sample(g_col_process_rssshmem, sample, OSWrapper::proc_rss_shm());
  }

  set_value_in_sample(g_col_process_num_threads, sample, OSWrapper::proc_thr());
  set_value_in_sample(g_col_process_num_of, sample, OSWrapper::proc_io_of());

  set_value_in_sample(g_col_process_io_bytes_read, sample, OSWrapper::proc_io_rd());
  set_value_in_sample(g_col_process_io_bytes_written, sample, OSWrapper::proc_io_wr());

  set_value_in_sample(g_col_process_cpu_user, sample, OSWrapper::proc_cpu_us());
  set_value_in_sample(g_col_process_cpu_system, sample, OSWrapper::proc_cpu_sy());

#ifdef __GLIBC__
  set_value_in_sample(g_col_process_chp_used, sample, OSWrapper::proc_chea_usd());
  set_value_in_sample(g_col_process_chp_free, sample, OSWrapper::proc_chea_free());
#endif // __GLIBC__

} // end: sample_platform_values

} // namespace sapmachine_vitals
