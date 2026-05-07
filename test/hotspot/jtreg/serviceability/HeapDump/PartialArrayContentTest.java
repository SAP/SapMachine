/*
 * Copyright (c) 2026 SAP SE. All rights reserved.
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

import java.io.File;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.concurrent.TimeUnit;

import jdk.test.lib.Asserts;
import jdk.test.lib.JDKToolLauncher;
import jdk.test.lib.apps.LingeredApp;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.hprof.model.*;
import jdk.test.lib.hprof.parser.Reader;

/*
 * @test
 * @summary Checks if -XX:+LimitPrimArrayContentInHeapDump works.
 * @library /test/lib
 * @run driver PartialArrayContentTest
 */
class ArrayAllocApp extends LingeredApp {
    public static int arraySize = 54321;

    public static void main(String[] args) {
        boolean[] za = new boolean[arraySize];
        byte[] ba = new byte[arraySize];
        short[] sa = new short[arraySize];
        char[] ca = new char[arraySize];
        int[] ia = new int[arraySize];
        long[] ja = new long[arraySize];
        float[] fa = new float[arraySize];
        double[] da = new double[arraySize];

        for (int i = 0; i < arraySize; ++i) {
            za[i] = true;
            ba[i] = (byte) 1;
            sa[i] = (short) 1;
            ca[i] = '1';
            ia[i] = 1;
            ja[i] = 1;
            fa[i] = 1.0f;
            da[i] = 1.0;
        }
        LingeredApp.main(args);
    }
}

public class PartialArrayContentTest {
    private static int charLikeLimit = 120;
    private static int nonCharLikeLimit = 80;

    public static void main(String[] args) throws Exception {
        File dumpFile = new File("partialarrays.hprof");
        createDump(dumpFile);
        verifyDump(dumpFile);
    }

    private static void createDump(File dumpFile) throws Exception {
        LingeredApp theApp = null;
        try {
            theApp = new ArrayAllocApp();
            LingeredApp.startApp(theApp, "-XX:+LimitPrimArrayContentInHeapDump", 
                "-XX:StringLikeContentSizeLimitInHeapDump=" + charLikeLimit,
                "-XX:ArrayContentSizeLimitInHeapDump=" + nonCharLikeLimit);

            //jcmd <pid> GC.heap_dump <file_path>
            JDKToolLauncher launcher = JDKToolLauncher
                    .createUsingTestJDK("jcmd")
                    .addToolArg(Long.toString(theApp.getPid()))
                    .addToolArg("GC.heap_dump")
                    .addToolArg(dumpFile.getAbsolutePath());
            Process p = ProcessTools.startProcess("jcmd", new ProcessBuilder(launcher.getCommand()));

            while (!p.waitFor(5, TimeUnit.SECONDS)) {
                if (!theApp.getProcess().isAlive()) {
                    p.destroyForcibly();
                    throw new Exception("Target VM died");
                }
            }

            Asserts.assertEquals(p.exitValue(), 0);
        } finally {
            LingeredApp.stopApp(theApp);
        }
    }

    private static void verifyDump(File dumpFile) throws Exception {
        Asserts.assertTrue(dumpFile.exists(), "Heap dump file not found.");

        try (Snapshot snapshot = Reader.readFile(dumpFile.getPath(), true, 0)) {
            snapshot.resolve(true);
            Enumeration<JavaHeapObject> things = snapshot.getThings();
            HashSet<Character> expectedTypes = new HashSet<>();
            expectedTypes.add('Z');
            expectedTypes.add('B');
            expectedTypes.add('S');
            expectedTypes.add('C');
            expectedTypes.add('I');
            expectedTypes.add('J');
            expectedTypes.add('F');
            expectedTypes.add('D');

            while (things.hasMoreElements()) {
                JavaHeapObject obj = things.nextElement();

                if (obj instanceof JavaValueArray) {
                    JavaValueArray array = (JavaValueArray) obj;

                    if (array.getLength() != ArrayAllocApp.arraySize) {
                        continue;
                    }

                    char type = (char) array.getElementType();
                    Asserts.assertTrue(expectedTypes.remove(type));
                    int limit = ((type == 'B') || (type == 'C')) ? charLikeLimit : nonCharLikeLimit;
                    JavaThing[] values = array.getElements();

                    String exp1 = "<invalid>";
                    String exp2 = "<invalid>";

                    switch (type) {
                        case 'Z':
                            exp1 = "true";
                            exp2 = "false";
                            break;
                        case 'B':
                            exp1 = "0x1";
                            exp2 = "0x0";
                            break;
                        case 'S':
                        case 'I':
                        case 'J':
                            exp1 = "1";
                            exp2 = "0";
                            break;
                        case 'C':
                            exp1 = "1";
                            exp2 = "" + (char) 0;
                            break;
                        case 'F':
                        case 'D':
                            exp1 = "1.0";
                            exp2 = "0.0";
                            break;
                    }

                    for (int i = 0; i < limit; ++i) {
                        Asserts.assertEquals(exp1, values[i].toString());
                    }

                    for (int i = limit; i < ArrayAllocApp.arraySize; ++i) {
                        Asserts.assertEquals(exp2, values[i].toString());
                    }
                }
            }

            Asserts.assertTrue(expectedTypes.isEmpty());
        }
    }
}