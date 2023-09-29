/*
 * Copyright (c) 2005, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef OS_LINUX_GLOBALS_LINUX_HPP
#define OS_LINUX_GLOBALS_LINUX_HPP

//
// Declare Linux specific flags. They are not available on other platforms.
//
#define RUNTIME_OS_FLAGS(develop,                                       \
                         develop_pd,                                    \
                         product,                                       \
                         product_pd,                                    \
                         notproduct,                                    \
                         range,                                         \
                         constraint)                                    \
                                                                        \
  product(bool, UseOprofile, false,                                     \
        "enable support for Oprofile profiler")                         \
                                                                        \
  /*  NB: The default value of UseLinuxPosixThreadCPUClocks may be   */ \
  /* overridden in Arguments::parse_each_vm_init_arg.                */ \
  product(bool, UseLinuxPosixThreadCPUClocks, true,                     \
          "enable fast Linux Posix clocks where available")             \
                                                                        \
  product(bool, UseHugeTLBFS, false,                                    \
          "Use MAP_HUGETLB for large pages")                            \
                                                                        \
  product(bool, UseTransparentHugePages, false,                         \
          "Use MADV_HUGEPAGE for large pages")                          \
                                                                        \
  product(bool, LoadExecStackDllInVMThread, true,                       \
          "Load DLLs with executable-stack attribute in the VM Thread") \
                                                                        \
  product(bool, UseSHM, false,                                          \
          "Use SYSV shared memory for large pages")                     \
                                                                        \
  product(bool, UseContainerSupport, true,                              \
          "Enable detection and runtime container configuration support") \
                                                                        \
  product(bool, AdjustStackSizeForTLS, false,                           \
          "Increase the thread stack size to include space for glibc "  \
          "static thread-local storage (TLS) if true")                  \
                                                                        \
  product(bool, DumpPrivateMappingsInCore, true, DIAGNOSTIC,            \
          "If true, sets bit 2 of /proc/PID/coredump_filter, thus "     \
          "resulting in file-backed private mappings of the process to "\
          "be dumped into the corefile.")                               \
                                                                        \
  product(bool, DumpSharedMappingsInCore, true, DIAGNOSTIC,             \
          "If true, sets bit 3 of /proc/PID/coredump_filter, thus "     \
          "resulting in file-backed shared mappings of the process to " \
          "be dumped into the corefile.")                               \
                                                                        \
  /* SapMachine 2022-05-01: HiMemReport */                              \
  product(bool, HiMemReport, false,                                     \
         "VM writes a high memory report and optionally execute "       \
         "additional jcmds if its rss+swap reaches 66%, 75% or 90% of " \
         "a maximum. If the VM is containerized, that maximum is the "  \
         "container memory limit at VM start. If the VM is not "        \
         "containerized, that maximum is half of the total physical "   \
         "memory. The maximum can be overridden with "                  \
         "-XX:HiMemReportMaximum=<size>. Per default, the report is "   \
         "printed to stderr. If HiMemReportDir is specified, that "     \
         "report is redirected to \"<report directory>/"                \
         "<sapmachine_himemalert>_pid<pid>_<timestamp>.log\".")         \
  product(size_t, HiMemReportMax, 0,                                    \
         "Overrides the maximum reference size for HiMemReport.")       \
  product(ccstr, HiMemReportDir, NULL,                                  \
         "Specifies a directory into which reports are written. Gets "  \
         "created (one level only) if it does not exist.")              \
  product(ccstr, HiMemReportExec, NULL,                                 \
         "Specifies one or more jcmds to be executed after a high "     \
         "memory report has been written. Multiple commands are "       \
         "separated by ';'. Command output is written to stderr. If "   \
         "HiMemReportDir is specified, command output is redirected to " \
         "\"<report directory>/<command>_pid<pid>_timestamp.(out|err)\"." \
         "If one of the commands is \"GC.heap_dump\" and its "         \
         "arguments are omitted, the heap dump is written as "         \
         "\"GC.heap_dump_pid<pid>_timestamp\" to either report "       \
         "directory or current directory if HiMemReportDir is "        \
         "omitted.\n"                                                  \
         "Example: \"-XX:HiMemReportExec=GC.class_histogram -all;GC.heap_dump\"") \
                                                                        \
  product(bool, UseCpuAllocPath, false, DIAGNOSTIC,                     \
          "Use CPU_ALLOC code path in os::active_processor_count ")     \
                                                                        \
  /* SapMachine 2021-09-01: malloc-trace */                             \
  product(bool, EnableMallocTrace, false, DIAGNOSTIC,                   \
          "Enable malloc trace at VM initialization")                   \
                                                                        \
  /* SapMachine 2021-09-01: malloc-trace */                             \
  product(bool, PrintMallocTraceAtExit, false, DIAGNOSTIC,              \
          "Print Malloc Trace upon VM exit")                            \
																	                                      \
  product(bool, DumpPerfMapAtExit, false, DIAGNOSTIC,                   \
          "Write map file for Linux perf tool at exit")                 \
                                                                        \
  product(intx, TimerSlack, -1, EXPERIMENTAL,                           \
          "Overrides the timer slack value to the given number of "     \
          "nanoseconds. Lower value provides more accurate "            \
          "high-precision timers, at the expense of (possibly) worse "  \
          "power efficiency. In current Linux, 0 means using the "      \
          "system-wide default, which would disable the override, but " \
          "VM would still print the current timer slack values. Use -1 "\
          "to disable both the override and the printouts."             \
          "See prctl(PR_SET_TIMERSLACK) for more info.")                \
                                                                        \
  product(bool, THPStackMitigation, true, DIAGNOSTIC,                   \
          "If THPs are unconditionally enabled on the system (mode "    \
          "\"always\"), the JVM will prevent THP from forming in "      \
          "thread stacks. When disabled, the absence of this mitigation"\
          "allows THPs to form in thread stacks.")                      \
                                                                        \
  develop(bool, DelayThreadStartALot, false,                            \
          "Artificially delay thread starts randomly for testing.")     \
                                                                        \


// end of RUNTIME_OS_FLAGS

//
// Defines Linux-specific default values. The flags are available on all
// platforms, but they may have different default values on other platforms.
//
define_pd_global(size_t, PreTouchParallelChunkSize, 4 * M);
define_pd_global(bool, UseLargePages, false);
define_pd_global(bool, UseLargePagesIndividualAllocation, false);
define_pd_global(bool, UseThreadPriorities, true) ;

#endif // OS_LINUX_GLOBALS_LINUX_HPP
