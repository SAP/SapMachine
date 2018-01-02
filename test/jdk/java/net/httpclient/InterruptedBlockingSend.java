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

import java.net.ServerSocket;
import java.net.URI;
import jdk.incubator.http.HttpClient;
import jdk.incubator.http.HttpRequest;
import static java.lang.System.out;
import static jdk.incubator.http.HttpResponse.BodyHandler.discard;

/**
 * @test
 * @summary Basic test for interrupted blocking send
 */

public class InterruptedBlockingSend {

    static volatile Throwable throwable;

    public static void main(String[] args) throws Exception {
        HttpClient client = HttpClient.newHttpClient();
        try (ServerSocket ss = new ServerSocket(0, 20)) {
            int port = ss.getLocalPort();
            URI uri = new URI("http://127.0.0.1:" + port + "/");

            HttpRequest request = HttpRequest.newBuilder(uri).build();

            Thread t = new Thread(() -> {
                try {
                    client.send(request, discard(null));
                } catch (InterruptedException e) {
                    throwable = e;
                } catch (Throwable th) {
                    throwable = th;
                }
            });
            t.start();
            Thread.sleep(5000);
            t.interrupt();
            t.join();

            if (!(throwable instanceof InterruptedException)) {
                throw new RuntimeException("Expected InterruptedException, got " + throwable);
            } else {
                out.println("Caught expected InterruptedException: " + throwable);
            }
        }
    }
}
