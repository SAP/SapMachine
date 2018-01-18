/*
 * Copyright (c) 2016, 2018, Oracle and/or its affiliates. All rights reserved.
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
 * @bug  8157349 8185985 8194953
 * @summary  test copy of doc-files, and its contents for HTML meta content.
 * @library  ../lib
 * @modules jdk.javadoc/jdk.javadoc.internal.tool
 * @build    JavadocTester
 * @run main TestCopyFiles
 */

public class TestCopyFiles extends JavadocTester {

    public static void main(String... args) throws Exception {
        TestCopyFiles tester = new TestCopyFiles();
        tester.runTests();
    }

    @Test
    void testDocFilesInModulePackages() {
        javadoc("-d", "modules-out",
                "-top", "phi-TOP-phi",
                "-bottom", "phi-BOTTOM-phi",
                "-header", "phi-HEADER-phi",
                "-footer", "phi-FOOTER-phi",
                "-windowtitle", "phi-WINDOW-TITLE-phi",
                "--module-source-path", testSrc("modules"),
                "--module", "acme.mdle");
        checkExit(Exit.OK);
        checkOrder("p/doc-files/inpackage.html",
                "\"Hello World\" (phi-WINDOW-TITLE-phi)",
                "phi-TOP-phi",
                // check top navbar
                "<a href=\"../../acme.mdle-summary.html\">Module</a>",
                "<a href=\"../../p/package-summary.html\">Package</a>",
                "<a href=\"../../overview-tree.html\">Tree</a>",
                "<a href=\"../../deprecated-list.html\">Deprecated</a>",
                "<a href=\"../../index-all.html\">Index</a>",
                "phi-HEADER-phi",
                "In a named module acme.module and named package "
                        + "<a href=\"../../p/package-summary.html\"><code>p</code></a>.",
                "\"simpleTagLabel\">Since:</",
                "1940",
                // check bottom navbar
                "<a href=\"../../acme.mdle-summary.html\">Module</a>",
                "<a href=\"../../p/package-summary.html\">Package</a>",
                "<a href=\"../../overview-tree.html\">Tree</a>",
                "<a href=\"../../deprecated-list.html\">Deprecated</a>",
                "<a href=\"../../index-all.html\">Index</a>",
                "phi-FOOTER-phi",
                "phi-BOTTOM-phi"
        );
    }

    @Test
    void testDocFilesInMultiModulePackagesWithRecursiveCopy() {
        javadoc("-d", "multi-modules-out-recursive",
                "-docfilessubdirs",
                "-top", "phi-TOP-phi",
                "-bottom", "phi-BOTTOM-phi",
                "-header", "phi-HEADER-phi",
                "-footer", "phi-FOOTER-phi",
                "-windowtitle", "phi-WINDOW-TITLE-phi",
                "--module-source-path", testSrc("modules"),
                "--module", "acme.mdle,acme2.mdle");
        checkExit(Exit.OK);
        checkOrder("p/doc-files/inpackage.html",
                "\"Hello World\" (phi-WINDOW-TITLE-phi)",
                "phi-TOP-phi",
                // check top navbar
                "<a href=\"../../acme.mdle-summary.html\">Module</a>",
                "<a href=\"../../p/package-summary.html\">Package</a>",
                "<a href=\"../../overview-tree.html\">Tree</a>",
                "<a href=\"../../deprecated-list.html\">Deprecated</a>",
                "<a href=\"../../index-all.html\">Index</a>",
                "phi-HEADER-phi",
                "In a named module acme.module and named package "
                        + "<a href=\"../../p/package-summary.html\"><code>p</code></a>.",
                "\"simpleTagLabel\">Since:</",
                "1940",
                // check bottom navbar
                "<a href=\"../../acme.mdle-summary.html\">Module</a>",
                "<a href=\"../../p/package-summary.html\">Package</a>",
                "<a href=\"../../overview-tree.html\">Tree</a>",
                "<a href=\"../../deprecated-list.html\">Deprecated</a>",
                "<a href=\"../../index-all.html\">Index</a>",
                "phi-FOOTER-phi",
                "phi-BOTTOM-phi"
        );

        // check the bottom most doc file
        checkOrder("p2/doc-files/sub-dir/sub-dir-1/SubSubReadme.html",
                "SubSubReadme (phi-WINDOW-TITLE-phi)",
                "phi-TOP-phi",
                // check top navbar
                "<a href=\"../../../../acme2.mdle-summary.html\">Module</a>",
                "<a href=\"../../../../p2/package-summary.html\">Package</a>",
                "<a href=\"../../../../overview-tree.html\">Tree</a>",
                "<a href=\"../../../../deprecated-list.html\">Deprecated</a>",
                "<a href=\"../../../../index-all.html\">Index</a>",
                "phi-HEADER-phi",
                "SubSubReadme.html at third level of doc-file directory.",
                // check bottom navbar
                "<a href=\"../../../../acme2.mdle-summary.html\">Module</a>",
                "<a href=\"../../../../p2/package-summary.html\">Package</a>",
                "<a href=\"../../../../overview-tree.html\">Tree</a>",
                "<a href=\"../../../../deprecated-list.html\">Deprecated</a>",
                "<a href=\"../../../../index-all.html\">Index</a>",
                "phi-FOOTER-phi",
                "phi-BOTTOM-phi"
        );
    }
    @Test
    void testDocFilesInModulePackagesWithRecursiveCopy() {
        javadoc("-d", "modules-out-recursive",
                "-docfilessubdirs",
                "--module-source-path", testSrc("modules"),
                "--module", "acme.mdle");
        checkExit(Exit.OK);
        checkOutput("p/doc-files/inpackage.html", true,
                "In a named module acme.module and named package "
                + "<a href=\"../../p/package-summary.html\"><code>p</code></a>."
        );
    }

    @Test
    void testDocFilesInModulePackagesWithRecursiveCopyWithExclusion() {
        javadoc("-d", "modules-out-recursive-with-exclusion",
                "-docfilessubdirs",
                "-excludedocfilessubdir", "sub-dir",
                "--module-source-path", testSrc("modules"),
                "--module", "acme.mdle");
        checkExit(Exit.OK);
        checkOutput("p/doc-files/inpackage.html", true,
                "In a named module acme.module and named package "
                + "<a href=\"../../p/package-summary.html\"><code>p</code></a>."
        );
    }

    @Test
    void testDocFilesInPackages() {
        javadoc("-d", "packages-out",
                "-sourcepath", testSrc("packages"),
                "p1");
        checkExit(Exit.OK);
        checkOutput("p1/doc-files/inpackage.html", true,
                "A named package in an unnamed module"
        );
    }

    @Test
    void testDocFilesInPackagesWithRecursiveCopy() {
        javadoc("-d", "packages-out-recursive",
                "-docfilessubdirs",
                "-sourcepath", testSrc("packages"),
                "p1");
        checkExit(Exit.OK);

        checkOutput("p1/doc-files/inpackage.html", true,
                "A named package in an unnamed module"
        );

        checkOutput("p1/doc-files/sub-dir/SubReadme.html", true,
                "<title>SubReadme</title>",
                "SubReadme.html at second level of doc-file directory."
        );
    }

    @Test
    void testDocFilesInPackagesWithRecursiveCopyWithExclusion() {
        javadoc("-d", "packages-out-recursive-with-exclusion",
                "-docfilessubdirs",
                "-excludedocfilessubdir", "sub-dir",
                "-sourcepath", testSrc("packages"),
                "p1");
        checkExit(Exit.OK);

        checkOutput("p1/doc-files/inpackage.html", true,
                "A named package in an unnamed module"
        );
    }

    @Test
    void testDocFilesInUnnamedPackages() {
        javadoc("-d", "unnamed-out",
                "-windowtitle", "phi-WINDOW-TITLE-phi",
                "-sourcepath", testSrc("unnamed"),
                testSrc("unnamed/Foo.java")
        );
        checkExit(Exit.OK);
        checkOutput("doc-files/inpackage.html", true,
                "<title>(phi-WINDOW-TITLE-phi)</title>\n",
                "In an unnamed package"
        );
    }

    @Test
    void testDocFilesInUnnamedPackagesWithRecursiveCopy() {
        javadoc("-d", "unnamed-out-recursive",
                "-docfilessubdirs",
                "-windowtitle", "phi-WINDOW-TITLE-phi",
                "-sourcepath", testSrc("unnamed"),
                testSrc("unnamed/Foo.java")
        );
        checkExit(Exit.OK);
        checkOutput("doc-files/inpackage.html", true,
                "<title>(phi-WINDOW-TITLE-phi)</title>\n",
                "In an unnamed package"
        );
        checkOutput("doc-files/doc-file/SubReadme.html", true,
                "<title>Beep Beep (phi-WINDOW-TITLE-phi)</title>\n",
                "SubReadme.html at second level of doc-file directory for unnamed package."
        );
    }

    @Test
    void testDocFilesInPackagesSource7() {
        javadoc("-d", "packages-out-src7",
                "-source", "7",
                "-sourcepath", testSrc("packages"),
                "p1");
        checkExit(Exit.OK);
        checkOutput("p1/doc-files/inpackage.html", true,
                "A named package in an unnamed module"
        );
    }

    @Test
    void testDocFilesInPackagesSource7UsingClassPath() {
        javadoc("-d", "packages-out-src7-cp",
                "-source", "7",
                "-classpath", testSrc("packages"),
                "p1");
        checkExit(Exit.OK);
        checkOutput("p1/doc-files/inpackage.html", true,
                "A named package in an unnamed module"
        );
    }

    @Test
    void testCopyThrough() {
        javadoc("-d", "copy",
                "-sourcepath", testSrc("packages"),
                "p2");
        checkExit(Exit.OK);
        checkOutput("p2/doc-files/case1.html", true, "<!-- Generated by javadoc");
        checkOutput("p2/doc-files/case2.html", false, "<!-- Generated by javadoc");
        checkOutput("p2/doc-files/case3.html", false, "<!-- Generated by javadoc");
        checkOutput("p2/doc-files/case4.html", false, "<!-- Generated by javadoc");
    }
}
