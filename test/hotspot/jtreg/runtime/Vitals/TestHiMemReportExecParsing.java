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
 * @test TestHiMemReportExecParsing
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportExecParsing 1
 */

/*
 * @test TestHiMemReportExecParsing
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportExecParsing 2
 */

/*
 * @test TestHiMemReportExecParsing
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportExecParsing 3
 */

/*
 * @test TestHiMemReportExecParsing
 * @library /test/lib
 * @requires os.family == "linux"
 * @run driver TestHiMemReportExecParsing 4
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.io.IOException;

public class TestHiMemReportExecParsing {

    static void do_test(String execString, boolean shouldSucceed, String... shouldContain) throws IOException {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                "-XX:+HiMemReport", "-Xlog:vitals", "-XX:HiMemReportExec=" + execString,
                "-Xmx64m", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        if (shouldSucceed) {
            output.shouldHaveExitValue(0);
        } else {
            output.shouldNotHaveExitValue(0);
        }
        for (String s : shouldContain) {
            output.shouldContain(s);
        }
    }

    public static void main(String[] args) throws Exception {
        int variant = Integer.parseInt(args[0]);
        switch (variant) {
            case 1:
                do_test("VM.metaspace", true, "HiMemReportExec: storing command \"VM.metaspace\"");
                break;
            case 2:
                do_test("VM.info;VM.metaspace;GC.heap_dump", true,
                        "HiMemReportExec: storing command \"VM.info\"", "HiMemReportExec: storing command \"VM.metaspace\"",
                        "HiMemReportExec: storing command \"GC.heap_dump\"");
                break;
            case 3:
                do_test(";  VM.info;; ; VM.metaspace show-loaders   ", true,
                        "HiMemReportExec: storing command \"VM.info\"", "HiMemReportExec: storing command \"VM.metaspace show-loaders\"");
                break;
            case 4:
                do_test("  hallo ", false,
                        "HiMemReportExec: Command \"hallo\" invalid");
                break;
        }
    }

}
