/*
 * Copyright (c) 2018, 2024, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2018, 2024 SAP SE. All rights reserved.
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
 * @requires os.arch != "aarch64" | os.family != "windows"
 * @summary Test basic functionality of the file socket debug transport.
 *
 * @author Ralf Schmelter
 *
 * @library /test/lib libs/com.sap.jvm.jdkext-8-patch-20181214.065356-2.jar
 * @run compile FileSocketTransportTest.java
 * @run main/othervm FileSocketTransportTest
 */

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertTrue;
import static jdk.test.lib.Asserts.assertFalse;

import java.io.File;
import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SocketChannel;
import java.util.ArrayList;
import java.util.List;

import com.sap.jdk.ext.filesocket.FileSocket;
import com.sap.jdk.ext.filesocket.FileSocketAddress;

import jdk.test.lib.Platform;
import jdk.test.lib.Utils;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class FileSocketTransportTest {

    private static int handshake(String path, byte[] handshake, byte[] reply) throws IOException {
        UnixDomainSocketAddress addr = UnixDomainSocketAddress.of(path);
        SocketChannel channel = SocketChannel.open(StandardProtocolFamily.UNIX);
        channel.connect(addr);
        ByteBuffer out = ByteBuffer.wrap(handshake);
        while (out.hasRemaining()) {
            channel.write(out);
        }
        ByteBuffer in = ByteBuffer.wrap(reply);
        int result = channel.read(in);
        channel.close();

        return result;
    }

    public static void main(String[] args) throws Throwable {
        if (args.length == 1 && "--sleep".equals(args[0])) {
            Thread.sleep(30000);
            System.exit(1);
        }

        String socketName = "test.socket";

        List<String> opts = new ArrayList<>();
        // opts.addAll(Utils.getVmOptions());
        opts.add("-agentlib:jdwp=transport=dt_filesocket,address=" +
                 socketName + ",server=y,suspend=n");
        opts.add(FileSocketTransportTest.class.getName());
        opts.add("--sleep");

        // First check if we get the expected errors.
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
                opts.toArray(new String[0]));
        Process proc = pb.start();
        new Thread(() -> {
            try {
                OutputAnalyzer output = new OutputAnalyzer(proc);
                System.out.println(output.getOutput());
            } catch (IOException e) {
                e.printStackTrace();
            }
        }).start();

        // Debug 3 times.
        try {
            byte[] handshake = "JDWP-Handshake".getBytes("UTF-8");
            byte[] received = new byte[handshake.length];
            int read;

            for (int i = 0; i < 3; ++i) {
                // Wait a bit to let the debugging be set up properly.
                Thread.sleep(3000);
                checkSocketPresent(socketName);
                read = handshake(socketName, handshake, received);
                checkSocketDeleted(socketName);
                assertEquals(new String(handshake, "UTF-8"),
                             new String(received, "UTF-8"));
                assertEquals(read, received.length);
            }
        } finally {
            Thread.sleep(2000);
            checkSocketPresent(socketName);
            proc.destroy();
            Thread.sleep(2000);
            checkSocketDeleted(socketName);
        }
    }

    private static void checkSocketPresent(String name) {
        if (!Platform.isWindows()) {
            assertTrue(new File(name).exists(), "Socket " + name + " missing");
        }
    }

    private static void checkSocketDeleted(String name) {
        if (!Platform.isWindows()) {
            assertFalse(new File(name).exists(), "Socket " + name + " exists");
        }
    }
}
