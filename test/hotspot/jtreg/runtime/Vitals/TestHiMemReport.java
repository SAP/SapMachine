/*
 * Copyright (c) 2021, 2023 SAP SE. All rights reserved.
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
 * @test TestHiMemReport-print
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReport print
 */

/*
 * @test TestHiMemReport-dump
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReport dump
 */

/*
 * @test TestHiMemReport-dump-with-exec-to-reportdir
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReport dump-with-exec-to-reportdir
 */

/*
 * @test TestHiMemReport-dump-with-exec-to-stderr
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReport dump-with-exec-to-stderr
 */

/*
 * @test TestHiMemReport-natural-max
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReport natural-max
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.io.File;
import java.io.IOException;

public class TestHiMemReport {

    // Match the output file suffix we generate with the HiMemReportFacility
    // (xxx_pid<pid>_<year>_<month>_<day>_<hour>_<minute>_<second>.yyy)
    static final String patterFileSuffix = "_pid\\d+_\\d+_\\d+_\\d+_\\d+_\\d+_\\d+";

    // These tests are very simple. We start the VM with a footprint (rss) higher than
    // what we set as HiMemReportMax. Therefore, the HiMem reporter should genereate a
    // 100% report right away.
    // Testing anything more is much more complicated since the test would have to
    // predict, with a certain correctness, rss and swap.

    static final String[] beforeReport = {
        "HiMemoryReport: rss\\+swap=.* - alert level increased to 3 \\(>=\\d+%\\).",
        "HiMemoryReport: ... seems we passed alert level 1 \\(\\d+%\\) without noticing.",
        "HiMemoryReport: ... seems we passed alert level 2 \\(\\d+%\\) without noticing.",
    };

    static final String[] reportHeader = {
            "# High Memory Report:",
            "# rss\\+swap .* larger than \\d+% of HiMemReportMax.*",
            "# Spike number: 1",
    };

    static final String[] reportBody = {
            ".*Vitals.*",
            "Now:",
            "Native Memory Tracking:",
            "Total: reserved=.*, committed=.*",
            "# END: High Memory Report"
    };

    static void testPrint() throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportMax=128m",
                "-XX:NativeMemoryTracking=summary",
                "-Xmx128m", "-Xms128m", "-XX:+AlwaysPreTouch",
                TestHiMemReport.class.getName(),
                "sleep", "2" // num seconds to sleep to give the reporter thread time to generate output
        );

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);

        String[] lines = VitalsUtils.stderrAsLines(output);
        int line = VitalsUtils.matchPatterns(lines, 0, beforeReport);
        line = VitalsUtils.matchPatterns(lines, line + 1, reportHeader);
        line = VitalsUtils.matchPatterns(lines, line + 1, reportBody);
    }

    static void testDump() throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportMax=128m",
                "-XX:NativeMemoryTracking=summary",
                "-XX:HiMemReportDir=himemreport-1",
                "-Xmx128m", "-Xms128m", "-XX:+AlwaysPreTouch",
                TestHiMemReport.class.getName(),
                "sleep", "2" // num seconds to sleep to give the reporter thread time to generate output
                );

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);

        // We expect, on stderr, just the report header
        String[] lines = VitalsUtils.stderrAsLines(output);
        int line = VitalsUtils.matchPatterns(lines, 0, beforeReport);
        line = VitalsUtils.matchPatterns(lines, line + 1, reportHeader);

        // We expect a report in a file inside HiMemReportDir, and a mentioning of it on
        // stderr
        line = VitalsUtils.matchPatterns(lines, line + 1,
            new String[] { "# Printing to .*himemreport-1/sapmachine_himemalert" + patterFileSuffix + ".log",
                           "# Done\\." } );

        String reportFile = output.firstMatch("# Printing to (.*himemreport-1/sapmachine_himemalert" + patterFileSuffix + ".log)", 1);
        VitalsUtils.assertFileExists(reportFile);

        VitalsUtils.assertFileContentMatches(new File(reportFile), reportBody);
    }

    static void testDumpWithExecToReportDir() throws Exception {
        String reportDirName = "himemreport-2";
        // Here we not only dump, but we also execute several jcmds. Therefore we take some more seconds to sleep
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportMax=128m",
                "-XX:HiMemReportDir=himemreport-2",
                "-XX:HiMemReportExec=VM.flags -all;VM.metaspace show-loaders;GC.heap_dump",
                "-XX:NativeMemoryTracking=summary",
                "-Xmx128m", "-Xms128m", "-XX:+AlwaysPreTouch",
                TestHiMemReport.class.getName(),
                "sleep", "12" // num seconds to sleep to give the reporter thread time to generate output
        );

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);

        // We expect, on stderr, just the report header
        String[] lines = VitalsUtils.stderrAsLines(output);
        int line = VitalsUtils.matchPatterns(lines, 0, beforeReport);
        line = VitalsUtils.matchPatterns(lines, line + 1, reportHeader);

        // We expect a report in a file inside HiMemReportDir, and a mentioning of it on
        // stderr
        line = VitalsUtils.matchPatterns(lines, line + 1,
                new String[] { "# Printing to .*himemreport-2/sapmachine_himemalert" + patterFileSuffix + ".log",
                        "# Done\\." } );

        String reportFile = output.firstMatch("# Printing to (.*himemreport-2/sapmachine_himemalert" + patterFileSuffix + ".log)", 1);
        VitalsUtils.assertFileExists(reportFile);
        VitalsUtils.assertFileContentMatches(new File(reportFile), reportBody);

        // We also expect, in the report dir, several more files
        String reportDir = output.firstMatch("# Printing to (.*himemreport-2/)sapmachine_himemalert" + patterFileSuffix + ".log", 1);
        VitalsUtils.assertFileExists(reportDir);
        File reportDirAsFile = new File(reportDir);

        ////////
        // HiMemReportExec should have caused three jcmds to fire...

        line = VitalsUtils.matchPatterns(lines, line + 1,
                new String[] { "HiMemReport: Successfully executed \"VM.flags -all\" \\(\\d+ ms\\), output redirected to report dir",
                        "HiMemReport: Successfully executed \"VM.metaspace show-loaders\" \\(\\d+ ms\\), output redirected to report dir",
                        "HiMemReport: Successfully executed \"GC.heap_dump .*himemreport-2/GC.heap_dump" + patterFileSuffix + ".dump\" \\(\\d+ ms\\), output redirected to report dir" } );

        // VM.flags:

        // I expect two files, VM.flags_pidXXXX_1_100.err and VM.flags_pidXXXX_1_100.out, in the report dir...
        File VMflagsOutFile = VitalsUtils.findFirstMatchingFileInDirectory(reportDirAsFile,  "VM.flags" + patterFileSuffix + ".out");
        File VMflagsErrFile = VitalsUtils.findFirstMatchingFileInDirectory(reportDirAsFile,  "VM.flags" + patterFileSuffix + ".err");

        // The err file should be empty
        VitalsUtils.assertFileisEmpty(VMflagsErrFile.getAbsolutePath());

        // The out file should contain a valid flags report, containing, among other things, the flags we passed above
        // Note that since we started VM.flags with -all, we have the full flags output
        VitalsUtils.assertFileContentMatches(VMflagsOutFile, new String[] {
                ".*HiMemReport *= *true.*",
                ".*HiMemReportDir *= *himemreport-2.*",
                ".*HiMemReportExec *= *VM.flags.*VM.metaspace.*GC.heap_dump.*",
                ".*HiMemReportMax *= *134217728.*"
        });

        // "VM.metaspace":

        // I expect two files, VM.metaspace_pidXXXX_1_100.err and VM.metaspace_pidXXXX_1_100.out, in the report dir...
        File VMmetaspaceOutFile = VitalsUtils.findFirstMatchingFileInDirectory(reportDirAsFile,  "VM.metaspace" + patterFileSuffix + ".out");
        File VMmetaspaceErrFile = VitalsUtils.findFirstMatchingFileInDirectory(reportDirAsFile,  "VM.metaspace" + patterFileSuffix + ".err");

        // The err file should be empty
        VitalsUtils.assertFileisEmpty(VMmetaspaceErrFile.getAbsolutePath());

        // The out file should contain a valid flags report, containing, among other things, the flags we passed above
        // Note that since we started VM.flags with -all, we have the full flags output
        VitalsUtils.assertFileContentMatches(VMmetaspaceOutFile, new String[] {
                "Usage per loader:",
                ".*app.*",
                ".*bootstrap.*",
                "Total Usage.*",
                "Virtual space.*",
                "Settings.*",
                "MaxMetaspaceSize.*"
        });

        // GC.heap_dump:

        // I expect three files, the usual GC.heap_dump_pid<pid>_<timestamp>.(out|err) and the heap dump itself as
        // GC.heap_dump_pid<pid>_<timestamp>.dump.
        File heapDumpOutFile = VitalsUtils.findFirstMatchingFileInDirectory(reportDirAsFile,  "GC.heap_dump" + patterFileSuffix + ".out");
        File heapDumpErrFile = VitalsUtils.findFirstMatchingFileInDirectory(reportDirAsFile,  "GC.heap_dump" + patterFileSuffix + ".err");
        File heapDumpDumpFile = VitalsUtils.findFirstMatchingFileInDirectory(reportDirAsFile,  "GC.heap_dump" + patterFileSuffix + ".dump");

        // The err file should be empty
        VitalsUtils.assertFileisEmpty(heapDumpErrFile.getAbsolutePath());

        // The out file should contain "dumped blabla"
        VitalsUtils.assertFileContentMatches(heapDumpOutFile, new String[] {
           "Dumping heap to.*",
           "Heap dump file created.*"
        });

        // The real dump file should exist and be reasonably sized.
        VitalsUtils.assertFileExists(heapDumpDumpFile);
        if (heapDumpDumpFile.length() < 1024 * 16) {
            throw new RuntimeException("heap dump suspiciously small.");
        }
    } // end: testDumpWithExecToReportDir

    // Test multiple Execs, with the output of the commands going to stderr
    static void testDumpWithExecToStderr() throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportMax=128m",
                "-XX:HiMemReportExec=VM.flags -all;VM.metaspace show-loaders",
                "-XX:NativeMemoryTracking=summary",
                "-Xmx128m", "-Xms128m", "-XX:+AlwaysPreTouch",
                TestHiMemReport.class.getName(),
                "sleep", "8" // num seconds to sleep to give the reporter thread time to generate output
        );

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);

        // We expect the normal HiMemReport on stderr...
        String[] lines = VitalsUtils.stderrAsLines(output);
        int line = VitalsUtils.matchPatterns(lines, 0, beforeReport);
        line = VitalsUtils.matchPatterns(lines, line + 1, reportHeader);
        line = VitalsUtils.matchPatterns(lines, line + 1, reportBody);

        // ... as well as the output of VM.flags
        line = VitalsUtils.matchPatterns(lines, line + 1, new String [] {
                        ".*HiMemReport *= *true.*",
                        ".*HiMemReportMax *= *134217728.*",
                        "HiMemReport: Successfully executed \"VM.flags -all\" \\(\\d+ ms\\)"
        });

        // ... as well as the output of VM.metaspace
        line = VitalsUtils.matchPatterns(lines, line + 1, new String [] {
                "Usage per loader:",
                ".*app.*",
                ".*bootstrap.*",
                "Total Usage.*",
                "Virtual space.*",
                "Settings.*",
                "MaxMetaspaceSize.*",
                "HiMemReport: Successfully executed \"VM.metaspace show-loaders\" \\(\\d+ ms\\)"
        });

    } // end: testDumpWithExecToReportDir



    /**
     * test that HiMemReport can feel out some limit from the environment without being given one explicitely
     * (I'm not sure that this always works, but I would like to know if it does not, and if not, in what context)
     */
    static void testHasNaturalMax() throws IOException {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+HiMemReport", "-Xlog:vitals", "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        output.shouldNotMatch("HiMemReport.*limit could not be established");
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("print"))
            testPrint();
        else if (args[0].equals("dump"))
            testDump();
        else if (args[0].equals("dump-with-exec-to-reportdir"))
            testDumpWithExecToReportDir();
        else if (args[0].equals("dump-with-exec-to-stderr"))
            testDumpWithExecToStderr();
        else if (args[0].equals("natural-max"))
            testHasNaturalMax();
        else if (args[0].equals("sleep")) {
            int numSeconds = Integer.parseInt(args[1]);
            Thread.sleep(numSeconds * 1000);
        } else
            throw new RuntimeException("Invalid test " + args[0]);
    }
}
