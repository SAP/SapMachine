/*
 * Copyright (c) 2021, SAP SE. All rights reserved.
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
 * @test TestHighMemoryReport-print
 * @summary Test verifies HighMemoryThreshold handling
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHighMemoryReport print
 */

/*
 * @test TestHighMemoryReport-dump
 * @summary Test verifies HighMemoryThreshold handling
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHighMemoryReport dump
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.io.File;

public class TestHighMemoryReport {

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            if (args[0].equals("print")) {
                testPrint();
            } else if (args[0].equals("dump")) {
                testDump();
            } else {
                throw new RuntimeException("unexpected argument");
            }
        } else {
            Thread.sleep(3000); // Enough time to let the report finish
        }
    }

    static void testPrint() throws Exception {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+EnableVitals",
                "-XX:VitalsSampleInterval=1",
                "-XX:HighMemoryThreshold=64m",
                "-XX:+PrintReportOnHighMemory",
                "-XX:NativeMemoryTracking=summary",
                "-XX:MaxMetaspaceSize=16m",
                "-Xmx128m", "-Xms128m", "-XX:+AlwaysPreTouch",
                TestHighMemoryReport.class.getName());

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);
        output.stderrShouldContain("# High Memory Threshold reached");
        output.stderrShouldContain("Native Memory Tracking:");
        output.stderrShouldContain("Vitals:");
    }

    static void testDump() throws Exception {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+EnableVitals",
                "-XX:VitalsSampleInterval=1",
                "-XX:HighMemoryThreshold=64m",
                "-XX:+DumpReportOnHighMemory",
                "-XX:NativeMemoryTracking=summary",
                "-XX:MaxMetaspaceSize=16m",
                "-Xmx128m", "-Xms128m", "-XX:+AlwaysPreTouch",
                TestHighMemoryReport.class.getName());

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();

        output.shouldHaveExitValue(0);
        output.stderrShouldContain("# High Memory Threshold reached");

        String dump_file = output.firstMatch("# Dumping report to (sapmachine_highmemory_\\d+\\.log)", 1);
        if (dump_file == null) {
            throw new RuntimeException("Did not find dump file in output.\n");
        }

        File f = new File(dump_file);
        if (!f.exists()) {
            throw new RuntimeException("Dump file missing at "
                    + f.getAbsolutePath() + ".\n");
        }

        System.out.println("Found dump file. Scanning...");

        VitalsTestHelper.fileShouldMatch(f, new String[] { "Vitals:", "Native Memory Tracking:" });

    }

}
