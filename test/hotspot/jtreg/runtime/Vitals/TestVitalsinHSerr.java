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
 * @test
 * @summary Test that Vitals report appears in hs-err file
 * @library /test/lib
 * @requires vm.debug
 * @modules java.base/jdk.internal.misc
 *          java.management
 * @run driver TestVitalsinHSerr
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;


// Test that, when we crash, we have Vitals as expected in the hs-err file
public class TestVitalsinHSerr {

    public static void main(String[] args) throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+UnlockDiagnosticVMOptions",
                "-Xmx100M",
                "-XX:-CreateCoredumpOnCrash",
                "-XX:ErrorHandlerTest=14", // sigsegv
                "-XX:+ErrorFileToStdout",
                "-version");

        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldNotHaveExitValue(0);

        output.shouldMatch("# A fatal error has been detected by the Java Runtime Environment:.*");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);
        // We want to see the Now value too. Dumping crash reports is about the only place we want
        // the sample exactly when we report.
        output.shouldContain("Now");        // hs-err file (in this case, dumped to stdout) should contain vitals

    }
}
