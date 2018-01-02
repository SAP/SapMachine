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

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.ArrayList;
import jdk.test.lib.Utils;
import jdk.test.lib.apps.LingeredApp;

/*
 * @test
 * @bug 8192985
 * @summary Test the clhsdb 'scanoops' command
 * @library /test/lib
 * @run main/othervm/timeout=1200 ClhsdbScanOops
 */

public class ClhsdbScanOops {

    private static void testWithGcType(String gc) throws Exception {

        LingeredApp theApp = null;

        try {
            ClhsdbLauncher test = new ClhsdbLauncher();
            List<String> vmArgs = new ArrayList<String>();
            vmArgs.add(gc);
            theApp = LingeredApp.startApp(vmArgs);

            System.out.println ("Started LingeredApp with the GC option " + gc +
                                " and pid " + theApp.getPid());

            // Run the 'universe' command to get the address ranges
            List<String> cmds = List.of("universe");

            String universeOutput = test.run(theApp.getPid(), cmds, null, null);

            if (universeOutput == null) {
                // Output could be null due to attach permission issues
                // and if we are skipping this.
                LingeredApp.stopApp(theApp);
                return;
            }

            cmds = new ArrayList<String>();
            Map<String, List<String>> expStrMap = new HashMap<>();
            Map<String, List<String>> unExpStrMap = new HashMap<>();

            String startAddress = null;
            String endAddress = null;
            String[] snippets = null;

            if (gc.contains("UseParallelGC")) {
                snippets = universeOutput.split("eden =  ");
            } else {
                snippets = universeOutput.split("eden \\[");
            }
            String[] words = snippets[1].split(",");
            // Get the addresses from Eden
            startAddress = words[0].replace("[", "");
            endAddress = words[1];
            String cmd = "scanoops " + startAddress + " " + endAddress;
            cmds.add(cmd);

            expStrMap.put(cmd, List.of
                ("java/lang/Object", "java/lang/Class", "java/lang/Thread",
                 "java/lang/String", "[B", "[I"));

            // Test the 'type' option also
            // scanoops <start addr> <end addr> java/lang/String
            // Ensure that only the java/lang/String oops are printed.
            cmd = cmd + " java/lang/String";
            cmds.add(cmd);
            expStrMap.put(cmd, List.of("java/lang/String"));
            unExpStrMap.put(cmd, List.of("java/lang/Thread"));

            test.run(theApp.getPid(), cmds, expStrMap, unExpStrMap);
        } catch (Exception ex) {
            throw new RuntimeException("Test ERROR " + ex, ex);
        } finally {
            LingeredApp.stopApp(theApp);
        }
    }

    public static void main(String[] args) throws Exception {
        System.out.println("Starting the ClhsdbScanOops test");
        try {
            testWithGcType("-XX:+UseParallelGC");
            testWithGcType("-XX:+UseSerialGC");
        } catch (Exception e) {
            throw new Error("Test failed with " + e);
        }
        System.out.println("Test PASSED");
    }
}
