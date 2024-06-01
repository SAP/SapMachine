/*
 * Copyright (c) 2018 SAP SE. All rights reserved.
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

/**
 * @test
 * @summary A VM is started with -Dsap.jdk.lang.Process.createNewProcessGroupOnSpawn=true and itself starts
 *          sub processes. Test that the process group id of the sub process is different from the parent process group
 *          id.
 * @requires (os.family != "windows")
 * @library /test/lib
 * @run main/othervm -Xmx64M -Dsap.jdk.lang.Process.createNewProcessGroupOnSpawn=true CreateNewProcessGroupOnSpawnTest
 * @run main/othervm -Xmx64M CreateNewProcessGroupOnSpawnTest
 */

public class CreateNewProcessGroupOnSpawnTest {

    static native private long getpgid0(long pid);

    static private boolean shouldWeCreateNewProcessGroupOnSpawn() {
        return Boolean.getBoolean("sap.jdk.lang.Process.createNewProcessGroupOnSpawn");
    }

    public static void main(String[] args) throws Throwable {

        System.loadLibrary("CreateNewProcessGroupOnSpawnTest");

        // Retrieve my process group id
        long mypid = ProcessHandle.current().pid();
        long mypgid = getpgid0(mypid);

        System.out.println("My pid: " + mypgid + " my pgid: " + mypgid);

        ProcessBuilder pb = new ProcessBuilder("sleep", "120");
        Process p = pb.start();
        ProcessHandle hdl = p.toHandle();

        System.out.println("Child started: " + hdl.toString());

        // Retrieve process group id
        long childpid = p.toHandle().pid();
        long childpgid = getpgid0(childpid);

        System.out.println("Child pid: " + childpid + " child pgid: " + childpgid);

        if (mypid <= 0 || mypgid <= 0 || childpgid <= 0 || childpid <= 0) {
            throw new RuntimeException("wat");
        }

        // Kill child, we do not need it anymore.
        p.destroy();
        p.waitFor();

        if (shouldWeCreateNewProcessGroupOnSpawn()) {
            System.out.println("We expect child to be pg leader.");
            if (childpgid == childpid) {
                System.out.println("Ok: Child created in its own process group.");
            } else {
                throw new RuntimeException("Error: child not in its own process group.");
            }
        } else {
            System.out.println("We expect child to be in our process group.");
            if (childpgid == mypgid) {
                System.out.println("Ok: Child created in my process group as expected.");
            } else {
                throw new RuntimeException("Error: child not in my process group.");
            }
        }

    }

}
