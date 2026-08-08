/*
 * Copyright (c) 2018, 2021, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2018, SAP and/or its affiliates. All rights reserved.
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

import jdk.test.lib.JDKToolFinder;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import jdk.test.whitebox.WhiteBox;
import jtreg.SkippedException;

import java.util.ArrayList;
import java.util.Random;

// For now 64bit only, 32bit stack capturing still does not work that well

/*
 * @test
 * @summary Test the System.malloctrace command
 * @library /test/lib
 * @requires vm.bits == "64"
 * @requires os.family == "linux"
 * @modules java.base/jdk.internal.misc
 *          java.management
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm/timeout=400
 *      -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:-EnableMallocTrace
 *      MallocTraceDcmdTest
 */

public class MallocTraceDcmdTest {

    private static class MallocStresser {
        int numThreads = 5;
        Thread threads[];
        static WhiteBox whiteBox = WhiteBox.getWhiteBox();

        public MallocStresser(int numThreads) {
            this.numThreads = numThreads;
        }

        boolean stop = false;

        class StresserThread extends Thread {
            public void run() {
                Random rand = new Random();
                while (!stop) {
                    // We misuse NMT's whitebox functions for mallocing
                    // We also could use Unsafe.allocateMemory, but dealing with
                    // deprecation is annoying.
                    long p[] = new long[10];
                    for (int i = 0; i < p.length; i++) {
                        p[i] = whiteBox.NMTMalloc(rand.nextInt(128));
                    }
                    for (int i = 0; i < p.length; i++) {
                        whiteBox.NMTFree(p[i]);
                    }
                }
            }
        }

        public void start() {
            threads = new Thread[numThreads];
            for (int i = 0; i < numThreads; i ++) {
                threads[i] = new StresserThread();
                threads[i].start();
            }
        }

        public void stop() throws InterruptedException {
            stop = true;
            for (int i = 0; i < numThreads; i++) {
                threads[i].join();
            }
        }
    }

    static boolean currentState = false;
    static int numSwitchedOn = 0; // How often we switched tracing on
    static int numReseted = 0; // How often we reseted

    static OutputAnalyzer testCommand(String... options) throws Exception {
        OutputAnalyzer output;
        // Grab my own PID
        String pid = Long.toString(ProcessTools.getProcessId());
        String jcmdPath = JDKToolFinder.getJDKTool("jcmd");
        ArrayList<String> command = new ArrayList<>();
        command.add(jcmdPath);
        command.add(pid);
        command.add("System.malloctrace");
        for (String option: options) {
            if (option != null) {
                command.add(option);
            }
        }
        System.out.println("--- " + command);
        ProcessBuilder pb = new ProcessBuilder(command);
        output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        return output;
    }

    // System.malloctrace on
    private static void testOn() throws Exception {
        OutputAnalyzer output = testCommand("on");
        output.shouldContain("Tracing active");
        numSwitchedOn ++;
        currentState = true;
    }

    // System.malloctrace off
    private static void testOff() throws Exception {
        OutputAnalyzer output = testCommand("off");
        output.shouldContain("Tracing inactive");
        currentState = false;
    }

    // System.malloctrace reset
    private static void testReset() throws Exception {
        OutputAnalyzer output = testCommand("reset");
        output.shouldContain("Tracing table reset");
        numReseted ++;
    }

    // System.malloctrace print
    private static void testPrint(boolean all) throws Exception {
        OutputAnalyzer output = testCommand("print", all ? "all" : null);
        if (currentState) {
            output.shouldContain("WB_NMTMalloc");
            output.shouldContain("Malloc trace on");
        } else {
            output.shouldContain("Malloc trace off");
        }
    }

    // aka, Alpine
    private static boolean NotAGlibcSystem() throws Exception {
        OutputAnalyzer output = testCommand("print");
        return output.getStdout().contains("Not a glibc system");
    }

    public static void main(String args[]) throws Throwable {

        if (NotAGlibcSystem()) {
            throw new SkippedException("Not a glibc system, skipping test");
        }

        if (!MallocTraceTestHelpers.GlibcSupportsMallocHooks()) {
            throw new SkippedException("Glibc has no malloc hooks. Skipping test.");
        }

        MallocStresser stresser = new MallocStresser(3);
        stresser.start();
        Thread.sleep(1000);
        testPrint(false);
        Thread.sleep(500);
        testOn();
        Thread.sleep(500);
        testPrint(false);
        testPrint(true);
        testReset();
        testPrint(true);
        testOn();
        Thread.sleep(500);
        testOff();
        testOff();
        Thread.sleep(500);
        testPrint(false);
        testReset();
        testPrint(false);
        testOff();
    }
}
