/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
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

import java.io.IOException;
import java.net.URI;
import java.util.concurrent.CompletableFuture;
import javax.net.ssl.SSLContext;
import javax.net.ServerSocketFactory;
import javax.net.ssl.SSLServerSocketFactory;
import jdk.incubator.http.HttpClient;
import jdk.incubator.http.HttpClient.Version;
import jdk.incubator.http.HttpRequest;
import jdk.incubator.http.HttpResponse;
import jdk.testlibrary.SimpleSSLContext;
import static java.lang.System.out;
import static java.lang.String.format;
import static jdk.incubator.http.HttpResponse.BodyHandler.asString;

/**
 * @test
 * @bug 8087112
 * @library /lib/testlibrary
 * @build jdk.testlibrary.SimpleSSLContext
 * @build MockServer
 * @run main/othervm -Djdk.internal.httpclient.debug=true -Djdk.httpclient.HttpClient.log=all SplitResponse
 */

/**
 * Similar test to QuickResponses except that each byte of the response
 * is sent in a separate packet, which tests the stability of the implementation
 * for receiving unusual packet sizes. Additionally, tests scenarios there
 * connections that are retrieved from the connection pool may reach EOF before
 * being reused.
 */
public class SplitResponse {

    static String response(String body, boolean serverKeepalive) {
        StringBuilder sb = new StringBuilder();
        sb.append("HTTP/1.1 200 OK\r\n");
        if (!serverKeepalive)
            sb.append("Connection: Close\r\n");

        sb.append("Content-length: ").append(body.length()).append("\r\n");
        sb.append("\r\n");
        sb.append(body);
        return sb.toString();
    }

    static final String responses[] = {
        "Lorem ipsum",
        "dolor sit amet",
        "consectetur adipiscing elit, sed do eiusmod tempor",
        "quis nostrud exercitation ullamco",
        "laboris nisi",
        "ut",
        "aliquip ex ea commodo consequat." +
        "Duis aute irure dolor in reprehenderit in voluptate velit esse" +
        "cillum dolore eu fugiat nulla pariatur.",
        "Excepteur sint occaecat cupidatat non proident."
    };

    final ServerSocketFactory factory;
    final SSLContext context;
    final boolean useSSL;
    SplitResponse(boolean useSSL) throws IOException {
        this.useSSL = useSSL;
        context = new SimpleSSLContext().get();
        SSLContext.setDefault(context);
        factory = useSSL ? SSLServerSocketFactory.getDefault()
                         : ServerSocketFactory.getDefault();
    }

    public HttpClient newHttpClient() {
        HttpClient client;
        if (useSSL) {
            client = HttpClient.newBuilder()
                               .sslContext(context)
                               .build();
        } else {
            client = HttpClient.newHttpClient();
        }
        return client;
    }

    public static void main(String[] args) throws Exception {
        boolean useSSL = false;
        if (args != null && args.length == 1) {
            useSSL = "SSL".equals(args[0]);
        }
        SplitResponse sp = new SplitResponse(useSSL);

        for (Version version : Version.values()) {
            for (boolean serverKeepalive : new boolean[]{ true, false }) {
                // Note: the mock server doesn't support Keep-Alive, but
                // pretending that it might exercises code paths in and out of
                // the connection pool, and retry logic
                for (boolean async : new boolean[]{ true, false }) {
                    sp.test(version, serverKeepalive, async);
                }
            }
        }
    }

    // @Test
    void test(Version version, boolean serverKeepalive, boolean async)
        throws Exception
    {
        out.println(format("*** version %s, serverKeepAlive: %s, async: %s ***",
                           version, serverKeepalive, async));
        MockServer server = new MockServer(0, factory);
        URI uri = new URI(server.getURL());
        out.println("server is: " + uri);
        server.start();


        // The following code can be uncommented to verify that the
        // MockServer will reject rogue requests whose URI does not
        // contain "/foo/".
        //
        //        Thread rogue = new Thread() {
        //            public void run() {
        //                try {
        //                    HttpClient client = newHttpClient();
        //                    URI uri2 = URI.create(uri.toString().replace("/foo/","/"));
        //                    HttpRequest request = HttpRequest
        //                        .newBuilder(uri2).version(version).build();
        //                    while (true) {
        //                        try {
        //                            client.send(request, HttpResponse.BodyHandler.asString());
        //                        } catch (IOException ex) {
        //                            System.out.println("Client rejected " + request);
        //                        }
        //                        sleep(250);
        //                    }
        //                } catch ( Throwable x) {
        //                }
        //            }
        //        };
        //        rogue.setDaemon(true);
        //        rogue.start();


        HttpClient client = newHttpClient();
        HttpRequest request = HttpRequest.newBuilder(uri).version(version).build();
        HttpResponse<String> r;
        CompletableFuture<HttpResponse<String>> cf1;

        try {
            for (int i=0; i<responses.length; i++) {
                out.println("----- iteration " + i + " -----");
                String body = responses[i];
                Thread t = sendSplitResponse(response(body, serverKeepalive), server);

                if (async) {
                    out.println("send async: " + request);
                    cf1 = client.sendAsync(request, asString());
                    r = cf1.get();
                } else { // sync
                    out.println("send sync: " + request);
                    r = client.send(request, asString());
                }

                if (r.statusCode() != 200)
                    throw new RuntimeException("Failed");

                String rxbody = r.body();
                out.println("received " + rxbody);
                if (!rxbody.equals(body))
                    throw new RuntimeException(format("Expected:%s, got:%s", body, rxbody));

                t.join();
                conn.close();
            }
        } finally {
            server.close();
        }
        System.out.println("OK");
    }

    // required for cleanup
    volatile MockServer.Connection conn;

    // Sends the response, mostly, one byte at a time with a small delay
    // between bytes, to encourage that each byte is read in a separate read
    Thread sendSplitResponse(String s, MockServer server) {
        System.out.println("Sending: ");
        Thread t = new Thread(() -> {
            System.out.println("Waiting for server to receive headers");
            conn = server.activity();
            System.out.println("Start sending response");

            try {
                int len = s.length();
                out.println("sending " + s);
                for (int i = 0; i < len; i++) {
                    String onechar = s.substring(i, i + 1);
                    conn.send(onechar);
                    Thread.sleep(10);
                }
                out.println("sent " + s);
            } catch (IOException | InterruptedException e) {
                throw new RuntimeException(e);
            }
        });
        t.setDaemon(true);
        t.start();
        return t;
    }
}
