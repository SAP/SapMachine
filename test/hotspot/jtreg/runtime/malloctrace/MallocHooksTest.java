/**
 * @test
 * @summary Test functionality of the malloc hooks library.
 * @library /test/lib
 *
 * @run main/othervm/native MallocHooksTest
 */

import java.io.File;

import jdk.test.lib.JDKToolFinder;
import jdk.test.lib.Platform;
import jdk.test.lib.Utils;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

public class MallocHooksTest {
    static native void doRandomMemOps(int nrOfOps, int maxLiveAllocations);

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
            testTracking();

            return;
        }

        if (args[0].equals("checkEnv")) {
            if (args[1].equals(getLdPrelodEnv())) {
               return;
            }

            throw new Exception("Expected " + LD_PRELOAD + "=\"" + args[1] + "\", but got " +
                                LD_PRELOAD + "=\"" +  getLdPrelodEnv() + "\"");
        } else if (args[0].equals("stress")) {
            doStress(args);
        } else {
            throw new Exception("Unknown command " + args[0]);
        }
    }

    private static void doStress(String[] args) {
        int nrOfOps = Integer.parseInt(args[1]);
        int maxLiveAllocations = Integer.parseInt(args[2]);
        int runs = Integer.parseInt(args[3]);

        for (int i = 0; i < runs; ++i) {
            doRandomMemOps(nrOfOps, maxLiveAllocations);
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

    private static void testTracking() throws Exception {
        ProcessBuilder pb = runStress(10, 1024 * 1024 * 10, 65536, "-XX:-MallocTraceTrackFrees");
        Process p = pb.start();
        OutputAnalyzer oa = callJcmd(p, "MallocTrace.dump -max-entries=10");
        oa.shouldHaveExitValue(0);
        p.destroy();
        oa.shouldContain("Contains every allocation done since enabling");
        oa.shouldContain("libtestmallochooks.");
        oa.shouldContain("Total allocated bytes");
        oa.shouldContain("Total printed count");
        oa.shouldContain("Stack 1 of");
        oa.shouldContain("Stack 10 of");
        oa.shouldNotContain("Stack 0 of");
        oa.shouldNotContain("Stack 11 of");
    }

    private static ProcessBuilder runStress(int runs, int nrOfOps, int maxLiveAllocations, String... opts) {
        String[] args = new String[opts.length + 7];
        System.arraycopy(opts, 0, args, 0, opts.length);
        args[opts.length + 0] = "-XX:+UseMallocHooks";
        args[opts.length + 1] = "-XX:+MallocTraceAtStartup";
        args[opts.length + 2] = MallocHooksTest.class.getName();
        args[opts.length + 3] = "stress";
        args[opts.length + 4] = Integer.toString(runs);
        args[opts.length + 5] = Integer.toString(nrOfOps);
        args[opts.length + 6] = Integer.toString(maxLiveAllocations);
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
