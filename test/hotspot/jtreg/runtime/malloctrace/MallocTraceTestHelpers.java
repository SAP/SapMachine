import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class MallocTraceTestHelpers {

    // Returns <major> <minor> in the form:
    // 0x0000MMmm
    public static int GlibVersion() throws Throwable {
        OutputAnalyzer output = ProcessTools.executeProcess("/usr/bin/ldd", "--version");
        output.reportDiagnosticSummary();
        String version = output.firstMatch("^ldd.* ([\\d+\\.]+)$", 1);
        System.out.println("Glibc version is " + version);
        if (version == null) {
            return 0; // unknown, assume very low
        }
        String[] parts = version.split("\\.");
        int major = Integer.parseInt(parts[0]);
        int minor = Integer.parseInt(parts[1]);
        int as_numeric = minor | (major << 8);
        return as_numeric;
    }

    public static boolean GlibcSupportsMallocHooks() throws Throwable {
        return GlibVersion() < 0x220; // 2.32
    }

}
