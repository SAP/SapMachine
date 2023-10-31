/**
 * @test
 * @summary Test functionality of the malloc hooks library.
 * @library /test/lib
 *
 * @run main/othervm/native MallocHooksTest
 */

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.io.StringReader;
import java.util.ArrayList;

import jdk.test.lib.JDKToolFinder;
import jdk.test.lib.Platform;
import jdk.test.lib.Utils;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

public class MallocHooksTest {
    static native void doRandomMemOps(int nrOfOps, int maxLiveAllocations, int seed,
                                      boolean trackLive, long[] sizes, long[] counts);

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
            throw new Exception();
        }

        if (args[0].equals("checkEnv")) {
            if (args[1].equals(getLdPrelodEnv())) {
               return;
            }

            throw new Exception("Expected " + LD_PRELOAD + "=\"" + args[1] + "\", but got " +
                                LD_PRELOAD + "=\"" +  getLdPrelodEnv() + "\"");
        } else if (args[0].equals("stress")) {
            doStress(args);

            while (true) {
              Thread.sleep(1000);
            }
        } else {
            throw new Exception("Unknown command " + args[0]);
        }
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
        pb = checkEnvProc(LIB_FAKE_MALLOC_HOOKS);
        pb.environment().put(LD_PRELOAD, LIB_FAKE_MALLOC_HOOKS + ":" + LIB_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        pb = checkEnvProcWithHooks("");
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        pb = checkEnvProcWithHooks("");
        pb.environment().put(LD_PRELOAD, LIB_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        pb = checkEnvProcWithHooks(LIB_FAKE_MALLOC_HOOKS);
        pb.environment().put(LD_PRELOAD, LIB_MALLOC_HOOKS + ":" + LIB_FAKE_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
        pb = checkEnvProcWithHooks(LIB_FAKE_MALLOC_HOOKS);
        pb.environment().put(LD_PRELOAD, LIB_FAKE_MALLOC_HOOKS + ":" + LIB_MALLOC_HOOKS);
        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
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
        return new OutputAnalyzer(pb.start());
    }

    private static MallocTraceResult parseOutput(String output) throws Exception {
       try (BufferedReader br = new BufferedReader(new StringReader(output))) {
           return new MallocTraceResult(br);
       }
    }

    private static void readResult(Process p, long[] bytes, long[] counts) throws Exception {
        BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()));
        for (int i = 0; i < bytes.length; ++i) {
           String[] parts = br.readLine().split(" ");
           bytes[i] = Long.parseLong(parts[0]);
           counts[i] = Long.parseLong(parts[1]);
        }
    }

    private static void testTracking(boolean trackLive) throws Exception {
        ProcessBuilder pb = runStress(1024 * 1024 * 10, 65536, 172369973, trackLive,
                                      "-XX:-MallocTraceTrackFrees", "-XX:MallocTraceStackDepth=2");
        Process p = pb.start();
        long[] bytes = new long[8];
        long[] counts = new long[8];
        readResult(p, bytes, counts);
        for (int i = 0; i < bytes.length; ++i) {
            System.out.println(bytes[i] + " " + counts[i]);
        }
        OutputAnalyzer oa = callJcmd(p, "MallocTrace.dump", "-max-entries=10", "-sort-by-count",
                                        "-filter=Java_MallocHooksTest_doRandomMemOps");
        oa.shouldHaveExitValue(0);
        p.destroy();
        System.out.println(oa.getOutput());
        System.out.println(parseOutput(oa.getOutput()).toString());
        if (trackLive) {
            oa.shouldContain("Contains the currently allocated memory since enabling");
        } else {
            oa.shouldContain("Contains every allocation done since enabling");
        }
        oa.shouldContain("libtestmallochooks.");
        oa.shouldContain("Total allocated bytes");
        oa.shouldContain("Total printed count");
        //oa.shouldContain("Stack 1 of");
        //oa.shouldContain("Stack 8 of");
        //oa.shouldNotContain("Stack 0 of");
        //oa.shouldNotContain("Stack 9 of");
    }

    private static ProcessBuilder runStress(int nrOfOps, int maxLiveAllocations, int seed, 
                                            boolean trackLive, String... opts) {
        String[] args = new String[opts.length + 9];
        System.arraycopy(opts, 0, args, 0, opts.length);
        args[opts.length + 0] = "-XX:+UseMallocHooks";
        args[opts.length + 1] = "-XX:+MallocTraceAtStartup";
        args[opts.length + 2] = "-XX:" + (trackLive ? "+" : "-") + "MallocTraceTrackFrees";
        args[opts.length + 3] = MallocHooksTest.class.getName();
        args[opts.length + 4] = "stress";
        args[opts.length + 5] = Integer.toString(nrOfOps);
        args[opts.length + 6] = Integer.toString(maxLiveAllocations);
        args[opts.length + 7] = Integer.toString(seed);
        args[opts.length + 8] = Boolean.toString(trackLive);
        return ProcessTools.createJavaProcessBuilder(args);
    }

    private static ProcessBuilder checkEnvProc(String env) {
        return ProcessTools.createJavaProcessBuilder(MallocHooksTest.class.getName(), "checkEnv", env);
    }

    private static ProcessBuilder checkEnvProcWithHooks(String env) {
        return ProcessTools.createJavaProcessBuilder("-XX:+UseMallocHooks", MallocHooksTest.class.getName(),
                                                     "checkEnv", env);
    }

    private static String getLdPrelodEnv() {
        String env = System.getenv(LD_PRELOAD);

        return env == null ? "" : env;
    }

    private static String absPath(String file) {
        return new File(file).getAbsolutePath().toString();
    }

    static {
        System.loadLibrary("testmallochooks");
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


