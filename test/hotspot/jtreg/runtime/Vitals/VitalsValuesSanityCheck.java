/*
 * Copyright (c) 2022, SAP SE. All rights reserved.
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
 */

/*
 * @test
 * @summary Test that Vitals memory sizes are within expectations
 * @requires os.family == "linux"
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @build sun.hotspot.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller sun.hotspot.WhiteBox
 * @run testng/othervm -XX:+UseSerialGC -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xmx64m -Xms64m -XX:NativeMemoryTracking=summary -XX:VitalsSampleInterval=1 VitalsValuesSanityCheck
 */

/*
 * @test
 * @summary Test that Vitals memory sizes are within expectations
 * @requires os.family == "linux"
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @build sun.hotspot.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller sun.hotspot.WhiteBox
 * @run testng/othervm -XX:+UseG1GC -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xmx64m -Xms64m -XX:NativeMemoryTracking=summary -XX:VitalsSampleInterval=1 VitalsValuesSanityCheck
 */

/*
 * @test
 * @summary Test that Vitals memory sizes are within expectations
 * @requires os.family == "linux"
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @build sun.hotspot.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller sun.hotspot.WhiteBox
 * @run testng/othervm -XX:+UseShenandoahGC -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xmx64m -Xms64m -XX:NativeMemoryTracking=summary -XX:VitalsSampleInterval=1 VitalsValuesSanityCheck
 */

import jdk.test.lib.Platform;
import jdk.test.lib.dcmd.CommandExecutor;
import jdk.test.lib.dcmd.JMXExecutor;
import jdk.test.lib.process.OutputAnalyzer;
import sun.hotspot.WhiteBox;
import org.testng.annotations.Test;

public class VitalsValuesSanityCheck {

    final long M = 1024 * 1024;

    // total size of what we will allocate from the cheap in the main function
    final long cheapAllocationSize = 32 * M;
    // individual block size, small enough to be guaranteed touched and counting toward rss
    final long cheapAllocationBlockSize = 1024;
    final long numCheapAllocations = cheapAllocationSize / cheapAllocationBlockSize;

    private static long findHighestValueForColumn(CSVParser.CSV csv, String colname) {
        int index = csv.header.findColumn(colname);
        if (index == -1) {
            throw new RuntimeException("column not found: "  + colname);
        }
        long l = 0;
        int i = 0;
        for (i = 0; i < csv.lines.length; i ++) {
            CSVParser.CSVDataLine line = csv.lines[i];
            long l2 = line.numberAt(index);
            if (l2 > l) {
                l = l2;
            }
        }
        System.out.println("Found highest value for " + colname + " in line " + i + ": " + l);
        return l;
    }

    public void run(CommandExecutor executor) {

        try {

            // We read the vitals in csv form and get the parsed output.
            // The heap (64m) should show up as full committed. It is also touched, so it should contribute to RSS unless we swap
            // The VM will have allocated 32M C-heap as well, in 1KB chunks. These should show up in NMT, in the cheap columns
            // as well as in rss.
            OutputAnalyzer output = executor.execute("VM.vitals csv raw");
            VitalsTestHelper.outputMatchesVitalsCSVMode(output);
            output.shouldNotContain("Now"); // off by default
            CSVParser.CSV csv = VitalsTestHelper.parseAndCheckCSV(output, true);

            // Java heap
            long highest_jvm_heap_comm = findHighestValueForColumn(csv, "jvm-heap-comm");
            long highest_jvm_heap_used = findHighestValueForColumn(csv, "jvm-heap-used");
            if (highest_jvm_heap_comm < (M * 58)) {
                throw new RuntimeException("jvm-heap-comm too low");
            }
            if (highest_jvm_heap_comm > (M * 68)) {
                throw new RuntimeException("jvm-heap-comm too high");
            }
            if (highest_jvm_heap_used <= 0 || highest_jvm_heap_used > highest_jvm_heap_comm) {
                throw new RuntimeException("jvm-heap-used looks off");
            }

            // Class+nonclass metaspace
            long highest_jvm_meta_comm = findHighestValueForColumn(csv, "jvm-meta-comm");
            long highest_jvm_meta_used = findHighestValueForColumn(csv, "jvm-meta-used");
            if (highest_jvm_meta_comm <= 0 || highest_jvm_meta_used <= 0 || highest_jvm_meta_used > highest_jvm_meta_comm) {
                throw new RuntimeException("jvm-meta-comm or jvm-meta-used look off");
            }

            // Class space
            if (Platform.is64bit()) {
                long highest_jvm_meta_csc = findHighestValueForColumn(csv, "jvm-meta-csc");
                long highest_jvm_meta_csu = findHighestValueForColumn(csv, "jvm-meta-csu");
                // Class space size is contained within jvm-meta-comm, and non-class should be != 0
                if (highest_jvm_meta_csc <= 0 || highest_jvm_meta_csc >= highest_jvm_meta_comm) {
                    throw new RuntimeException("jvm-meta-csc looks off");
                }
                if (highest_jvm_meta_csu > highest_jvm_meta_csc) {
                    throw new RuntimeException("jvm-meta-csu looks off");
                }
            }

            // Code cache
            long highest_jvm_code_cache = findHighestValueForColumn(csv, "jvm-code");
            if (highest_jvm_code_cache <= 0) {
                throw new RuntimeException("jvm-code looks off");
            }

            // NMT
            long highest_jvm_nmt_mlc = findHighestValueForColumn(csv, "jvm-nmt-mlc");
            long highest_jvm_nmt_map = findHighestValueForColumn(csv, "jvm-nmt-map");
            // NMT mlc should contain all committed space done by malloc. We only know for sure that
            // the heap is fully committed, and that we used some code cache. So it should show at least that.
            if (highest_jvm_nmt_map < (highest_jvm_heap_comm + highest_jvm_code_cache)) {
                throw new RuntimeException("jvm-nmt-map looks off");
            }
            // We malloced at least 64M, and the VM also uses some.
            long expected_minimal_cheap_usage_jvm = 2 * M;
            long expected_minimal_cheap_usage = cheapAllocationSize + expected_minimal_cheap_usage_jvm;
            if (highest_jvm_nmt_mlc < expected_minimal_cheap_usage) {
                throw new RuntimeException("jvm-nmt-mlc too low");
            }
            // ... but not much more than ten times that
            if (highest_jvm_nmt_mlc > (expected_minimal_cheap_usage * 10)) {
                throw new RuntimeException("jvm-nmt-mlc seems high");
            }

            // Number of jvm threads: we expect at least 4-5 in total, with at least one non-deamon thread (the one we run in right now)
            long min_expected_java_threads = 5; // probably more
            long max_expected_java_threads = 1000; // probably way less, but lets go with this.
            long highest_jvm_jthr_num = findHighestValueForColumn(csv, "jvm-jthr-num");
            long highest_jvm_jthr_nd = findHighestValueForColumn(csv, "jvm-jthr-nd");
            if (highest_jvm_jthr_num < min_expected_java_threads || highest_jvm_jthr_num > max_expected_java_threads) {
                throw new RuntimeException("jvm-jthr-num seems off");
            }
            if (highest_jvm_jthr_nd < 1 || highest_jvm_jthr_nd > highest_jvm_jthr_num) {
                throw new RuntimeException("jvm-jthr-nd seems off");
            }

            // Classes:
            long min_expected_classes_base = 300; // probably more
            long max_expected_classes_base = 60000; // probably way less
            long highest_jvm_cls_num = findHighestValueForColumn(csv, "jvm-cls-num");
            if (highest_jvm_cls_num < min_expected_classes_base || highest_jvm_cls_num > max_expected_classes_base) {
                throw new RuntimeException("jvm_cls_num seems off");
            }
            // We do not test highest_jvm_cls_ld and ul since those are delta cols and may not show in all samples

            // Linux specific platform columns
            if (Platform.isLinux()) {

                // Check --- system --- cols on Linux

                // Number of processes and kernel threads. In containers shows the local processes only. But we should have
                // at least one, us, and multiple kernel threads, since we are multithreaded.
                long min_expected_processes = 1;
                long max_expected_processes = 1000000000; // Anything above a billion would surprise me
                long min_expected_kernel_threads = min_expected_java_threads;
                long max_expected_kernel_threads = 1000000000; // same here
                long highest_proc_p = findHighestValueForColumn(csv, "syst-p");
                long highest_proc_t = findHighestValueForColumn(csv, "syst-t");
                if (highest_proc_p < min_expected_processes || highest_proc_p > max_expected_processes) {
                    throw new RuntimeException("proc-p seems off");
                }
                if (highest_proc_t < min_expected_kernel_threads || highest_proc_t > max_expected_kernel_threads) {
                    throw new RuntimeException("proc-t seems off");
                }
                if (highest_proc_p > highest_proc_t) {
                    throw new RuntimeException("cannot have more processes than kernel threads");
                }

                // processes running, processes blocked.
                long highest_proc_pr = findHighestValueForColumn(csv, "syst-pr");
                long highest_proc_pb = findHighestValueForColumn(csv, "syst-pb");
                if (highest_proc_pr > highest_proc_p || highest_proc_pb > highest_proc_p) {
                    throw new RuntimeException("proc-p(r|b) seem off");
                }
                // Sum of running and blocked should be at least 1 since we are running
                if (highest_proc_pr == 0 && highest_proc_pb == 0) {
                    throw new RuntimeException("At least one process should show up (proc-p and proc-t both 0)");
                }

                // Cgroup
                // We may not always show this. But if we do, at least the usage numbers should be checked
                if (csv.header.findColumn("syst-cgro-usg") != -1) {
                    if (findHighestValueForColumn(csv, "syst-cgro-usg") <= 0) {
                        throw new RuntimeException("syst-cgro-usg seems off");
                    }
                }
                if (csv.header.findColumn("syst-cgro-usgsw") != -1) {
                    if (findHighestValueForColumn(csv, "syst-cgro-usgsw") <= 0) {
                        throw new RuntimeException("syst-cgro-usgsw seems off");
                    }
                }
                if (csv.header.findColumn("syst-cgro-kusg") != -1) {
                    if (findHighestValueForColumn(csv, "syst-cgro-kusg") <= 0) {
                        throw new RuntimeException("syst-cgro-kusg seems off");
                    }
                }

                // Check --- process --- cols on Linux
                long highest_proc_virt = findHighestValueForColumn(csv, "proc-virt");
                long highest_proc_rss_all = findHighestValueForColumn(csv, "proc-rss-all");
                long highest_proc_proc_swdo = findHighestValueForColumn(csv, "proc-swdo");
                long highest_rss_plus_swap = highest_proc_rss_all + highest_proc_proc_swdo;
                long min_expected_proc_virt = 128 * M; // probably way more
                long max_expected_proc_virt = 128 * 1024 * M; // probably way less
                long min_expected_proc_rss = expected_minimal_cheap_usage;   // since we touch the malloced 64*m, we should see at least that.
                long max_expected_proc_rss = 8 * 1024 * M; // probably way less
                if (highest_proc_virt < min_expected_proc_virt || highest_proc_virt > max_expected_proc_virt) {
                    throw new RuntimeException("proc-virt seems off");
                }
                if (highest_rss_plus_swap < min_expected_proc_rss || highest_rss_plus_swap > max_expected_proc_rss) {
                    throw new RuntimeException("proc-rss-all + swap seems off");
                }
                if (highest_rss_plus_swap > highest_proc_virt) {
                    throw new RuntimeException("rss+swap cannot be larger than virt");
                }

                // -cheap--
                // We know we allocated 64M, those should show up in the cheap used display
                long highest_proc_chea_usd = findHighestValueForColumn(csv, "proc-chea-usd");
                long highest_proc_chea_free = findHighestValueForColumn(csv, "proc-chea-free");
                if (highest_proc_chea_usd < expected_minimal_cheap_usage) {
                    throw new RuntimeException("proc-chea-usd seems low");
                }
                if (highest_proc_chea_usd > max_expected_proc_rss) {
                    throw new RuntimeException("proc-chea-usd seems high");
                }
                if (highest_proc_chea_free < 0 || highest_proc_chea_free > max_expected_proc_rss) {
                    throw new RuntimeException("proc-chea-free seems off");
                }

                // Number of open file descriptors
                long highest_io_proc_of = findHighestValueForColumn(csv, "proc-io-of");
                if (highest_io_proc_of < 0 || highest_io_proc_of > 1 * M) {
                    throw new RuntimeException("proc-io-of seems off");
                }

                // Number of threads in this process (should be larger than java threads, but lets go with at least equal to)
                long highest_proc_thr = findHighestValueForColumn(csv, "proc-thr");
                if (highest_proc_thr < highest_jvm_jthr_num || highest_proc_thr > 1 * M) {
                    throw new RuntimeException("proc-thr seems off");
                }

            } // end: linux specific sanity tests

        } catch (Exception e) {
            e.printStackTrace();
            throw new RuntimeException(e.getMessage());
        }

    }

    @Test
    public void jmx() {
        WhiteBox wb = WhiteBox.getWhiteBox();
        for (int i = 0; i < numCheapAllocations; i ++) {
            long p = wb.NMTMalloc(cheapAllocationBlockSize);
        }
        try {
            Thread.sleep(2000);
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
        run(new JMXExecutor());
    }

}
