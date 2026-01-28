/*
 * Copyright (c) 2026 SAP SE. All rights reserved.
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

package gc.g1;

import static gc.testlibrary.Allocation.blackHole;

/*
 * @test TestHumongousAllocMixedPhase
 * @bug 8355972
 * @summary G1: should finish mixed phase and start a new concurrent
 *          cycle if there are excessive humongous allocations
 * @requires vm.gc.G1
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @library /
 * @run driver gc.g1.TestHumongousAllocMixedPhase
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestHumongousAllocMixedPhase {
    // Heap sizes < 224 MB are increased to 224 MB if vm_page_size == 64K to
    // fulfill alignment constraints.
    private static final int heapSize                       = 224; // MB
    private static final int heapRegionSize                 = 1;   // MB

    public static void main(String[] args) throws Exception {
        boolean success = false;
        int allocDelayMsMax = 500;
        for(int allocDelayMs = 10; !success; allocDelayMs *= 4) {
            System.out.println("Testing with allocDelayMs=" + allocDelayMs);
            success = runTest(allocDelayMs, allocDelayMs >= allocDelayMsMax /* lastTry*/);
        }
    }

    static boolean runTest(int allocDelayMs, boolean lastTry) throws Exception {
        OutputAnalyzer output = ProcessTools.executeLimitedTestJava(
            "-XX:+UseG1GC",
            "-Xms" + heapSize + "m",
            "-Xmx" + heapSize + "m",
            "-XX:G1HeapRegionSize=" + heapRegionSize + "m",
            "-Xlog:gc*",
            "-XX:+UnlockExperimentalVMOptions",
            "-XX:G1HumAllocPercentUntilConcurrent=30",
            HumongousObjectAllocator.class.getName(),
            Integer.toString(allocDelayMs));

        boolean success = false;
        String pauseFull = "Pause Full (G1 Compaction Pause)";
        if (lastTry) {
            output.shouldNotContain(pauseFull);
            output.shouldHaveExitValue(0);
            success = true;
        } else {
            success = !output.stdoutContains(pauseFull) && !output.getStderr().contains(pauseFull) &&
                output.getExitValue() == 0;
        }
        return success;
    }

    static class HumongousObjectAllocator {
        public static void main(String[] args) throws Exception {
            int allocDelayMs = Integer.parseInt(args[0]);
            // Make sure there are some candidate regions for mixed pauses.
            // (Eg w/o CDS there might be none)
            System.gc();
            for (int i = 0; i < 100; i++) {
                Integer[] humongous = new Integer[5_000_000];
                blackHole(humongous);
                System.out.println(i);
                Thread.sleep(allocDelayMs);
            }
        }
    }
}

