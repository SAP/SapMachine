/*
 * Copyright (c) 2025 SAP SE. All rights reserved.
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
 * @summary Runs the test for the SAP JMC agent integration.
 *
 * @run main/othervm JmcAgentIntegrationTest
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

        // Only run VM-agnostic tests if the ASM version we are using cannot handle the class file spec of this VM.
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
