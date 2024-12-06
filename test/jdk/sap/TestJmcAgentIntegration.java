/**
 * @test
 * @summary Runs the test for the jcm agent integration.
 *
 * @run main/othervm -Dcp=agent-1.0.1-SNAPSHOT-sap-tests.jar TestJmcAgentIntegration
 */

import java.lang.reflect.Method;
import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;

public class TestJmcAgentIntegration {

    public static void main(String[] args) throws Exception {
        File jar = new File(System.getenv("TEST_IMAGE_DIR") + "/jars/agent-tests.jar");
        if (!jar.exists()) {
            return; // Feature was not enabled.
        }
        URL url = jar.toURI().toURL();
        String classPath = System.getProperty("java.class.path", ".");

        System.setProperty("java.class.path", classPath + System.getProperty("path.separator") + jar.toString());
        System.setProperty("useJmcAgentOption", "true");
        System.setProperty("traceExecs", "true");

        URLClassLoader cl = new URLClassLoader(new URL[] {url}, TestJmcAgentIntegration.class.getClassLoader());
        Class<?> testClass = Class.forName("org.openjdk.jmc.agent.sap.test.TestRunner", true, cl);
        Method mainMethod = testClass.getDeclaredMethod("main", String[].class);
        mainMethod.invoke(null, new Object[] {new String[0]});
    }
}
