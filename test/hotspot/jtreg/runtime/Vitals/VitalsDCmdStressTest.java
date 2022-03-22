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
 * @run testng/othervm -XX:VitalsSampleInterval=1 VitalsDCmdStressTest
 */

import jdk.test.lib.dcmd.CommandExecutor;
import jdk.test.lib.dcmd.JMXExecutor;
import jdk.test.lib.process.OutputAnalyzer;
import org.testng.annotations.Test;

public class VitalsDCmdStressTest {

    final long runtime_secs = 30;

    public void run_once(CommandExecutor executor, boolean silent) {
        OutputAnalyzer output = executor.execute("VM.vitals now", silent);
        VitalsTestHelper.outputMatchesVitalsTextMode(output);
        output.shouldContain("Now");
    }

    public void run(CommandExecutor executor) {
        // Let vitals run a while and bombard it with report requests which include "now" sampling
        long t1 = System.currentTimeMillis();
        long t2 = t1 + runtime_secs * 1000;
        int invocations = 0;
        while (System.currentTimeMillis() < t2) {
            run_once(executor, true); // run silent to avoid filling logs with garbage output
            invocations ++;
        }

        // One last time run with silent off
        run_once(executor, false);
        invocations ++;

        System.out.println("Called " + invocations + " times.");
    }

    @Test
    public void jmx() {
        run(new JMXExecutor());
        // wait two seconds to collect some samples, then repeat with filled tables
        try {
            Thread.sleep(2000);
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
        run(new JMXExecutor());
    }

}
