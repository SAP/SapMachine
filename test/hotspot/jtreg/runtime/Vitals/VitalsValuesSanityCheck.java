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
 * @test Serial
 * @summary Test that Vitals memory sizes are within expectations
 * @requires os.family == "linux"
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run testng/othervm -XX:+UseSerialGC -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xmx64m -Xms64m -XX:NativeMemoryTracking=summary -XX:VitalsSampleInterval=1 VitalsValuesSanityCheck
 */

/*
 * @test G1
 * @summary Test that Vitals memory sizes are within expectations
 * @requires os.family == "linux"
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run testng/othervm -XX:+UseG1GC -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xmx64m -Xms64m -XX:NativeMemoryTracking=summary -XX:VitalsSampleInterval=1 VitalsValuesSanityCheck
 */

/*
 * @test G1_no_nmt
 * @summary Test that Vitals memory sizes are within expectations
 * @requires os.family == "linux"
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run testng/othervm -XX:+UseG1GC -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xmx64m -Xms64m -XX:NativeMemoryTracking=off -XX:VitalsSampleInterval=1 VitalsValuesSanityCheck
 */

import jdk.test.lib.Platform;
import jdk.test.lib.dcmd.CommandExecutor;
import jdk.test.lib.dcmd.JMXExecutor;
import jdk.test.lib.process.OutputAnalyzer;
import org.testng.annotations.Test;
import jdk.test.whitebox.WhiteBox;

import java.io.File;
import java.io.IOException;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class VitalsValuesSanityCheck {

    static WhiteBox wb = WhiteBox.getWhiteBox();

    final long K = 1024;
    final long M = 1024 * K;
    final long G = 1024 * M;

    // total size of what we will allocate from the cheap in the main function
    final long cheapAllocationSize = 32 * M;
    // individual block size, small enough to be guaranteed touched and counting toward rss
    final long cheapAllocationBlockSize = 1024;
    final long numCheapAllocations = cheapAllocationSize / cheapAllocationBlockSize;


    private static void assertColumnExists(CSVParser.CSV csv, String colname) {
        if (!csv.header.hasColumn(colname)) {
            throw new RuntimeException("Column " + colname + " not found");
        }
    }

    /**
     * scan the CSV for the highest value in a column.
     *
     * @param csv
     * @param colname
     * @return highest value, or -1 on error (either the column was not found, or no cell in the column contain a valid value)
     */
    private static long findHighestValueForColumn(CSVParser.CSV csv, String colname) {
        int index = csv.header.findColumn(colname);
        if (index == -1) {
            throw new RuntimeException("cannot find column " + colname);
        }
        long max = -1;
        int line_containing_max = -1;
        int i = 0;
        for (i = 0; i < csv.lines.length; i++) {
            CSVParser.CSVDataLine line = csv.lines[i];
            if (!line.isEmpty(index)) {
                long l2 = line.numberAt(index);
                if (l2 > max) {
                    max = l2;
                    line_containing_max = i;
                }
            }
        }
        if (max == -1) {
            System.out.println("Found no valid value " + colname);
        } else {
            System.out.println("Found highest value for " + colname + " in line " + line_containing_max + ": " + max);
        }
        return max;
    }

    /**
     * Check that a certain value is between [min and max).
     * @param colname
     * @param min
     * @param max
     */
    private static void checkValueIsBetween(long value, String colname, long min, long max) {
        if (value < min) {
            throw new RuntimeException(colname + " seems too low (expected at least " + min + ")");
        } else if (value >= max) {
            throw new RuntimeException(colname + " seems too high (expected at most " + max + ")");
        }
    }

    /**
     * Check that a certain value exists (column exists), look for the highest value found,
     * and check that it is between [min and max).
     * For convenience, we also return that highest value.
     *
     * @param colname
     * @param min
     * @param max
     * @return the highest value
     */
    private static long checkValueIsBetween(CSVParser.CSV csv, String colname, long min, long max) {
        long l = findHighestValueForColumn(csv, colname);
        checkValueIsBetween(l, colname, min, max);
        return l;
    }

    public void run(CommandExecutor executor) {

        try {

            final long veryLowButNot0 = K;
            // very high but still far away from 0x80000000_00000000
            final long veryVeryHigh = 256 * G;

            // We malloced at least xxx M, and the VM also uses some.
            long expected_minimal_cheap_usage_jvm = 2 * M;
            long expected_minimal_cheap_usage = cheapAllocationSize + expected_minimal_cheap_usage_jvm;

            // We read the vitals in csv form and get the parsed output.
            // Note to self: don't use raw! It does display delta values as accumulating absolutes.
            // The heap (64m) should show up as full committed. It is also touched, so it should contribute to RSS unless we swap
            // The VM will have allocated 32M C-heap as well, in 1KB chunks. These should show up in NMT, in the cheap columns
            // as well as in rss.
            OutputAnalyzer output = executor.execute("VM.vitals csv scale=1");
            VitalsTestHelper.outputMatchesVitalsCSVMode(output);
            output.shouldNotContain("Now"); // off by default
            CSVParser.CSV csv = VitalsTestHelper.parseCSV(output);

            VitalsTestHelper.simpleCSVSanityChecks(csv);

            // Java heap (we specify 64M, but depending on GC this can wobble a bit)
            long highest_expected_jvm_heap_comm = M * 68;
            long lowest_expected_jvm_heap_comm = M * 60;
            long jvm_heap_comm = checkValueIsBetween(csv, "jvm-heap-comm", lowest_expected_jvm_heap_comm, highest_expected_jvm_heap_comm);
            // check heap used
            long jvm_heap_used = checkValueIsBetween(csv, "jvm-heap-used", veryLowButNot0, jvm_heap_comm);

            // Class+nonclass metaspace
            long highest_expected_metaspace_comm = 1 * G; // for this little programm, nothing more
            long jvm_meta_comm = checkValueIsBetween(csv, "jvm-meta-comm", veryLowButNot0, highest_expected_metaspace_comm);
            long jvm_meta_used = checkValueIsBetween(csv, "jvm-meta-used", veryLowButNot0, jvm_meta_comm);

            // Class space
            if (Platform.is64bit()) {
                long highest_expected_classspace_comm = highest_expected_metaspace_comm / 2;
                long jvm_meta_csc = checkValueIsBetween(csv, "jvm-meta-csc", veryLowButNot0, highest_expected_classspace_comm);
                checkValueIsBetween(csv, "jvm-meta-csu", veryLowButNot0, jvm_meta_csc);
            }

            // Code cache
            long jvm_code = checkValueIsBetween(csv, "jvm-code", veryLowButNot0, 2 * G);

            // How much we know for now the JVM mapped for sure
            long jvm_mapped_this_much_at_least = jvm_heap_comm + jvm_code + jvm_meta_comm;

            // How much we think the jvm has touched (RSS) at least.
            long jvm_touched_this_much_at_least = (jvm_heap_used + jvm_meta_used) / 2;

            long jvm_highest_expected_cheap_usage = expected_minimal_cheap_usage * 10;

            // NMT (may be off)
            long jvm_nmt_mlc = -1;
            if (csv.header.hasColumn("jvm-nmt-mlc")) {
                // NMT committed maps:
                long highest_expected_nmt_mmap = jvm_mapped_this_much_at_least + 2 * G; // very generous
                long lowest_expected_nmt_mmap = jvm_mapped_this_much_at_least;
                checkValueIsBetween(csv, "jvm-nmt-map", lowest_expected_nmt_mmap, highest_expected_nmt_mmap);
                // NMT malloc:
                jvm_nmt_mlc = checkValueIsBetween(csv, "jvm-nmt-mlc", expected_minimal_cheap_usage, jvm_highest_expected_cheap_usage);
                checkValueIsBetween(csv, "jvm-nmt-ovh", veryLowButNot0, jvm_nmt_mlc);
                checkValueIsBetween(csv, "jvm-nmt-gc", veryLowButNot0, jvm_nmt_mlc);
                checkValueIsBetween(csv, "jvm-nmt-oth", veryLowButNot0, jvm_nmt_mlc);

            }

            // Number of jvm threads: we expect at least 4-5 in total, with at least one non-deamon thread (the one we run in right now)
            long min_expected_java_threads = 5; // probably more
            long max_expected_java_threads = 1000; // probably way less, but lets go with this.
            long jvm_thr_num = checkValueIsBetween(csv, "jvm-jthr-num", min_expected_java_threads, max_expected_java_threads);
            // we expect at least 1 non-deamon thread (me)
            checkValueIsBetween(csv, "jvm-jthr-nd", 1, jvm_thr_num);
            // How many threads were created in the last second (delta col)?
            checkValueIsBetween(csv, "jvm-jthr-cr", 0, 10000);

            // Classloaders:
            long min_expected_classloaders = 1; // probably more
            long max_expected_classloaders = 100000; // probably way less, not sure how many lambdas are active, dep. on jvm version they show up as cldg entries
            long jvm_clg_num = checkValueIsBetween(csv, "jvm-cldg-num", min_expected_classloaders, max_expected_classloaders);
            checkValueIsBetween(csv, "jvm-cldg-anon", 0, jvm_clg_num);

            // Classes:
            long min_expected_classes_base = 300; // probably more
            long max_expected_classes_base = 60000; // probably way less
            long jvm_cls_num = checkValueIsBetween(csv, "jvm-cls-num", min_expected_classes_base, max_expected_classes_base);
            // cls-ld and cls-uld are delta values!
            // Unless loading is insanely slow, I'd expect at least two-digit loads per second (vitals interval)
            checkValueIsBetween(csv, "jvm-cls-ld", 10, max_expected_classes_base);
            // I don't think we have unloaded yet
            checkValueIsBetween(csv, "jvm-cls-uld", 0, max_expected_classes_base);

            // Linux specific platform columns
            if (Platform.isLinux()) {

                // Check --- system --- cols on Linux

                long highestExpectedMemoryValue = 512 * G; // I wish...
                if (csv.header.hasColumn("syst-avail")) {
                    checkValueIsBetween(csv, "syst-avail", M, highestExpectedMemoryValue);
                }

                long syst_comm = checkValueIsBetween(csv, "syst-comm", M, highestExpectedMemoryValue);
                checkValueIsBetween(csv, "syst-crt", 1, 10000); // its a percentage; anything above ~150 is very unlikely unless we aggressivly overcommit
                checkValueIsBetween(csv, "syst-swap", 0, highestExpectedMemoryValue);
                // si, so: number of pages, delta
                checkValueIsBetween(csv, "syst-si", 0, 100000);
                checkValueIsBetween(csv, "syst-so", 0, 100000);

                // Number of processes and kernel threads. In containers shows the local processes only. But we should have
                // at least one, us, and multiple kernel threads, since we are multithreaded.
                long min_expected_processes = 1;
                long max_expected_processes = 1000000000; // Anything above a billion would surprise me
                checkValueIsBetween(csv, "syst-p", min_expected_processes, max_expected_processes);

                long min_expected_kernel_threads = min_expected_java_threads;
                long max_expected_kernel_threads = 1000000000; // same here
                long syst_t = checkValueIsBetween(csv, "syst-t", min_expected_kernel_threads, max_expected_kernel_threads);

                // threads running, blocked on disk IO (cannot be larger than number of kernel threads)
                checkValueIsBetween(csv, "syst-tr", 0, syst_t);
                checkValueIsBetween(csv, "syst-tb", 0, syst_t);

                // Cgroup
                // We may not always show this. But if we do, at least the usage numbers should be checked
                if (csv.header.hasColumn("syst-cgro-usg")) {
                    checkValueIsBetween(csv, "syst-cgro-usg", veryLowButNot0, highestExpectedMemoryValue);
                }
//                Completely disable this test because hunting down erros here is exhausting. Many kernels don't seem to show
//                kernel memory values. So this may not show off at all, or show a column without value.
//                if (csv.header.hasColumn("syst-cgro-kusg")) {
//                    // kusg can get surprisingly high, e.g. if ran on a host hosting guest VMs (seen that with virtual box)
//                    // ... and it can be 0 too for some reason, seen on our loaner ppcle machines at Adopt
//                    checkValueIsBetween(csv, "syst-cgro-kusg", 0, highestExpectedMemoryValue);
//                }

                // Check --- process --- cols on Linux
                long proc_virt = checkValueIsBetween(csv, "proc-virt", jvm_mapped_this_much_at_least, 100 * G); // virt size can get crazy
                long proc_rss_all = checkValueIsBetween(csv, "proc-rss-all", jvm_touched_this_much_at_least, proc_virt);
                if (csv.header.hasColumn("proc-rss-anon")) {
                    checkValueIsBetween(csv, "proc-rss-anon", jvm_touched_this_much_at_least, proc_rss_all); // most JVM mappings are anon
                    checkValueIsBetween(csv, "proc-rss-file", 0, proc_rss_all);
                    checkValueIsBetween(csv, "proc-rss-shm", 0, proc_rss_all);
                }

                checkValueIsBetween(csv, "proc-swdo", 0, proc_virt);

                // -cheap--
                if (!Platform.isMusl() && proc_rss_all < 4 * G) {
                    // we expect to see what NMT sees, plus a bit, since glibc has overhead. If NMT is off, we use
                    // our initial ballpark number.
                    long min_c_heap_usage_glibc = (jvm_nmt_mlc == -1 ? jvm_nmt_mlc : expected_minimal_cheap_usage) + K;
                    long max_c_heap_usage_glibc = min_c_heap_usage_glibc + (2 * G); // glibc has crazy overhead
                    // but cannot be larger than virt ever was
                    if (max_c_heap_usage_glibc > proc_virt) {
                        max_c_heap_usage_glibc = proc_virt - M; // bit less since a lot of stuff is not malloc
                    }
                    checkValueIsBetween(csv, "proc-chea-usd", min_c_heap_usage_glibc, max_c_heap_usage_glibc);
                    checkValueIsBetween(csv, "proc-chea-free", 0, max_c_heap_usage_glibc);
                }

                // Counter-check NMT mlc vs rss
                // "mlc" can be higher than RSS in release builds, since large malloc blocks may not be fully touched.
                // However, in debug builds os::malloc inits all malloced blocks, so - apart from a few raw mallocs here and there -
                // we should not see mlc > rss
                if (jvm_nmt_mlc != -1) {
                    if (Platform.isDebugBuild()) {
                        if (proc_rss_all < jvm_nmt_mlc) {
                            throw new RuntimeException("NMT mlc higher than RSS?");
                        }
                    }
                }

                checkValueIsBetween(csv, "proc-cpu-us", 0, 100000);
                checkValueIsBetween(csv, "proc-cpu-sy", 0, 100000);

                // Number of open file descriptors
                // (at least one, since we write to stdout. Probably not many more.
                checkValueIsBetween(csv, "proc-io-of", 1, 1000);

                // IO read, written (note: deltas, so its "read, written, in one second").
                checkValueIsBetween(csv, "proc-io-rd", 0, 1 * G);
                checkValueIsBetween(csv, "proc-io-wr", 0, 10 * G);

                // Number of threads in this process (java + native)
                checkValueIsBetween(csv, "proc-thr", jvm_thr_num, jvm_thr_num + 100);

            } // end: linux specific sanity tests

        } catch (Exception e) {
            e.printStackTrace();
            throw new RuntimeException(e.getMessage());
        }

    }

    @Test
    public void jmx() {
        for (int i = 0; i < numCheapAllocations; i++) {
            long p = wb.NMTMalloc(cheapAllocationBlockSize);
        }
        try {
            // wait some time. We sample with 1sec sample frequency, that should give us more than one sample
            // and therefore some of them should show delta values
            Thread.sleep(4000);
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
        run(new JMXExecutor());
    }

}
