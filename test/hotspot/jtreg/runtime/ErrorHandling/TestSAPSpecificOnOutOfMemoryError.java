/*
 * Copyright (c) 2015, 2021, Oracle and/or its affiliates. All rights reserved.
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

/*
 * @test TestSAPSpecificOnOutOfMemoryError
 * @summary Test SapMachine/SapJVM specific behavior
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @run driver TestSAPSpecificOnOutOfMemoryError
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import java.util.ArrayList;

public class TestSAPSpecificOnOutOfMemoryError {

    static OutputAnalyzer run_test(String ... vm_args) throws Exception {
        ArrayList<String> args = new ArrayList<>();
        for (String s : vm_args) {
            args.add(s);
        }
        args.add("-Xmx128m");
        args.add(TestSAPSpecificOnOutOfMemoryError.class.getName());
        args.add("throwOOME");
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(args);
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.reportDiagnosticSummary();
        int exitValue = output.getExitValue();
        if (0 == exitValue) {
            //expecting a non zero value
            throw new Error("Expected to get non zero exit value");
        }
        return output;
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1) {
            // This should guarantee to throw:
            // java.lang.OutOfMemoryError: Requested array size exceeds VM limit
            Object[] oa = new Object[Integer.MAX_VALUE];
            return;
        }

        final String aborting_due = "Aborting due to java.lang.OutOfMemoryError";
        final String terminating_due = "Terminating due to java.lang.OutOfMemoryError";
        final String java_frames = "Java frames: (J=compiled Java code, j=interpreted, Vv=VM code)";
        final String a_fatal_error = "# A fatal error has been detected by the Java Runtime Environment";
        final String no_core = "CreateCoredumpOnCrash turned off, no core file dumped";
        final String yes_core_1 = "Core dump will be written.";     // If limit > 0
        final String yes_core_2 = "Core dumps have been disabled."; // if limit == 0. For the purpose of this test this is still okay
        final String summary_from_hs_err = "S U M M A R Y";

        // CrashOnOutOfMemoryError, without cores explicitly enabled:
        // - thread stack
        // - aborting with hs-err file
        // - no core
        {
            OutputAnalyzer output = run_test("-XX:+CrashOnOutOfMemoryError");

            output.shouldContain(aborting_due);
            output.shouldContain(java_frames);
            output.shouldContain(a_fatal_error);
            output.shouldContain(no_core);

            output.shouldNotContain(terminating_due);
            output.shouldNotContain(yes_core_1);
            output.shouldNotContain(yes_core_2);
        }

        // ExitVMOnOutOfMemoryError is a SAP specific alias for CrashOnOutOfMemoryError
        {
            OutputAnalyzer output = run_test("-XX:+ExitVMOnOutOfMemoryError");

            output.shouldContain(aborting_due);
            output.shouldContain(java_frames);
            output.shouldContain(a_fatal_error);
            output.shouldContain(no_core);

            output.shouldNotContain(terminating_due);
            output.shouldNotContain(yes_core_1);
            output.shouldNotContain(yes_core_2);
        }

        // CrashOnOutOfMemoryError, with cores explicitly enabled:
        // - thread stack
        // - aborting with hs-err file
        // - core is to be written (or, attempted, if ulimit = 0)
        {
            OutputAnalyzer output = run_test("-XX:+CrashOnOutOfMemoryError", "-XX:+CreateCoredumpOnCrash");

            output.shouldContain(aborting_due);
            output.shouldContain(java_frames);
            output.shouldContain(a_fatal_error);
            output.shouldMatch("(" + yes_core_1 + "|" + yes_core_2 + ")");

            output.shouldNotContain(no_core);
            output.shouldNotContain(terminating_due);
        }

        // ExitOnOutOfMemoryError should:
        // - print thread stack
        // - terminate the VM
        {
            OutputAnalyzer output = run_test("-XX:+ExitOnOutOfMemoryError");

            output.shouldContain(terminating_due);
            output.shouldContain(java_frames);

            output.shouldNotContain(aborting_due);
            output.shouldNotContain(a_fatal_error);
            output.shouldNotContain(yes_core_1);
            output.shouldNotContain(yes_core_2);
            output.shouldNotContain(no_core);
        }

        // Test that giving ErrorFileToStdout in combination with CrashOnOutOfMemoryError will
        //  print the hs-err file - including the limits, which may be important here - to stdout
        {
            OutputAnalyzer output = run_test("-XX:+CrashOnOutOfMemoryError", "-XX:+ErrorFileToStdout");

            output.shouldContain(aborting_due);
            output.shouldContain(java_frames);
            output.shouldContain(a_fatal_error);
            output.shouldContain(no_core);
            output.shouldContain(summary_from_hs_err);

            output.shouldNotContain(terminating_due);
            output.shouldNotContain(yes_core_1);
            output.shouldNotContain(yes_core_2);
        }

        // HeapDumpOnOutOfMemoryError should:
        // - print thread stack
        // - The VM should just run on. OOM will bubble up and end the program.
        {
            OutputAnalyzer output = run_test("-XX:+HeapDumpOnOutOfMemoryError");

            output.shouldContain(java_frames);

            output.shouldNotContain(terminating_due);
            output.shouldNotContain(aborting_due);
            output.shouldNotContain(a_fatal_error);
            output.shouldNotContain(yes_core_1);
            output.shouldNotContain(yes_core_2);
            output.shouldNotContain(no_core);
        }
    }
}
