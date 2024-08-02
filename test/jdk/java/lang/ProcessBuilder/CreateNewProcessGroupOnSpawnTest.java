/*
 * Copyright (c) 2018, 2024 SAP SE. All rights reserved.
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

import java.util.concurrent.TimeUnit;

import jdk.test.lib.Platform;

import com.sap.jdk.ext.process.ProcessGroupHelper;

/**
 * @test
 * @summary Tests the SapMachine feature to use process groups via ProcessBuilder.
 *          Test that the process group id of the sub process is different from the parent process group id.
 * @library /test/lib
 * @run main CreateNewProcessGroupOnSpawnTest
 */

public class CreateNewProcessGroupOnSpawnTest {

    static native private long getpgid0(long pid);

    public static void main(String[] args) throws Throwable {

        if (!Platform.isWindows()) {
            System.loadLibrary("CreateNewProcessGroupOnSpawnTest");
        }

        // Retrieve my process group id
        long mypid = ProcessHandle.current().pid();
        long mypgid = -1;

        if (Platform.isWindows()) {
            System.out.println("My pid: " + mypid);
            if (mypid <= 0) {
                throw new RuntimeException("Unexpected value for own processid");
            }
        } else {
            mypgid = getpgid0(mypid);
            System.out.println("My pid: " + mypid + " my pgid: " + mypgid);
            if (mypid <= 0 || mypgid <= 0) {
                throw new RuntimeException("Unexpected value for own processid");
            }
        }

        ProcessBuilder pb = new ProcessBuilder("sleep", "120");
        Process p = pb.start();

        try {
            // Retrieve process group id
            System.out.println("Child started: " + p.toHandle());
            long childpid = p.toHandle().pid();
            if (Platform.isWindows()) {
                System.out.println("Child pid: " + childpid);
                if (childpid <= 0) {
                    throw new RuntimeException("Unexpected value for child processid");
                }
            } else {
                long childpgid = getpgid0(childpid);
                System.out.println("Child pid: " + childpid + " child pgid: " + childpgid);
                if (childpid <= 0 || childpgid <= 0) {
                    throw new RuntimeException("Unexpected value for child processid");
                }
                System.out.println("We expect child to be in our process group.");
                if (childpgid == mypgid) {
                    System.out.println("Ok: Child created in my process group as expected.");
                } else {
                    throw new RuntimeException("Error: child not in my process group.");
                }
            }
        } finally {
            // clean up
            p.destroy();
            p.waitFor();
        }


        ProcessGroupHelper.createNewProcessGroupOnSpawn(pb, true);
        p = pb.start();

        try {
            // Retrieve process group id
            System.out.println("Child started: " + p.toHandle());
            long childpid = p.toHandle().pid();
            if (Platform.isWindows()) {
                System.out.println("Child pid: " + childpid);
                if (childpid <= 0) {
                    throw new RuntimeException("Unexpected value for child processid");
                }
            } else {
                long childpgid = getpgid0(childpid);
                System.out.println("Child pid: " + childpid + " child pgid: " + childpgid);
                if (childpid <= 0 || childpgid <= 0) {
                    throw new RuntimeException("Unexpected value for child processid");
                }
                System.out.println("We expect child to be pg leader.");
                if (childpgid == childpid) {
                    System.out.println("Ok: Child created in its own process group.");
                } else {
                    throw new RuntimeException("Error: child not in its own process group.");
                }
            }
            ProcessGroupHelper.terminateProcessGroupForLeader(p, false);
            p.waitFor(1, TimeUnit.SECONDS);
            if (p.isAlive()) {
                throw new RuntimeException("Error: Could not terminate process through its process group.");
            }
        } finally {
            // clean up
            p.destroy();
            p.waitFor();
        }
    }
}
