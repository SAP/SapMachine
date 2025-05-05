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
 * @test TestVitalsAtExit
 * @summary Test verifies that -XX:+PrintVitalsAtExit prints vitals at exit.
 * @library /test/lib
 * @run driver TestVitalsAtExit print
 */

/*
 * @test TestVitalsAtExit
 * @summary Test verifies that -XX:+DumpVitalsAtExit works
 * @library /test/lib
 * @run driver TestVitalsAtExit dump
 */

import jdk.test.lib.Asserts;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.io.File;

public class TestVitalsAtExit {


    public static void main(String[] args) throws Exception {
        if (args.length == 0) {
            try {
                Thread.sleep(5000); // we start with interval=1, so give us some secs to gather samples
            } catch (InterruptedException err) {
            }
            return;
        }
        if (args[0].equals("print")) {
            testPrint();
        } else if (args[0].equals("dump")) {
            testDump();
        } else {
            throw new RuntimeException("invalid argument " + args[0]);
        }
    }

    static void testPrint() throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+EnableVitals",
                "-XX:+PrintVitalsAtExit",
                "-XX:VitalsSampleInterval=1",
                "-XX:MaxMetaspaceSize=16m",
                "-Xmx128m",
                TestVitalsAtExit.class.getName());

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        output.stdoutShouldNotBeEmpty();
        VitalsTestHelper.outputMatchesVitalsTextMode(output);
    }

    static void testDump() throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+EnableVitals",
                "-XX:+DumpVitalsAtExit",
                "-XX:VitalsFile=abcd",
                "-XX:VitalsSampleInterval=1",
                "-XX:MaxMetaspaceSize=16m",
                "-Xmx128m",
                TestVitalsAtExit.class.getName());

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        output.stdoutShouldNotBeEmpty();
        output.shouldContain("Dumping Vitals to abcd.txt");
        output.shouldContain("Dumping Vitals csv to abcd.csv");
        File text_dump = new File("abcd.txt");
        Asserts.assertTrue(text_dump.exists() && text_dump.isFile(),
                "Could not find abcd.txt");
        File csv_dump = new File("abcd.csv");
        Asserts.assertTrue(csv_dump.exists() && csv_dump.isFile(),
                "Could not find abcd.csv");

        VitalsTestHelper.fileMatchesVitalsTextMode(text_dump);
        VitalsTestHelper.fileMatchesVitalsCSVMode(csv_dump);
    }
}
