/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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

import java.util.ArrayList;
import java.util.List;
import java.io.IOException;
import java.util.stream.Collectors;
import java.io.OutputStream;
import jdk.test.lib.apps.LingeredApp;
import jdk.test.lib.JDKToolLauncher;
import jdk.test.lib.Platform;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.Utils;

/*
 * @test
 * @summary Test the 'type' command of jhsdb clhsdb.
 * @bug 8190307
 * @library /test/lib
 * @build jdk.test.lib.apps.*
 * @run main/othervm TestType
 */

public class TestType {

    private static void testClhsdbForType(
                        long lingeredAppPid,
                        String commandString,
                        String[] expectedOutputStrings) throws Exception {

        Process p;
        JDKToolLauncher launcher = JDKToolLauncher.createUsingTestJDK("jhsdb");
        launcher.addToolArg("clhsdb");
        launcher.addToolArg("--pid");
        launcher.addToolArg(Long.toString(lingeredAppPid));

        ProcessBuilder pb = new ProcessBuilder();
        pb.command(launcher.getCommand());
        System.out.println(
            pb.command().stream().collect(Collectors.joining(" ")));

        try {
            p = pb.start();
        } catch (Exception attachE) {
            throw new Error("Couldn't start jhsdb or attach to LingeredApp : " + attachE);
        }

        // Issue the 'type' commands at the clhsdb prompt.
        OutputStream input = p.getOutputStream();
        try {
            input.write((commandString + "\n").getBytes());
            input.write("quit\n".getBytes());
            input.flush();
        } catch (IOException ioe) {
            throw new Error("Problem issuing the 'type' command ", ioe);
        }

        OutputAnalyzer output = new OutputAnalyzer(p);

        try {
            p.waitFor();
        } catch (InterruptedException ie) {
            p.destroyForcibly();
            throw new Error("Problem awaiting the child process: " + ie);
        }

        output.shouldHaveExitValue(0);
        System.out.println(output.getOutput());

        for (String expectedOutputString: expectedOutputStrings) {
            output.shouldContain(expectedOutputString);
        }
    }

    public static void main (String... args) throws Exception {
        LingeredApp app = null;

        if (!Platform.shouldSAAttach()) {
            System.out.println(
               "SA attach not expected to work - test skipped.");
            return;
        }

        try {
            List<String> vmArgs = new ArrayList<String>();
            vmArgs.addAll(Utils.getVmOptions());
            // Strings to check for in the output of 'type'. The 'type'
            // command prints out entries from 'gHotSpotVMTypes', which
            // is a table containing the hotspot types, their supertypes,
            // sizes, etc, which are of interest to the SA.
            String[] defaultOutputStrings =
                {"type G1CollectedHeap CollectedHeap",
                 "type ConstantPoolCache MetaspaceObj",
                 "type ConstantPool Metadata",
                 "type CompilerThread JavaThread",
                 "type CardGeneration Generation",
                 "type ArrayKlass Klass",
                 "type InstanceKlass Klass"};
            // String to check for in the output of "type InstanceKlass"
            String[] instanceKlassOutputString = {"type InstanceKlass Klass"};

            app = LingeredApp.startApp(vmArgs);
            System.out.println ("Started LingeredApp with pid " + app.getPid());
            testClhsdbForType(app.getPid(), "type", defaultOutputStrings);
            testClhsdbForType(app.getPid(),
                              "type InstanceKlass",
                              instanceKlassOutputString);
        } finally {
            LingeredApp.stopApp(app);
        }
    }
}
