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
        File subdir = VitalsUtils.createSubTestDir("test-outputdir-1", true);
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportDir=" + subdir.getAbsolutePath(),
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        VitalsUtils.fileShouldExist(subdir);
    }

    /**
     * test HiMemReportDir with a valid, absolute path to an existing directory
     */
    static void testValidExistingReportDir() throws IOException {
        File subdir = VitalsUtils.createSubTestDir("test-outputdir-2", false);
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-XX:HiMemReportDir=" + subdir.getAbsolutePath(),
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        // VM should have created the output directory
        VitalsUtils.fileShouldExist(subdir);
    }

    /**
     * test +HiMemReport without any further options. It should come up with a reasonable limit.
     */
    static void testHiMemReportOn() throws IOException {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-Xlog:os",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        output.shouldContain("Vitals HiMemReport: Setting limit to");
        output.shouldContain("HiMemReport subsystem initialized.");
    }

    /**
     * test that -HiMemReport means off
     */
    static void testHiMemReportOff() throws IOException {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:-HiMemReport", "-Xlog:os",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        output.shouldNotContain("Vitals HiMemReport: Setting limit to");
    }

    /**
     * test that HiMemReport is off by default
     */
    static void testHiMemReportOffByDefault() throws IOException {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-Xlog:os",
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        output.shouldNotContain("Vitals HiMemReport: Setting limit to");
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("ValidNonExistingReportDir"))
            testValidNonExistingReportDir();
        else if (args[0].equals("ValidExistingReportDir"))
            testValidExistingReportDir();
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
