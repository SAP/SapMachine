/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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

/**
 * @test
 * @bug 8177076 8185840 8178109
 * @modules
 *     jdk.compiler/com.sun.tools.javac.api
 *     jdk.compiler/com.sun.tools.javac.main
 *     jdk.jshell/jdk.internal.jshell.tool.resources:open
 *     jdk.jshell/jdk.jshell:open
 * @library /tools/lib
 * @build toolbox.ToolBox toolbox.JarTask toolbox.JavacTask
 * @build Compiler UITesting
 * @build ToolTabCommandTest
 * @run testng ToolTabCommandTest
 */

import java.util.regex.Pattern;

import org.testng.annotations.Test;

@Test
public class ToolTabCommandTest extends UITesting {

    public void testCommand() throws Exception {
        // set terminal height so that help output won't hit page breaks
        System.setProperty("test.terminal.height", "1000000");

        doRunTest((inputSink, out) -> {
            inputSink.write("1\n");
            waitOutput(out, "\u0005");
            inputSink.write("/\011");
            waitOutput(out, ".*/edit.*/list.*\n\n" + Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n\r\u0005/");
            inputSink.write("\011");
            waitOutput(out,   ".*\n/edit\n" + Pattern.quote(getResource("help.edit.summary")) +
                            "\n.*\n/list\n" + Pattern.quote(getResource("help.list.summary")) +
                            ".*\n\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n\r\u0005/");
            inputSink.write("\011");
            waitOutput(out,  "/!\n" +
                            Pattern.quote(getResource("help.bang")) + "\n" +
                            "\n" +
                            Pattern.quote(getResource("jshell.console.see.next.command.doc")) + "\n" +
                            "\r\u0005/");
            inputSink.write("\011");
            waitOutput(out,  "/-<n>\n" +
                            Pattern.quote(getResource("help.previous")) + "\n" +
                            "\n" +
                            Pattern.quote(getResource("jshell.console.see.next.command.doc")) + "\n" +
                            "\r\u0005/");

            inputSink.write("ed\011");
            waitOutput(out, "edit $");

            inputSink.write("\011");
            waitOutput(out, ".*-all.*" +
                            "\n\n" + Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n\r\u0005/");
            inputSink.write("\011");
            waitOutput(out, Pattern.quote(getResource("help.edit.summary")) + "\n\n" +
                            Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n\r\u0005/edit ");
            inputSink.write("\011");
            waitOutput(out, Pattern.quote(getResource("help.edit").replaceAll("\t", "    ")));

            inputSink.write("\u0003/env \011");
            waitOutput(out, "\u0005/env -\n" +
                            "-add-exports    -add-modules    -class-path     -module-path    \n" +
                            "\n" +
                            Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n" +
                            "\r\u0005/env -");

            inputSink.write("\011");
            waitOutput(out, Pattern.quote(getResource("help.env.summary")) + "\n\n" +
                            Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/env -");

            inputSink.write("\011");
            waitOutput(out, Pattern.quote(getResource("help.env").replaceAll("\t", "    ")) + "\n" +
                            "\r\u0005/env -");

            inputSink.write("\011");
            waitOutput(out, "-add-exports    -add-modules    -class-path     -module-path    \n" +
                            "\n" +
                            Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n" +
                            "\r\u0005/env -");

            inputSink.write("\u0003/exit \011");
            waitOutput(out, Pattern.quote(getResource("help.exit.summary")) + "\n\n" +
                            Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n\r\u0005/exit ");
            inputSink.write("\011");
            waitOutput(out, Pattern.quote(getResource("help.exit").replaceAll("\t", "    ")) + "\n" +
                            "\r\u0005/exit ");
            inputSink.write("\011");
            waitOutput(out, Pattern.quote(getResource("help.exit.summary")) + "\n\n" +
                            Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n\r\u0005/exit ");
            inputSink.write("\u0003");
            inputSink.write("int zebraStripes = 11\n");
            waitOutput(out, "zebraStripes ==> 11\n\u0005");
            inputSink.write("/exit zeb\011");
            waitOutput(out, "braStr.*es");
            inputSink.write("\u0003/doesnotexist\011");
            waitOutput(out, "\u0005/doesnotexist\n" +
                            Pattern.quote(getResource("jshell.console.no.such.command")) + "\n" +
                            "\n" +
                            "\r\u0005/doesnotexist");
        });
    }

    public void testHelp() throws Exception {
        // set terminal height so that help output won't hit page breaks
        System.setProperty("test.terminal.height", "1000000");

        doRunTest((inputSink, out) -> {
            inputSink.write("/help \011");
            waitOutput(out, ".*/edit.*/list.*intro.*\n\n" + Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n" +
                            "\r\u0005/");
            inputSink.write("\011");
            waitOutput(out,   ".*\n/edit\n" + Pattern.quote(getResource("help.edit.summary")) +
                            "\n.*\n/list\n" + Pattern.quote(getResource("help.list.summary")) +
                            "\n.*\nintro\n" + Pattern.quote(getResource("help.intro.summary")) +
                            ".*\n\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/");
            inputSink.write("/env\011");
            waitOutput(out,   "help /env ");
            inputSink.write("\011");
            waitOutput(out,   ".*\n/env\n" + Pattern.quote(getResource("help.env.summary")) +
                            ".*\n\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/help /env ");
            inputSink.write("\011");
            waitOutput(out,   ".*\n/env\n" + Pattern.quote(getResource("help.env").replaceAll("\t", "    ")) + "\n" +
                            "\r\u0005/help /env ");
            inputSink.write("\u0003/help intro\011");
            waitOutput(out,   "help intro ");
            inputSink.write("\011");
            waitOutput(out,   ".*\nintro\n" + Pattern.quote(getResource("help.intro.summary")) +
                            ".*\n\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/help intro ");
            inputSink.write("\011");
            waitOutput(out,   ".*\nintro\n" + Pattern.quote(getResource("help.intro").replaceAll("\t", "    ")) + "\n" +
                            "\r\u0005/help intro ");
            inputSink.write("\u0003/help /set \011");
            waitOutput(out, ".*format.*truncation.*\n\n" + Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n" +
                            "\r\u0005/help /set ");
            inputSink.write("\011");
            waitOutput(out,   ".*\n/set format\n" + Pattern.quote(getResource("help.set.format.summary")) +
                            "\n.*\n/set truncation\n" + Pattern.quote(getResource("help.set.truncation.summary")) +
                            ".*\n\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/help /set ");
            inputSink.write("truncation\011");
            waitOutput(out,   ".*truncation\n" + Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n" +
                            "\r\u0005/help /set truncation");
            inputSink.write("\011");
            waitOutput(out,   ".*/set truncation\n" + Pattern.quote(getResource("help.set.truncation.summary")) + "\n" +
                            "\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/help /set truncation");
            inputSink.write("\011");
            waitOutput(out,   ".*/set truncation\n" + Pattern.quote(getResource("help.set.truncation").replaceAll("\t", "    ")) +
                            "\r\u0005/help /set truncation");
            inputSink.write("\u0003/help env \011");
            waitOutput(out,   ".*\n/env\n" + Pattern.quote(getResource("help.env.summary")) +
                            ".*\n\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/help env ");
            inputSink.write("\u0003/help set truncation\011");
            waitOutput(out,   ".*truncation\n" + Pattern.quote(getResource("jshell.console.see.synopsis")) + "\n" +
                            "\r\u0005/help set truncation");
            inputSink.write("\011");
            waitOutput(out,   ".*\n/set truncation\n" + Pattern.quote(getResource("help.set.truncation.summary")) +
                            ".*\n\n" + Pattern.quote(getResource("jshell.console.see.full.documentation")) + "\n" +
                            "\r\u0005/help set truncation");
        });
    }
}
