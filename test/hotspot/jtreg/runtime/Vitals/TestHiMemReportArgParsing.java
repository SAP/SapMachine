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
 * @test TestHiMemReportArgParsing-ValidNonExistingReportDir
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportArgParsing ValidNonExistingReportDir
 */

/*
 * @test TestHiMemReportArgParsing-ValidExistingReportDir
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportArgParsing ValidExistingReportDir
 */

/*
 * @test TestHiMemReportArgParsing-InValidReportDir
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportArgParsing InValidReportDir
 */

/*
 * @test TestHiMemReportArgParsing-HiMemReportOn
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportArgParsing HiMemReportOn
 */

/*
 * @test TestHiMemReportArgParsing-HiMemReportOff
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportArgParsing HiMemReportOff
 */

/*
 * @test TestHiMemReportArgParsing-HiMemReportOffByDefault
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportArgParsing HiMemReportOffByDefault
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.io.File;
import java.io.IOException;

public class TestHiMemReportArgParsing {

    /**
     * test HiMemReportDir with a valid, absolute path to a non-existing directory
     */
    static void testValidNonExistingReportDir() throws IOException {
        File subdir = VitalsUtils.createSubTestDir("test-outputdir-1", false);
        VitalsUtils.fileShouldNotExist(subdir);
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportDir=" + subdir.getAbsolutePath(), "-Xlog:vitals",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);
        VitalsUtils.outputStdoutMatchesPatterns(output, new String[] {
                ".*[vitals].*HiMemReportDir: Created report directory.*" + subdir.getName() + ".*",
                ".*[vitals].*HiMemReport subsystem initialized.*"
        });
        VitalsUtils.fileShouldExist(subdir);
    }

    /**
     * test HiMemReportDir with a valid, absolute path to an existing directory
     */
    static void testValidExistingReportDir() throws IOException {
        File subdir = VitalsUtils.createSubTestDir("test-outputdir-2", true);
        VitalsUtils.fileShouldExist(subdir);
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportDir=" + subdir.getAbsolutePath(), "-Xlog:vitals",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);
        VitalsUtils.outputStdoutMatchesPatterns(output, new String[] {
                ".*[vitals].*Found existing report directory at.*" + subdir.getName() + ".*",
                ".*[vitals].*HiMemReport subsystem initialized.*"
        });
        VitalsUtils.fileShouldExist(subdir);
    }

    /**
     * test HiMemReportDir with an invalid path (containing several layers, which the VM will not create, it will only
     * create subdirs of max 1 level). We explicitly omit Xlog:vitals, but still expect a warning
     */
    static void testInValidReportDir() throws IOException {
        File f = new File("/tmp/gibsnicht/gibsnicht/gibsnicht");
        VitalsUtils.fileShouldNotExist(f);
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportDir=" + f.getAbsolutePath(), "-Xlog:vitals",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldNotHaveExitValue(0);
        VitalsUtils.outputStdoutMatchesPatterns(output, new String[] {
                ".*[vitals].*Failed to create report directory.*" + f.getAbsolutePath() + ".*",
                "Error occurred during initialization of VM"
        });
        VitalsUtils.fileShouldNotExist(f);
    }

    /**
     * test +HiMemReport without any further options. It should come up with a reasonable limit.
     */
    static void testHiMemReportOn() throws IOException {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-Xlog:vitals",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);
        VitalsUtils.outputStdoutMatchesPatterns(output, new String[] {
                ".*[vitals].*Setting limit to.*",
                ".*[vitals].*HiMemReport subsystem initialized.*"
        });
    }

    /**
     * test that -HiMemReport means off
     */
    static void testHiMemReportOff() throws IOException {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:-HiMemReport", "-Xlog:vitals",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);
        output.shouldNotContain("HiMemReport subsystem initialized");
    }

    /**
     * test that HiMemReport is off by default
     */
    static void testHiMemReportOffByDefault() throws IOException {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-Xlog:vitals",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        output.shouldHaveExitValue(0);
        output.shouldNotContain("HiMemReport subsystem initialized");
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("ValidNonExistingReportDir"))
            testValidNonExistingReportDir();
        else if (args[0].equals("ValidExistingReportDir"))
            testValidExistingReportDir();
        else if (args[0].equals("InValidReportDir"))
            testInValidReportDir();
        else if (args[0].equals("HiMemReportOn"))
            testHiMemReportOn();
        else if (args[0].equals("HiMemReportOff"))
            testHiMemReportOff();
        else if (args[0].equals("HiMemReportOffByDefault"))
            testHiMemReportOffByDefault();
        else
            throw new RuntimeException("Invalid test " + args[0]);
    }

}
