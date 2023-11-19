/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
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

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import jtreg.SkippedException;

// SapMachine 2021-08-01: malloctrace
// For now 64bit only, 32bit stack capturing still does not work that well


/*
 * @test MallocTraceTest
 * @requires os.family == "linux"
 * @requires vm.bits == "64"
 * @library /test/lib
 * @run driver MallocTraceTest on
 */

/*
 * @test MallocTraceTest
 * @requires os.family == "linux"
 * @requires vm.bits == "64"
 * @library /test/lib
 * @run driver MallocTraceTest off
 */

public class MallocTraceTest {

    public static void main(String... args) throws Throwable {

        if (!MallocTraceTestHelpers.GlibcSupportsMallocHooks()) {
            throw new SkippedException("Glibc has no malloc hooks. Skipping test.");
        }

        if (args[0].equals("off") || args[0].equals("on")) {
            boolean active = args[0].equals("on");
            String option = active ? "-XX:+EnableMallocTrace" : "-XX:-EnableMallocTrace";
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-XX:+UnlockDiagnosticVMOptions",
                    option, "-XX:+PrintMallocTraceAtExit", "-version");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.reportDiagnosticSummary();
            output.shouldHaveExitValue(0);

            // Ignore output for Alpine
            if (output.getStderr().contains("Not a glibc system")) {
                throw new SkippedException("Not a glibc system, skipping test");
            }

            if (active) {
                String stdout = output.getStdout();
                // Checking for the correct frames is a whack-the-mole game since we cannot be sure how frames
                // are inlined. Therefore we cannot just use "shouldContain".
                output.stdoutShouldMatch("(os::malloc|my_malloc_hook)");
                output.shouldContain("Malloc trace on.");
            } else {
                output.shouldContain("Malloc trace off.");
            }
        } else {
            throw new RuntimeException("Wrong or missing args.");
        }
    }
}
