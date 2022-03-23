/*
 * Copyright (c) 2020, SAP SE. All rights reserved.
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

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc
 *          java.compiler
 *          java.management
 *          jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.dcmd.CommandExecutor;
import jdk.test.lib.dcmd.JMXExecutor;
import org.testng.annotations.Test;

public class VitalsDCmdTest {

    public void run(CommandExecutor executor) {

        OutputAnalyzer output = executor.execute("VM.vitals");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);
        output.shouldMatch("\\d+[gkm]"); // we print by default in "dynamic" scale which should show some values as k or m or g
        output.shouldNotContain("Now"); // off by default

        output = executor.execute("VM.vitals reverse");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);

        output = executor.execute("VM.vitals scale=m");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);
        output.shouldNotMatch("\\d+[km]"); // A specific scale disables dynamic scaling, and we omit the unit suffix

        output = executor.execute("VM.vitals scale=1");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);
        output.shouldNotMatch("\\d+[km]"); // A specific scale disables dynamic scaling, and we omit the unit suffix

        output = executor.execute("VM.vitals raw");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);

        output = executor.execute("VM.vitals now");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);

        output = executor.execute("VM.vitals reverse now");
        VitalsTestHelper.outputMatchesVitalsTextMode(output);

        output = executor.execute("VM.vitals csv");
        VitalsTestHelper.outputMatchesVitalsCSVMode(output);
        output.shouldNotContain("Now"); // off always in csv mode

        output = executor.execute("VM.vitals csv reverse");
        VitalsTestHelper.outputMatchesVitalsCSVMode(output);

        output = executor.execute("VM.vitals csv reverse raw");
        VitalsTestHelper.outputMatchesVitalsCSVMode(output);

        output = executor.execute("VM.vitals csv now reverse raw");
        VitalsTestHelper.outputMatchesVitalsCSVMode(output);
    }

    @Test
    public void jmx() {
        try {
            Thread.sleep(3000);
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
        run(new JMXExecutor());
    }

}
