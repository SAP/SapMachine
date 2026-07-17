/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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
 * @bug 8385665
 * @summary Tests for Math.pow with a large exponent
 * @build Tests
 * @build PowLargeExpTests
 * @run main PowLargeExpTests
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions
 *                   -XX:ControlIntrinsic=-_dpow
 *                    PowLargeExpTests
 */

public class PowLargeExpTests {
    private PowLargeExpTests(){}

    public static void main(String... args) {
        int failures = 0;
        // Probe arguments where stock FDLIBM 5.3 has a large error.
        double[][] testCases = {
            // {x, y, expected pow(x, y)}
            {
                0x1.000002c5e2e99p+0,   // |x| > 1
                0x1.c9eee35374af6p+31,  // |y| huge
                0x1.ffffe0bc9e399p+915
            },

            {
                0x1.fffff4e900013p-1,   // |x| < 1
                0x1.0000100000001p+31,  // |y| huge
                0x0.421378008b246p-1022
            },

        };

        for (double[] testCase: testCases) {
            failures += testPowCase(testCase[0], testCase[1], testCase[2]);
        }

        if (failures > 0) {
            System.err.println("Testing pow incurred " + failures + " failures.");
            throw new RuntimeException();
        }
    }

    private static int testPowCase(double input1, double input2, double expected) {
        int failures = 0;
        // With the general quality of implementation requirements of
        // the pow method, the results of Math.pow(x, y) should be
        // within two ulps of the expected reference value. This could
        // be narrowed to one ulp if it was known if the reference
        // value was rounded up or down.
        failures += Tests.testUlpDiff("Math.pow(double)", input1, input2,
                                      Math::pow, expected, 2.0);
        return failures;
    }
}