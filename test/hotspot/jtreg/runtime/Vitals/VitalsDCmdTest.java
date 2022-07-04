/*
 * Copyright (c) 2020,2022 SAP SE. All rights reserved.
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
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=1 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=2 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=3 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=4 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=5 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=6 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=7 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=8 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=9 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=10 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

/*
 * @test
 * @summary Test of diagnostic command VM.vitals
 * @library /test/lib
 * @modules java.base/jdk.internal.misc java.compiler java.management jdk.internal.jvmstat/sun.jvmstat.monitor
 * @run testng/othervm -Dsapmachine.vitalstest=11 -XX:VitalsSampleInterval=1 VitalsDCmdTest
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.dcmd.CommandExecutor;
import jdk.test.lib.dcmd.JMXExecutor;
import org.testng.annotations.Test;

public class VitalsDCmdTest {

    public void run(CommandExecutor executor) {

        try {

            int testnumber = Integer.parseInt(System.getProperties().getProperty("sapmachine.vitalstest"));

            switch (testnumber) {
                case 1:
                    OutputAnalyzer output = executor.execute("VM.vitals");
                    VitalsTestHelper.outputMatchesVitalsTextMode(output);
                    output.shouldMatch("\\d+[gkm]"); // we print by default in "dynamic" scale which should show some values as k or m or g
                    output.shouldNotContain("Now"); // off by default
                    break;
                case 2:
                    output = executor.execute("VM.vitals reverse");
                    VitalsTestHelper.outputMatchesVitalsTextMode(output);
                    output.shouldNotContain("Now"); // off by default
                    break;
                case 3:
                    output = executor.execute("VM.vitals scale=m");
                    VitalsTestHelper.outputMatchesVitalsTextMode(output);
                    output.shouldNotMatch("\\d+[km]"); // A specific scale disables dynamic scaling, and we omit the unit suffix
                    output.shouldNotContain("Now"); // off by default
                    break;
                case 4:
                    output = executor.execute("VM.vitals scale=1");
                    VitalsTestHelper.outputMatchesVitalsTextMode(output);
                    output.shouldNotMatch("\\d+[km]"); // A specific scale disables dynamic scaling, and we omit the unit suffix
                    output.shouldNotContain("Now"); // off by default
                    break;
                case 5:
                    output = executor.execute("VM.vitals raw");
                    VitalsTestHelper.outputMatchesVitalsTextMode(output);
                    output.shouldNotContain("Now"); // off by default
                    break;
                case 6:
                    output = executor.execute("VM.vitals now");
                    VitalsTestHelper.outputMatchesVitalsTextMode(output);
                    output.shouldContain("Now");
                    break;
                case 7:
                    output = executor.execute("VM.vitals reverse now");
                    VitalsTestHelper.outputMatchesVitalsTextMode(output);
                    output.shouldContain("Now");
                    break;
                case 8:
                    output = executor.execute("VM.vitals csv");
                    VitalsTestHelper.outputMatchesVitalsCSVMode(output);
                    output.shouldNotContain("Now"); // off always in csv mode
                    VitalsTestHelper.parseCSV(output);
                    break;
                case 9:
                    output = executor.execute("VM.vitals csv reverse");
                    VitalsTestHelper.outputMatchesVitalsCSVMode(output);
                    output.shouldNotContain("Now"); // off always in csv mode
                    VitalsTestHelper.parseCSV(output);
                    break;
                case 10: {
                    output = executor.execute("VM.vitals csv reverse raw");
                    VitalsTestHelper.outputMatchesVitalsCSVMode(output);
                    output.shouldNotContain("Now"); // off always in csv mode
                    CSVParser.CSV csv = VitalsTestHelper.parseCSV(output);
                    VitalsTestHelper.simpleCSVSanityChecks(csv); // requires raw or scale=1 mode
                }
                break;
                case 11: {
                    output = executor.execute("VM.vitals csv now reverse scale=1");
                    VitalsTestHelper.outputMatchesVitalsCSVMode(output);
                    // "Now" sample printing always off in csv mode even if explicitly given.
                    output.shouldNotContain("Now");
                    output.shouldContain("\"now\" ignored in csv mode");
                    CSVParser.CSV csv = VitalsTestHelper.parseCSV(output);
                    VitalsTestHelper.simpleCSVSanityChecks(csv); // requires raw or scale=1 mode
                }
                break;
                default:
                    throw new RuntimeException("unknown test number " + testnumber);
            }

        } catch (Exception e) {
            e.printStackTrace();
            throw new RuntimeException(e.getMessage());
        }

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
