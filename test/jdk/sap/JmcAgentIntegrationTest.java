/**
 * @test
 * @summary Runs the test for the jcm agent integration.
 *
 * @run main/othervm -Dcp=agent-1.0.1-SNAPSHOT-sap-tests.jar JmcAgentIntegrationTest
 */

import java.lang.reflect.Method;
import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;

public class JmcAgentIntegrationTest {

    public static void main(String[] args) throws Exception {
        File jar = new File(System.getenv("TEST_IMAGE_DIR") + "/jars/agent-tests.jar");

        if (!jar.exists()) {
            return; // Feature was not enabled.
        }

        ArrayList<String> testOptions = new ArrayList<>();
        testOptions.add("-dump");

        // Don't run tests if the ASM version we are using cannot handle the class file spec of this VM.
        int spec = Integer.getInteger("java.vm.specification.version", 99999);
        String javaHome = System.getProperty("java.home");
        File agentJar = new File(javaHome + "/lib/agent.jar");
		ClassLoader parentLoader = JmcAgentIntegrationTest.class.getClassLoader();
        URLClassLoader agentLoader = new URLClassLoader(new URL[] {agentJar.toURI().toURL()}, parentLoader);
        try {
             Class.forName("org.openjdk.jmc.internal.org.objectweb.asm.Opcodes", true, agentLoader).getDeclaredField("V" + spec);
        } catch (NoSuchFieldException e) {
            System.out.println("Incompatible class file version. Skipping VM specific tests.");
            testOptions.add("-vm-agnostic-tests");
        }

        URL url = jar.toURI().toURL();
        String classPath = System.getProperty("java.class.path", ".");

        System.setProperty("java.class.path", classPath + System.getProperty("path.separator") + jar.toString());
        System.setProperty("useJmcAgentOption", "true");
        System.setProperty("traceExecs", "true");

        URLClassLoader cl = new URLClassLoader(new URL[] {url}, parentLoader);
        Class<?> testClass = Class.forName("org.openjdk.jmc.agent.sap.test.TestRunner", true, cl);
        Method mainMethod = testClass.getDeclaredMethod("main", String[].class, String[].class);
        mainMethod.invoke(null, new Object[] {testOptions.toArray(new String[0]), new String[] {"-XX:+EnableDynamicAgentLoading"}});
    }
}
