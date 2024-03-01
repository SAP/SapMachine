/*
 * Copyright (c) 2024 SAP SE. All rights reserved.
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
 *
 */

/**
 * @test
 * @summary Test functionality of the malloc hooks library.
 * @library /test/lib
 *
 * @run main/othervm/native/timeout=600 MallocHooksTest
 */

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.io.StringReader;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;

import jdk.test.lib.JDKToolFinder;
import jdk.test.lib.Platform;
import jdk.test.lib.Utils;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

import static jdk.test.lib.Asserts.*;

public class MallocHooksTest {
    static native void doRandomMemOps(int nrOfOps, int maxLiveAllocations, int seed,
                                      boolean trackLive, long[] sizes, long[] counts);
    static native void doRandomAllocsWithFrees(int nrOfOps, int size, int maxStack, int seed);

    private static final String LD_PRELOAD = Platform.isOSX() ? "DYLD_INSERT_LIBRARIES" : "LD_PRELOAD";
    private static final String LIB_SUFFIX = Platform.isOSX() ? ".dylib" : ".so";
    private static final String LIB_DIR = System.getProperty("sun.boot.library.path") + "/";
    private static final String NATIVE_DIR = System.getProperty("test.nativepath") + "/";
    private static final String LIB_MALLOC_HOOKS = LIB_DIR + "libmallochooks" + LIB_SUFFIX;
    private static final String LIB_FAKE_MALLOC_HOOKS = NATIVE_DIR + "libfakemallochooks" + LIB_SUFFIX;

    public static void main(String args[]) throws Exception {
        if (!Platform.isLinux() && !Platform.isOSX()) {
            return;
        }

        if (args.length == 0) {
            testNoRecursiveCallsForFallbacks();
            testEnvSanitizing();
            testTracking(false);
            testTracking(true);
            testDumpPercentage(true);
            testDumpPercentage(false);
            testUniqueStacks();
            testPartialTrackint(false, 2, 0.2);
            testPartialTrackint(true, 2, 0.2);
            testPartialTrackint(false, 10, 0.3);
            testPartialTrackint(true, 10, 0.3);
            testFlags();
            testJcmdOptions();
            return;
        }

        switch (args[0]) {
            case "manyStacks":
                System.loadLibrary("testmallochooks");
                doManyStacks(args);
                System.out.println("Done");

                while (true) {
                  Thread.sleep(1000);
                }

            case "checkEnv":
                if (args[1].equals(getLdPrelodEnv())) {
                   return;
                }

                throw new Exception("Expected " + LD_PRELOAD + "=\"" + args[1] + "\", but got " +
                                    LD_PRELOAD + "=\"" +  getLdPrelodEnv() + "\"");

            case "stress":
                System.loadLibrary("testmallochooks");
                doStress(args);

                while (true) {
                  Thread.sleep(1000);
                }

            case "sleep":
                System.out.print("*");
                System.out.flush();
                Thread.sleep(1000 * Long.parseLong(args[1]));
                return;

            case "wait":
                System.loadLibrary("testmallochooks");
                System.out.print("*");
                System.out.flush();

                while (true) {
                     doRandomMemOps(1000000, 32768, 1348763421, false, new long[8], new long[8]);
                }

            default:
                throw new Exception("Unknown command " + args[0]);
        }
    }

    private static void doManyStacks(String[] args) {
        int nrOfOps = Integer.parseInt(args[1]);
        int size = Integer.parseInt(args[2]);
        int maxStack = Integer.parseInt(args[3]);
        int seed = Integer.parseInt(args[4]);

        doRandomAllocsWithFrees(nrOfOps, size, maxStack, seed);
    }

    private static void doStress(String[] args) {
        int nrOfOps = Integer.parseInt(args[1]);
        int maxLiveAllocations = Integer.parseInt(args[2]);
        int seed = Integer.parseInt(args[3]);
        boolean trackLive = Boolean.parseBoolean(args[4]);
        long[] sizes = new long[8];
        long[] counts = new long[8];

        doRandomMemOps(nrOfOps, maxLiveAllocations, seed, trackLive, sizes, counts);

        for (int i = 0; i < sizes.length; ++i) {
            System.out.println(sizes[i] + " " + counts[i]);
        }
    }

    private static void testNoRecursiveCallsForFallbacks() throws Exception {
        ProcessBuilder pb = ProcessTools.createNativeTestProcessBuilder("testmallochooks");
        pb.environment().put(LD_PRELOAD, LIB_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
    }

    private static void testEnvSanitizing() throws Exception {
        ProcessBuilder pb = checkEnvProc("");
        pb.environment().put(LD_PRELOAD, LIB_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        pb = checkEnvProc(LIB_FAKE_MALLOC_HOOKS);
        pb.environment().put(LD_PRELOAD, LIB_MALLOC_HOOKS + ":" + LIB_FAKE_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        if (!Platform.isMusl()) {
            pb = checkEnvProc(LIB_FAKE_MALLOC_HOOKS);
            pb.environment().put(LD_PRELOAD, LIB_FAKE_MALLOC_HOOKS + ":" + LIB_MALLOC_HOOKS);
            new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        }
        pb = checkEnvProcWithHooks("");
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        pb = checkEnvProcWithHooks("");
        pb.environment().put(LD_PRELOAD, LIB_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        pb = checkEnvProcWithHooks(LIB_FAKE_MALLOC_HOOKS);
        pb.environment().put(LD_PRELOAD, LIB_MALLOC_HOOKS + ":" + LIB_FAKE_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        if (!Platform.isMusl()) {
            pb = checkEnvProcWithHooks(LIB_FAKE_MALLOC_HOOKS);
            pb.environment().put(LD_PRELOAD, LIB_FAKE_MALLOC_HOOKS + ":" + LIB_MALLOC_HOOKS);
            new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        }
    }

    private static long getPid(Process p) {
        return p.pid();
    }

    private static OutputAnalyzer callJcmd(Process p, String... args) throws Exception {
        ProcessBuilder pb = new ProcessBuilder();
        String[] realArgs = new String[args.length + 2];
        System.arraycopy(args, 0, realArgs, 2, args.length);
        realArgs[0] = JDKToolFinder.getJDKTool("jcmd");
        realArgs[1] = Long.toString(getPid(p));
        pb.command(realArgs);
        OutputAnalyzer oa = new OutputAnalyzer(pb.start());
        System.out.println("Output of jcmd " + String.join(" ", args));
        System.out.println(oa.getOutput());
        System.out.println("---------------------");
        oa.shouldHaveExitValue(0);
        return oa;
    }

    private static Process runWait(String... opts) throws Exception {
        String[] args = new String[opts.length + 3];
        System.arraycopy(opts, 0, args, 0, opts.length);
        args[opts.length + 0] = "-Djava.library.path=" + System.getProperty("java.library.path");
        args[opts.length + 1] = MallocHooksTest.class.getName();
        args[opts.length + 2] = "wait";
        Process p = ProcessTools.createLimitedTestJavaProcessBuilder(args).start();
        // Make sure java is running when we return
        p.getInputStream().read();
        return p;
    }

    private static void testJcmdOptions() throws Exception {
      // Check we cannot enable if already enabled.
      Process p = runWait("-XX:+UseMallocHooks", "-XX:+MallocTraceAtStartup");
      OutputAnalyzer oa = callJcmd(p, "MallocTrace.enable");
      oa.shouldContain("Malloc statistic is already enabled");
      oa = callJcmd(p, "MallocTrace.enable", "-force");
      oa.shouldNotContain("Malloc statistic is already enabled");
      oa.shouldContain("Malloc statistic enabled");
      oa.shouldContain("Disabled already running trace first");
      oa.shouldContain("Tracking all allocated memory");
      oa = callJcmd(p, "MallocTrace.disable");
      oa.shouldContain("Malloc statistic disabled");
      oa = callJcmd(p, "MallocTrace.disable");
      oa.shouldNotContain("Malloc statistic disabled");
      oa.shouldContain("Malloc statistic is already disabled");
      oa = callJcmd(p, "MallocTrace.enable", "-detailed-stats");
      oa.shouldContain("Malloc statistic enabled");
      oa.shouldContain("Collecting detailed statistics");
      oa = callJcmd(p, "MallocTrace.dump", "-internal-stats");
      oa.shouldMatch("Sampled [0-9,.]+ stacks, took [0-9,.]+ ns per stack on average");
      oa.shouldMatch("Sampling took [0-9,.]+ seconds in total");
      oa.shouldContain("Tracked allocations");
      oa.shouldContain("Untracked allocations");
      oa.shouldMatch("Untracked frees[ ]*:[ ]*0");
      oa.shouldContain("Statistic for stack maps");
      oa.shouldNotContain("Statistic for alloc maps");
      oa = callJcmd(p, "MallocTrace.enable", "-force", "-use-backtrace", "-only-nth=4");
      oa.shouldContain("Malloc statistic enabled"); // We cannot assume this machine has backtrace available.
      oa = callJcmd(p, "MallocTrace.enable", "-force", "-track-free");
      oa.shouldContain("Tracking live memory");
      oa = callJcmd(p, "MallocTrace.dump", "-dump-file=stdout");
      oa.shouldContain("Dumping done in");
      oa.shouldNotContain("Total unique stacks");
      try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
          while (!br.readLine().startsWith("Total unique stacks")) {
          }
      }
      p.destroy();
      p = runWait("-XX:+UseMallocHooks");
      oa = callJcmd(p, "MallocTrace.enable");
      oa = callJcmd(p, "MallocTrace.dump", "-dump-file=stderr");
      oa.shouldContain("Dumping done in");
      oa.shouldNotContain("Total unique stacks");
      try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getErrorStream()))) {
          while (!br.readLine().startsWith("Total unique stacks")) {
          }
      }
      p.destroy();
      p = runWait("-XX:+UseMallocHooks");
      oa = callJcmd(p, "MallocTrace.enable");
      oa = callJcmd(p, "MallocTrace.dump", "-dump-file=malloctrace.txt");
      oa.shouldContain("Dumping done in");
      oa.shouldNotContain("Total unique stacks");
      try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream("malloctrace.txt")))) {
          while (!br.readLine().startsWith("Total unique stacks")) {
          }
      }
      p.destroy();
      p = runWait("-XX:+UseMallocHooks");
      oa = callJcmd(p, "MallocTrace.enable", "-force", "-stack-depth=2");
      oa = callJcmd(p, "MallocTrace.dump", "-dump-file=stderr");
      oa.shouldContain("Dumping done in");
      try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getErrorStream()))) {
          MallocTraceResult result = new MallocTraceResult(br);
          int maxDepth = 0;
          long lastSize = Long.MAX_VALUE;

          for (int i = 0; i < result.nrOfStacks(); ++i) {
              Stack stack = result.getStack(i);
              maxDepth = Math.max(maxDepth, stack.getStackDepth());
              assertLTE(stack.getBytes(), lastSize);
              lastSize = stack.getBytes();
          }

          assertLTE(maxDepth, 2);
      }
      p.destroy();
      p = runWait("-XX:+UseMallocHooks");
      oa = callJcmd(p, "MallocTrace.enable", "-force", "-stack-depth=8");
      oa = callJcmd(p, "MallocTrace.dump", "-dump-file=stderr", "-sort-by-count");
      oa.shouldContain("Dumping done in");
      try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getErrorStream()))) {
          MallocTraceResult result = new MallocTraceResult(br);
          int maxDepth = 0;
          long lastCount = Long.MAX_VALUE;

          for (int i = 0; i < result.nrOfStacks(); ++i) {
              Stack stack = result.getStack(i);
              maxDepth = Math.max(maxDepth, stack.getStackDepth());
              assertLTE(stack.getCount(), lastCount);
              lastCount = stack.getCount();
          }

          assertLTE(maxDepth, 8);
      }
      p.destroy();
      p = runWait("-XX:+UseMallocHooks");
      oa = callJcmd(p, "MallocTrace.enable", "-force", "-stack-depth=8");
      oa = callJcmd(p, "MallocTrace.dump", "-dump-file=stderr", "-filter=Java_MallocHooksTest_doRandomMemOps");
      oa.shouldContain("Dumping done in");
      try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getErrorStream()))) {
          MallocTraceResult result = new MallocTraceResult(br);

          for (int i = 0; i < result.nrOfStacks(); ++i) {
              Stack stack = result.getStack(i);
              boolean found = false;

              for (int j = 0; j < stack.getStackDepth(); ++j) {
                  if (stack.getFunction(j).contains("Java_MallocHooksTest_doRandomMemOps")) {
                      found = true;
                      break;
                  }
              }

              assertTrue(found);
          }
      }
      p.destroy();
      p = runWait("-XX:+UseMallocHooks");
      oa = callJcmd(p, "MallocTrace.enable", "-force", "-stack-depth=8");
      oa = callJcmd(p, "MallocTrace.dump", "-dump-file=stderr", "-filter=should_not_be_found");
      oa.shouldContain("Dumping done in");
      try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getErrorStream()))) {
          while (true) {
              String line = br.readLine();

              if (line.startsWith("Printed 0 stacks")) {
                  break;
              }

              if (line.startsWith("Stack ")) {
                  throw new Exception(line);
              }
          }
      }
      p.destroy();
    }

    private static void testPartialTrackint(boolean trackLive, int nth, double diff) throws Exception {
        ProcessBuilder pb = runStress(1024 * 1024 * 10, 65536, 172369977, trackLive,
                                      "-Djava.library.path=" + System.getProperty("java.library.path"),
                                      "-XX:MallocTraceStackDepth=2",
                                      "-XX:+MallocTraceDetailedStats",
                                      "-XX:MallocTraceOnlyNth=" + nth);
        Process p = pb.start();
        MallocTraceExpectedStatistic expected = new MallocTraceExpectedStatistic(p);
        OutputAnalyzer oa = callJcmd(p, "MallocTrace.dump", "-max-entries=10", "-sort-by-count",
                                        "-filter=Java_MallocHooksTest_doRandomMemOps",
                                        "-internal-stats");
        p.destroy();
        MallocTraceResult actual = MallocTraceResult.fromString(oa.getOutput());
        System.out.println(expected);
        System.out.println(actual);

        double real_nth = Double.parseDouble(oa.getOutput().split("about every ")[1].split(" ")[0]);
        assertGTE(real_nth, (1 - diff) * nth);
        assertLTE(real_nth, (1 + diff) * nth);

        expected.check(actual, nth, diff);

        if (trackLive) {
            oa.shouldContain("Contains the currently allocated memory since enabling");
        } else {
            oa.shouldContain("Contains every allocation done since enabling");
        }
        oa.shouldContain("libtestmallochooks.");
        oa.shouldContain("Total allocated bytes");
        oa.shouldContain("Total printed count");
    }

    private static void testTracking(boolean trackLive) throws Exception {
        ProcessBuilder pb = runStress(1024 * 1024 * 10, 65536, 172369973, trackLive,
                                      "-Djava.library.path=" + System.getProperty("java.library.path"),
                                      "-XX:MallocTraceStackDepth=2");
        Process p = pb.start();
        MallocTraceExpectedStatistic expected = new MallocTraceExpectedStatistic(p);
        OutputAnalyzer oa = callJcmd(p, "MallocTrace.dump", "-max-entries=10", "-sort-by-count",
                                        "-filter=Java_MallocHooksTest_doRandomMemOps");
        p.destroy();
        MallocTraceResult actual = MallocTraceResult.fromString(oa.getOutput());
        System.out.println(expected);
        System.out.println(actual);

        expected.check(actual);

        if (trackLive) {
            oa.shouldContain("Contains the currently allocated memory since enabling");
        } else {
            oa.shouldContain("Contains every allocation done since enabling");
        }
        oa.shouldContain("libtestmallochooks.");
        oa.shouldContain("Total allocated bytes");
        oa.shouldContain("Total printed count");
    }

    private static OutputAnalyzer runSleep(int sleep, String... opts) throws Exception {
        String[] args = new String[opts.length + 4];
        System.arraycopy(opts, 0, args, 0, opts.length);
        args[opts.length + 0] = "-XX:+MallocTraceAtStartup";
        args[opts.length + 1] = MallocHooksTest.class.getName();
        args[opts.length + 2] = "sleep";
        args[opts.length + 3] = Integer.toString(sleep);
        return new OutputAnalyzer(ProcessTools.createLimitedTestJavaProcessBuilder(args).start());
    }

    private static void testBasicOutput(OutputAnalyzer oa) throws Exception {
        oa.shouldHaveExitValue(0);
        oa.shouldContain("Total printed bytes:");
    }

    private static void testFlags() throws Exception {
        OutputAnalyzer oa = null;
        try {
            oa = runSleep(1);
            oa.shouldHaveExitValue(1);
            oa.shouldContain("Could not find preloaded libmallochooks");
            oa.shouldContain(LD_PRELOAD + "=");
            oa = runSleep(10, "-XX:+UseMallocHooks", "-XX:MallocTraceDumpDelay=1s",
                              "-XX:MallocTraceDumpCount=1");
            testBasicOutput(oa);
            oa.shouldContain("Contains the currently allocated memory since enabling");
            oa.shouldContain("Stacks were collected via");
            oa = runSleep(10, "-XX:+UseMallocHooks", "-XX:MallocTraceDumpDelay=1s",
                              "-XX:MallocTraceDumpCount=1", "-XX:-MallocTraceTrackFree",
                              "-XX:-MallocTraceUseBacktrace");
            testBasicOutput(oa);
            oa.shouldContain("Contains every allocation done since enabling");
            oa.shouldContain("Stacks were collected via the fallback mechanism.");
        } catch (Exception e) {
            System.out.println(oa.getOutput());
            throw e;
        }
    }

    private static ProcessBuilder runManyStacks(int nrOfOps, int size, int maxStack, int seed,
                                                String... opts) {
        String[] args = new String[opts.length + 9];
        System.arraycopy(opts, 0, args, 0, opts.length);
        args[opts.length + 0] = "-XX:+UseMallocHooks";
        args[opts.length + 1] = "-XX:+MallocTraceAtStartup";
        args[opts.length + 2] = "-XX:-MallocTraceTrackFree";
        args[opts.length + 3] = MallocHooksTest.class.getName();
        args[opts.length + 4] = "manyStacks";
        args[opts.length + 5] = Integer.toString(nrOfOps);
        args[opts.length + 6] = Integer.toString(size);
        args[opts.length + 7] = Integer.toString(maxStack);
        args[opts.length + 8] = Integer.toString(seed);
        return ProcessTools.createLimitedTestJavaProcessBuilder(args);
    }

    private static void testDumpPercentage(boolean bySize) throws Exception {
        ProcessBuilder pb = runManyStacks(1024 * 1024 * 10, 1024 * 16, 5, 172369973,
                                          "-Djava.library.path=" + System.getProperty("java.library.path"),
                                          "-XX:MallocTraceStackDepth=12");
        Process p = ProcessTools.startProcess("runManyStack", pb, x -> System.out.println("> " + x), null, -1, null);
        p.getInputStream().read();
        OutputAnalyzer oa = bySize ? callJcmd(p, "MallocTrace.dump", "-percentage=90") :
                                     callJcmd(p, "MallocTrace.dump", "-sort-by-count", "-percentage=90");
        oa.shouldHaveExitValue(0);
        p.destroy();
        oa.stdoutShouldMatch("Total printed " + (bySize ? "bytes" : "count") + ": .*[(].*90[.][0-9]+ %[)]");
    }

    private static void testUniqueStacks() throws Exception {
        ProcessBuilder pb = runManyStacks(1024 * 1024 * 10, 1024 * 16, 12, 172369975,
                                          "-Djava.library.path=" + System.getProperty("java.library.path"),
                                          "-XX:MallocTraceStackDepth=14");
        Process p = ProcessTools.startProcess("runManyStack", pb, x -> System.out.println("> " + x), null, -1, null);
        p.getInputStream().read();
        OutputAnalyzer oa = callJcmd(p, "MallocTrace.dump", "-percentage=100");
        oa.shouldHaveExitValue(0);
        p.destroy();

        MallocTraceResult result = MallocTraceResult.fromString(oa.getOutput());
        HashSet<Stack> seenStacks = new HashSet<>();

        for (int i = 0; i < result.nrOfStacks(); ++i) {
            if (!seenStacks.add(result.getStack(i))) {
                throw new Exception("Duplicate stack");
            }
        }
    }

    private static ProcessBuilder runStress(int nrOfOps, int maxLiveAllocations, int seed, 
                                            boolean trackLive, String... opts) {
        String[] args = new String[opts.length + 9];
        System.arraycopy(opts, 0, args, 0, opts.length);
        args[opts.length + 0] = "-XX:+UseMallocHooks";
        args[opts.length + 1] = "-XX:+MallocTraceAtStartup";
        args[opts.length + 2] = "-XX:" + (trackLive ? "+" : "-") + "MallocTraceTrackFree";
        args[opts.length + 3] = MallocHooksTest.class.getName();
        args[opts.length + 4] = "stress";
        args[opts.length + 5] = Integer.toString(nrOfOps);
        args[opts.length + 6] = Integer.toString(maxLiveAllocations);
        args[opts.length + 7] = Integer.toString(seed);
        args[opts.length + 8] = Boolean.toString(trackLive);
        return ProcessTools.createLimitedTestJavaProcessBuilder(args);
    }

    private static ProcessBuilder checkEnvProc(String env) {
        String[] args = {MallocHooksTest.class.getName(), "checkEnv", env};
        return ProcessTools.createLimitedTestJavaProcessBuilder(args);
    }

    private static ProcessBuilder checkEnvProcWithHooks(String env) {
        String[] args = {"-XX:+UseMallocHooks", MallocHooksTest.class.getName(),"checkEnv", env};
        return ProcessTools.createLimitedTestJavaProcessBuilder(args);
    }

    private static String getLdPrelodEnv() {
        String env = System.getenv(LD_PRELOAD);

        return env == null ? "" : env;
    }

    private static String absPath(String file) {
        return new File(file).getAbsolutePath().toString();
    }
}

class Stack {
    private String[] funcs;
    private int index;
    private int maxIndex;
    private long bytes;
    private long count;

    public Stack(BufferedReader reader, int index, int maxIndex, long bytes, long count) throws Exception {
        this.index = index;
        this.maxIndex = maxIndex;
        this.bytes = bytes;
        this.count = count;

        ArrayList<String> lines = new ArrayList<>();

        while (true) {
            reader.mark(10);

            if (reader.read() != ' ') {
              reader.reset();
              break;
            }

            lines.add(reader.readLine());
        }

        funcs = new String[lines.size()];

        for (int i = 0; i < funcs.length; ++i) {
            String line = lines.get(i);
            int pos = line.indexOf(']');

            if (pos > 0) {
                funcs[i] = line.substring(pos + 1).trim();
            } else {
                funcs[i] = "<missing>";
            }
        }
    }

    public int getStackIndex() {
        return index;
    }

    public int getMaxStackIndex() {
        return maxIndex;
    }

    public long getBytes() {
        return bytes;
    }

    public long getCount() {
        return count;
    }

    public int getStackDepth() {
        return funcs.length;
    }
    
    public String getFunction(int index) {
        return funcs[index];
    }

    public String toString() {
        StringBuilder result = new StringBuilder();

        result.append("Stack " + index + " of " + maxIndex + ": " + bytes + " bytes, " + 
                      count + " allocations" + System.lineSeparator());

        for (String f: funcs) {
            result.append("  " + f + System.lineSeparator());
        }

        return result.toString();
    }

    public int hashCode() {
        return Arrays.hashCode(funcs);
    }

    public boolean equals(Object other) {
        if (other instanceof Stack) {
            return Arrays.equals(funcs, ((Stack) other).funcs);
        }

        return false;
    }
}

class MallocTraceResult {
    private ArrayList<Stack> stacks = new ArrayList<>();

    public MallocTraceResult(BufferedReader reader) throws Exception {
        String line;

        while (true) {
            line = reader.readLine();

            if (line.startsWith("Stack ")) {
                break;
            }
        }

        while (true) {
            String[] parts = line.split(":|(bytes,)");
            long bytes = Long.parseLong(parts[1].trim().split(" ")[0]);
            long count = Long.parseLong(parts[2].trim().split(" ")[0]);
            int index = Integer.parseInt(parts[0].trim().split(" ")[1]);
            int maxIndex = Integer.parseInt(parts[0].trim().split(" ")[3]);
            stacks.add(new Stack(reader, index, maxIndex, bytes, count));
            line = reader.readLine();

            if (!line.startsWith("Stack ")) {
                break;
            }
        }
    }

    public static MallocTraceResult fromString(String output) throws Exception {
       try (BufferedReader br = new BufferedReader(new StringReader(output))) {
           return new MallocTraceResult(br);
       }
    }

    public int nrOfStacks() {
        return stacks.size();
    }

    public Stack getStack(int index) {
        return stacks.get(index);
    }

    public String toString() {
        StringBuilder result = new StringBuilder();

        for (Stack s: stacks) {
            result.append(s.toString());
        }

        return result.toString();
    }
}

class MallocTraceExpectedStatistic {
    private static  String[] funcs = new String[] {"malloc", "calloc", "realloc", "posix_memalign",
                                                   "memalign", "aligned_alloc", "valloc", "pvalloc"};
    private long[] bytes = new long[funcs.length];
    private long[] counts = new long[funcs.length];

    private static void dumpHsErrorFiles() throws Exception {
        for (File f: new File(".").listFiles()) {
          if (!f.isDirectory() && f.getName().startsWith("hs_err")) {
              System.out.println("Found " + f.getName() + ":");
              System.out.println(new String(Files.readAllBytes(f.toPath())));
              System.out.println("------- End of " + f.getName());
          }
        }
    }
    public MallocTraceExpectedStatistic(Process p) throws Exception {
        BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()));

        for (int i = 0; i < bytes.length; ++i) {
           String line = br.readLine();
           System.out.println(i + ": " + line);
           String[] parts = line.split(" ");
           if (parts[0].equals("#")) {
               dumpHsErrorFiles();
           }
           bytes[i] = Long.parseLong(parts[0]);
           counts[i] = Long.parseLong(parts[1]);
        }
    }

    public void check(MallocTraceResult actual) throws Exception {
        check(actual, 1, 0.0);
    }

    public void check(MallocTraceResult actual, int nth, double diff) throws Exception {
        for (int i = 0; i < funcs.length; ++i) {
            if (counts[i] == 0) {
                continue; // Not supported by platform
            }

            boolean found = false;

            for (int j = 0; j < actual.nrOfStacks(); ++j) {
                Stack stack = actual.getStack(j);

                if (stack.getFunction(0).startsWith(funcs[i] + (Platform.isOSX() ? "_interpose " : " "))) {
                    assertFalse(found, "Found entry for " + funcs[i] + " more than once");
                    long expected_bytes = bytes[i] / nth;
                    long expected_count = counts[i] / nth;
                    // Check we are in the range of +/- 20 percent.
                    assertLTE(stack.getBytes(), (long) (expected_bytes * (1 + diff)));
                    assertGTE(stack.getBytes(), (long) (expected_bytes * (1 - diff)));
                    assertLTE(stack.getCount(), (long) (expected_count * (1 + diff)));
                    assertGTE(stack.getCount(), (long) (expected_count * (1 - diff)));
                    found = true;
                }
            }

            assertTrue(found, "Didn't found entry for " + funcs[i]);
        }
    }

    public String toString() {
        StringBuilder result = new StringBuilder("Expacted results:" + System.lineSeparator());

        for (int i = 0; i < funcs.length; ++i) {
            result.append(funcs[i] + ": " + bytes[i] + " bytes, " + counts[i] + " counts" + System.lineSeparator());
        }

        return result.toString();
    }
}

