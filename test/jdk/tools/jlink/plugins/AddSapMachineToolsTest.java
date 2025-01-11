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
 */

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

import org.testng.SkipException;
import org.testng.annotations.Test;

import jdk.test.lib.Platform;

import static org.testng.Assert.assertFalse;
import static org.testng.Assert.assertTrue;

import tests.Helper;

/* @test
 * @summary Test the --add-sapmachine-tools plugin
 * @library ../../lib
 * @library /test/lib
 * @modules java.base/jdk.internal.jimage
 *          jdk.jlink/jdk.tools.jimage
 * @run testng AddSapMachineToolsTest
 */
@Test
public class AddSapMachineToolsTest {

    private final String[] sapMachineTools = {
            "bin/asprof",
            "lib/" + System.mapLibraryName("asyncProfiler"),
            "lib/async-profiler.jar",
            "lib/converter.jar",
            "legal/async/CHANGELOG.md",
            "legal/async/LICENSE",
            "legal/async/README.md"
    };

    @Test
    public void testSapMachineTools() throws IOException {
        // async profiler is not pulled in GHA builds, so skip the test there.
        // checking whether we are in a GHA environment is hacky because jtreg removes environment variables,
        // so we guess by checking for a user name containing the String "runner"
        if (System.getProperty("user.name", "n/a").contains("runner")) {
            throw new SkipException("Detected a Github Actions environment. No tools get added to SapMachine here, so skip test.");
        }

        Helper helper = Helper.newHelper();
        if (helper == null) {
            throw new SkipException("JDK image is not suitable for this test.");
        }

        // async profiler is only available on a subset of platforms
        boolean shouldHaveAsync = Platform.isOSX() ||
                (Platform.isLinux() && (Platform.isAArch64() || Platform.isPPC() || Platform.isX64()) && !Platform.isMusl());

        Path sourceJavaHome = Path.of(System.getProperty("java.home"));

        if (shouldHaveAsync) {
            for (String tool : sapMachineTools) {
                assertTrue(Files.exists(sourceJavaHome.resolve(tool)), tool + " must exist.");
            }
            System.out.println("All SapMachine tools files found, as expected.");
        } else {
            for (String tool : sapMachineTools) {
                assertFalse(Files.exists(sourceJavaHome.resolve(tool)), tool + " should not exist.");
            }
            System.out.println("No SapMachine tools files found, as expected.");
        }

        var module = "sapmachine.tools";
        helper.generateDefaultJModule(module);
        var image = helper
                .generateDefaultImage(new String[] { "--add-sapmachine-tools" }, module)
                .assertSuccess();

        if (shouldHaveAsync) {
            helper.checkImage(image, module, null, null, sapMachineTools);
        } else {
            helper.checkImage(image, module, null, sapMachineTools, null);
        }
    }
}
