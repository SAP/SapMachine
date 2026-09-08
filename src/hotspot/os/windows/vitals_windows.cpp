/*
 * Copyright (c) 2019, 2025 SAP SE. All rights reserved.
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

#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "vitals/vitals_internals.hpp"
#include "pdh_interface.hpp"

#include <psapi.h>

namespace sapmachine_vitals {

static Column* g_col_system_memoryload = nullptr;
static Column* g_col_system_avail_phys = nullptr;
static Column* g_col_system_load_average = nullptr;
static Column* g_col_process_working_set_size = nullptr;
static Column* g_col_process_commit_charge = nullptr;

#define QUEUE_LENGTH "\\System\\Processor Queue Length"
#define PROCESSOR_TIME "\\Processor(_Total)\\% Processor Time"

static bool log_pdh(const char* operation, PDH_STATUS status) {
  if (status != ERROR_SUCCESS) {
    log_debug(vitals)("pdh opperation '%s' failed with error code %x", operation, status);
    return false;
  }

  return true;
}

static bool has_loadavg = false;
static double proc_scale_factor = 1.0;

static HQUERY query;
static HCOUNTER queue_length_counter, processor_time_counter;
static PDH_FMT_COUNTERVALUE queue_length, processor_time;

static double get_load_average_impl(bool first_call) {
  double load_avg = -1;

  if (first_call) {
    has_loadavg = log_pdh("open query", PdhDll::PdhOpenQuery(nullptr, 0, &query)) &&
      log_pdh("add queue length", PdhDll::PdhAddCounter(query, QUEUE_LENGTH, 0, &queue_length_counter)) &&
      log_pdh("add processor time", PdhDll::PdhAddCounter(query, PROCESSOR_TIME, 0, &processor_time_counter)) &&
      log_pdh("collect data", PdhDll::PdhCollectQueryData(query));
    proc_scale_factor = 100.0 / MAX2(1, os::processor_count());
  }
  else {
    if (log_pdh("collect data", PdhDll::PdhCollectQueryData(query)) &&
      log_pdh("format queue length", PdhDll::PdhGetFormattedCounterValue(queue_length_counter, PDH_FMT_DOUBLE, nullptr, &queue_length)) &&
      log_pdh("format processor time", PdhDll::PdhGetFormattedCounterValue(processor_time_counter, PDH_FMT_DOUBLE, nullptr, &processor_time))) {
      log_debug(vitals)("Queue lengt. %d, processor time %d", (int)queue_length.doubleValue, (int)processor_time.doubleValue);
      load_avg = processor_time.doubleValue + queue_length.doubleValue * proc_scale_factor;
    }
  }

  return load_avg;
}

static void initialize_pdh() {
  if (!PdhDll::PdhAttach()) {
    log_debug(vitals)("Could not attach pdh lib.");
    return;
  }

  get_load_average_impl(true);
}

bool platform_columns_initialize() {
  initialize_pdh();

  g_col_system_memoryload =
      define_column<PlainValueColumn>("system", nullptr, "mload", "Approximate percentage of physical memory that is in use.", true, MAX);

  // MEMORYSTATUSEX ullAvailPhys
  g_col_system_avail_phys =
      define_column<MemorySizeColumn>("system", nullptr, "avail-phys", "Amount of physical memory currently available.", true, MIN);

  g_col_system_load_average =
    define_column<MemorySizeColumn>("system", nullptr, "la", "Load average of system in percent.", has_loadavg, MAX);

  // PROCESS_MEMORY_COUNTERS_EX WorkingSetSize
  g_col_process_working_set_size =
      define_column<MemorySizeColumn>("system", nullptr, "wset", "Working set size", true);

  // PROCESS_MEMORY_COUNTERS_EX PrivateUsage
  g_col_process_commit_charge =
      define_column<MemorySizeColumn>("system", nullptr, "comch", "Commit charge", true);

  return true;
}

static void set_value_in_sample(Column* col, Sample* sample, value_t val) {
  if (col != nullptr) {
    int index = col->index();
    sample->set_value(index, val);
  }
}

static double get_load_average() {
  if (!has_loadavg) {
    return -1.0;
  }

  return get_load_average_impl(false);
}

void sample_platform_values(Sample* sample, Sample* long_term_sample) {
  double load_avg = get_load_average();

  add_load_average(load_avg);
  set_value_in_sample(g_col_system_load_average, sample, load_avg);

  if (long_term_sample != nullptr) {
    set_value_in_sample(g_col_system_load_average, sample, get_long_term_load_average());
  }

  MEMORYSTATUSEX mse;
  mse.dwLength = sizeof(mse);
  if (::GlobalMemoryStatusEx(&mse)) {
    set_value_in_sample(g_col_system_memoryload, sample, mse.dwMemoryLoad);
    set_value_in_sample(g_col_system_avail_phys, sample, mse.ullAvailPhys);
  }

  PROCESS_MEMORY_COUNTERS cnt;
  cnt.cb = sizeof(cnt);
  if (::GetProcessMemoryInfo(::GetCurrentProcess(), &cnt, sizeof(cnt))) {
    set_value_in_sample(g_col_process_working_set_size, sample, cnt.WorkingSetSize);
    set_value_in_sample(g_col_process_commit_charge, sample, cnt.PagefileUsage);
  }
}

} // namespace sapmachine_vitals
