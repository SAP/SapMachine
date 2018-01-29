/*
 * Copyright (c) 2012, 2018, Oracle and/or its affiliates. All rights reserved.
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
 * @bug      8005092 6469562
 * @summary  Test repeated annotations output.
 * @author   bpatel
 * @library  ../lib
 * @modules jdk.javadoc/jdk.javadoc.internal.tool
 * @build    JavadocTester
 * @run main TestRepeatedAnnotations
 */

public class TestRepeatedAnnotations extends JavadocTester {

    public static void main(String... args) throws Exception {
        TestRepeatedAnnotations tester = new TestRepeatedAnnotations();
        tester.runTests();
    }

    @Test
    void test() {
        javadoc("-d", "out",
                "-sourcepath", testSrc,
                "pkg", "pkg1");
        checkExit(Exit.OK);

        checkOutput("pkg/C.html", true,
                "<a href=\"ContaineeSynthDoc.html\" "
                + "title=\"annotation in pkg\">@ContaineeSynthDoc</a> "
                + "<a href=\"ContaineeSynthDoc.html\" "
                + "title=\"annotation in pkg\">@ContaineeSynthDoc</a>",
                "<a href=\"ContaineeRegDoc.html\" "
                + "title=\"annotation in pkg\">@ContaineeRegDoc</a> "
                + "<a href=\"ContaineeRegDoc.html\" "
                + "title=\"annotation in pkg\">@ContaineeRegDoc</a>",
                "<a href=\"RegContainerDoc.html\" "
                + "title=\"annotation in pkg\">@RegContainerDoc</a>"
                + "({"
                + "<a href=\"RegContaineeNotDoc.html\" "
                + "title=\"annotation in pkg\">@RegContaineeNotDoc</a>,"
                + "<a href=\"RegContaineeNotDoc.html\" "
                + "title=\"annotation in pkg\">@RegContaineeNotDoc</a>})");

        checkOutput("pkg/D.html", true,
                "<a href=\"RegDoc.html\" title=\"annotation in pkg\">@RegDoc</a>"
                + "(<a href=\"RegDoc.html#x--\">x</a>=1)",
                "<a href=\"RegArryDoc.html\" title=\"annotation in pkg\">@RegArryDoc</a>"
                + "(<a href=\"RegArryDoc.html#y--\">y</a>=1)",
                "<a href=\"RegArryDoc.html\" title=\"annotation in pkg\">@RegArryDoc</a>"
                + "(<a href=\"RegArryDoc.html#y--\">y</a>={1,2})",
                "<a href=\"NonSynthDocContainer.html\" "
                + "title=\"annotation in pkg\">@NonSynthDocContainer</a>"
                + "("
                + "<a href=\"RegArryDoc.html\" title=\"annotation in pkg\">@RegArryDoc</a>"
                + "(<a href=\"RegArryDoc.html#y--\">y</a>=1))");

        checkOutput("pkg1/C.html", true,
                "<a href=\"RegContainerValDoc.html\" "
                + "title=\"annotation in pkg1\">@RegContainerValDoc</a>"
                + "(<a href=\"RegContainerValDoc.html#value--\">value</a>={"
                + "<a href=\"RegContaineeNotDoc.html\" "
                + "title=\"annotation in pkg1\">@RegContaineeNotDoc</a>,"
                + "<a href=\"RegContaineeNotDoc.html\" "
                + "title=\"annotation in pkg1\">@RegContaineeNotDoc</a>},"
                + "<a href=\"RegContainerValDoc.html#y--\">y</a>=3)",
                "<a href=\"ContainerValDoc.html\" "
                + "title=\"annotation in pkg1\">@ContainerValDoc</a>"
                + "(<a href=\"ContainerValDoc.html#value--\">value</a>={"
                + "<a href=\"ContaineeNotDoc.html\" "
                + "title=\"annotation in pkg1\">@ContaineeNotDoc</a>,"
                + "<a href=\"ContaineeNotDoc.html\" "
                + "title=\"annotation in pkg1\">@ContaineeNotDoc</a>},"
                + "<a href=\"ContainerValDoc.html#x--\">x</a>=1)");

        checkOutput("pkg/C.html", false,
                "<a href=\"RegContaineeDoc.html\" "
                + "title=\"annotation in pkg\">@RegContaineeDoc</a> "
                + "<a href=\"RegContaineeDoc.html\" "
                + "title=\"annotation in pkg\">@RegContaineeDoc</a>",
                "<a href=\"RegContainerNotDoc.html\" "
                + "title=\"annotation in pkg\">@RegContainerNotDoc</a>"
                + "(<a href=\"RegContainerNotDoc.html#value--\">value</a>={"
                + "<a href=\"RegContaineeNotDoc.html\" "
                + "title=\"annotation in pkg\">@RegContaineeNotDoc</a>,"
                + "<a href=\"RegContaineeNotDoc.html\" "
                + "title=\"annotation in pkg\">@RegContaineeNotDoc</a>})");

        checkOutput("pkg1/C.html", false,
                "<a href=\"ContaineeSynthDoc.html\" "
                + "title=\"annotation in pkg1\">@ContaineeSynthDoc</a> "
                + "<a href=\"ContaineeSynthDoc.html\" "
                + "title=\"annotation in pkg1\">@ContaineeSynthDoc</a>",
                "<a href=\"RegContainerValNotDoc.html\" "
                + "title=\"annotation in pkg1\">@RegContainerValNotDoc</a>"
                + "(<a href=\"RegContainerValNotDoc.html#value--\">value</a>={"
                + "<a href=\"RegContaineeDoc.html\" "
                + "title=\"annotation in pkg1\">@RegContaineeDoc</a>,"
                + "<a href=\"RegContaineeDoc.html\" "
                + "title=\"annotation in pkg1\">@RegContaineeDoc</a>},"
                + "<a href=\"RegContainerValNotDoc.html#y--\">y</a>=4)",
                "<a href=\"ContainerValNotDoc.html\" "
                + "title=\"annotation in pkg1\">@ContainerValNotDoc</a>"
                + "(<a href=\"ContainerValNotDoc.html#value--\">value</a>={"
                + "<a href=\"ContaineeNotDoc.html\" "
                + "title=\"annotation in pkg1\">@ContaineeNotDoc</a>,"
                + "<a href=\"ContaineeNotDoc.html\" "
                + "title=\"annotation in pkg1\">@ContaineeNotDoc</a>},"
                + "<a href=\"ContainerValNotDoc.html#x--\">x</a>=2)",
                "<a href=\"ContainerSynthNotDoc.html\" "
                + "title=\"annotation in pkg1\">@ContainerSynthNotDoc</a>("
                + "<a href=\"ContainerSynthNotDoc.html#value--\">value</a>="
                + "<a href=\"ContaineeSynthDoc.html\" "
                + "title=\"annotation in pkg1\">@ContaineeSynthDoc</a>)");
    }
}
