/**
 * @test
 * @summary Test functionality of the malloc hooks library.
 * @library /test/lib
 *
 * @run main/native MallocHooksTest
 */

import jdk.test.lib.Platform;
import jdk.test.lib.Utils;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

public class MallocHooksTest {

    public static void main(String args[]) throws Exception {
        String libDir = System.getProperty("sun.boot.library.path") + "/";
        ProcessBuilder pb = ProcessTools.createNativeTestProcessBuilder("testmallochooks");

        if (Platform.isLinux()) {
            pb.environment().put("LD_PRELOAD", libDir + "libmallochooks.so");
        } else if (Platform.isOSX()) {
           pb.environment().put("DYLD_INSERT_LIBRARIES", libDir + "libmallochooks.dylib");
        } else {
            return;
        }

        new OutputAnalyzer(pb.start()).shouldHaveExitValue(0);
    }
}
