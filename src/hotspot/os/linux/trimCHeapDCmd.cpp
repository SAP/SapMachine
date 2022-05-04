/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

// SapMachine 2021-09-06: This file originates from cherry-picking
// 8268893: "jcmd to trim the glibc heap" but note that I did not downport
// that patch verbatim. The function "query_process_info", which upstream
// lives in the os:: namespace and in os_linux.hpp/cpp, I copied to this file
// locally. The intent is to reduce merging effort and to keep this cherry-picked
// patch easy to merge between the different SapMachine versions.
//
// Should we officially downport 8268893, we can remove the local version of
// query_process_info().

#include "precompiled.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"
#include "trimCHeapDCmd.hpp"

#include <malloc.h>

// Output structure for query_process_memory_info()
struct meminfo_t {
  ssize_t vmsize;     // current virtual size
  ssize_t vmpeak;     // peak virtual size
  ssize_t vmrss;      // current resident set size
  ssize_t vmhwm;      // peak resident set size
  ssize_t vmswap;     // swapped out
  ssize_t rssanon;    // resident set size (anonymous mappings, needs 4.5)
  ssize_t rssfile;    // resident set size (file mappings, needs 4.5)
  ssize_t rssshmem;   // resident set size (shared mappings, needs 4.5)
};

// Attempts to query memory information about the current process and return it in the output structure.
// May fail (returns false) or succeed (returns true) but not all output fields are available; unavailable
// fields will contain -1.
static bool query_process_memory_info(meminfo_t* info) {
  FILE* f = ::fopen("/proc/self/status", "r");
  const int num_values = sizeof(meminfo_t) / sizeof(size_t);
  int num_found = 0;
  char buf[256];
  info->vmsize = info->vmpeak = info->vmrss = info->vmhwm = info->vmswap =
      info->rssanon = info->rssfile = info->rssshmem = -1;
  if (f != NULL) {
    while (::fgets(buf, sizeof(buf), f) != NULL && num_found < num_values) {
      if ( (info->vmsize == -1    && sscanf(buf, "VmSize: " SSIZE_FORMAT " kB", &info->vmsize) == 1) ||
           (info->vmpeak == -1    && sscanf(buf, "VmPeak: " SSIZE_FORMAT " kB", &info->vmpeak) == 1) ||
           (info->vmswap == -1    && sscanf(buf, "VmSwap: " SSIZE_FORMAT " kB", &info->vmswap) == 1) ||
           (info->vmhwm == -1     && sscanf(buf, "VmHWM: " SSIZE_FORMAT " kB", &info->vmhwm) == 1) ||
           (info->vmrss == -1     && sscanf(buf, "VmRSS: " SSIZE_FORMAT " kB", &info->vmrss) == 1) ||
           (info->rssanon == -1   && sscanf(buf, "RssAnon: " SSIZE_FORMAT " kB", &info->rssanon) == 1) || // Needs Linux 4.5
           (info->rssfile == -1   && sscanf(buf, "RssFile: " SSIZE_FORMAT " kB", &info->rssfile) == 1) || // Needs Linux 4.5
           (info->rssshmem == -1  && sscanf(buf, "RssShmem: " SSIZE_FORMAT " kB", &info->rssshmem) == 1)  // Needs Linux 4.5
           )
      {
        num_found ++;
      }
    }
    fclose(f);
    return true;
  }
  return false;
}

void TrimCLibcHeapDCmd::execute(DCmdSource source, TRAPS) {
#ifdef __GLIBC__
  stringStream ss_report(1024); // Note: before calling trim

  meminfo_t info1;
  meminfo_t info2;
  // Query memory before...
  bool have_info1 = query_process_memory_info(&info1);

  _output->print_cr("Attempting trim...");
  ::malloc_trim(0);
  _output->print_cr("Done.");

  // ...and after trim.
  bool have_info2 = query_process_memory_info(&info2);

  // Print report both to output stream as well to UL
  bool wrote_something = false;
  if (have_info1 && have_info2) {
    if (info1.vmsize != -1 && info2.vmsize != -1) {
      ss_report.print_cr("Virtual size before: " SSIZE_FORMAT "k, after: " SSIZE_FORMAT "k, (" SSIZE_FORMAT "k)",
                         info1.vmsize, info2.vmsize, (info2.vmsize - info1.vmsize));
      wrote_something = true;
    }
    if (info1.vmrss != -1 && info2.vmrss != -1) {
      ss_report.print_cr("RSS before: " SSIZE_FORMAT "k, after: " SSIZE_FORMAT "k, (" SSIZE_FORMAT "k)",
                         info1.vmrss, info2.vmrss, (info2.vmrss - info1.vmrss));
      wrote_something = true;
    }
    if (info1.vmswap != -1 && info2.vmswap != -1) {
      ss_report.print_cr("Swap before: " SSIZE_FORMAT "k, after: " SSIZE_FORMAT "k, (" SSIZE_FORMAT "k)",
                         info1.vmswap, info2.vmswap, (info2.vmswap - info1.vmswap));
      wrote_something = true;
    }
  }
  if (!wrote_something) {
    ss_report.print_raw("No details available.");
  }

  _output->print_raw(ss_report.base());
  log_info(os)("malloc_trim:\n%s", ss_report.base());
#else
  _output->print_cr("Not available.");
#endif
}
