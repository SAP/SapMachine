/*
 * Copyright (c) 2004, 2018, Oracle and/or its affiliates. All rights reserved.
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
 * @bug      4973609 8015249 8025633 8026567 6469561 8071982 8162363
 * @summary  Make sure that annotation types with 0 members does not have
 *           extra HR tags.
 * @author   jamieh
 * @library  ../lib
 * @modules jdk.javadoc/jdk.javadoc.internal.tool
 * @build    JavadocTester
 * @run main TestAnnotationTypes
 */

public class TestAnnotationTypes extends JavadocTester {

    public static void main(String... args) throws Exception {
        TestAnnotationTypes tester = new TestAnnotationTypes();
        tester.runTests();
    }

    @Test
    void test() {
        javadoc("-d", "out-1",
                "-sourcepath", testSrc,
                "pkg");
        checkExit(Exit.OK);

        checkOutput("pkg/AnnotationTypeField.html", true,
                "<li>Summary:&nbsp;</li>\n"
                + "<li><a href=\"#annotation.type."
                + "field.summary\">Field</a>&nbsp;|&nbsp;</li>",
                "<li>Detail:&nbsp;</li>\n"
                + "<li><a href=\"#annotation.type."
                + "field.detail\">Field</a>&nbsp;|&nbsp;</li>",
                "<!-- =========== ANNOTATION TYPE FIELD SUMMARY =========== -->",
                "<h3>Field Summary</h3>",
                "<th class=\"colSecond\" scope=\"row\"><code><span class=\"memberNameLink\"><a href=\"#DEFAULT_NAME\">DEFAULT_NAME</a></span>"
                + "</code></th>",
                "<!-- ============ ANNOTATION TYPE FIELD DETAIL =========== -->",
                "<h4>DEFAULT_NAME</h4>\n"
                + "<pre>static final&nbsp;java."
                + "lang.String&nbsp;DEFAULT_NAME</pre>");

        checkOutput("pkg/AnnotationType.html", true,
                "<li>Summary:&nbsp;</li>\n"
                + "<li>Field&nbsp;|&nbsp;</li>",
                "<li>Detail:&nbsp;</li>\n"
                + "<li>Field&nbsp;|&nbsp;</li>");

        checkOutput("pkg/AnnotationType.html", true,
                    "<!-- ============ ANNOTATION TYPE MEMBER DETAIL =========== -->",
                    "<ul class=\"blockList\">",
                    "<li class=\"blockList\"><a name=\"annotation.type.element.detail\">",
                    "<!--   -->",
                    "</a>",
                    "<h3>Element Detail</h3>",
                    "<a name=\"value--\">",
                    "<!--   -->",
                    "</a>",
                    "<ul class=\"blockListLast\">",
                    "<li class=\"blockList\">",
                    "<h4>value</h4>",
                    "<pre>int&nbsp;value</pre>" );

        checkOutput("pkg/AnnotationType.html", false,
                "<HR>\n\n"
                + "<P>\n\n"
                + "<P>"
                + "<!-- ========= END OF CLASS DATA ========= -->" + "<HR>");

        javadoc("-d", "out-2",
                "-linksource",
                "-sourcepath", testSrc,
                "pkg");
        checkExit(Exit.OK);

        checkOutput("src-html/pkg/AnnotationType.html", true,
                "<title>Source code</title>",
                "@Documented public @interface AnnotationType {");

        checkOutput("src-html/pkg/AnnotationTypeField.html", true,
                "<title>Source code</title>",
                "@Documented public @interface AnnotationTypeField {");

        checkOutput("pkg/AnnotationType.html", true,
                "public @interface <a href=\"../src-html/pkg/AnnotationType.html#line.34"
                + "\">AnnotationType</a></pre>");

        checkOutput("pkg/AnnotationTypeField.html", true,
                "public @interface <a href=\"../src-html/pkg/AnnotationTypeField.html#line.31"
                + "\">AnnotationTypeField</a></pre>");
    }
}
