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

import javax.net.ServerSocketFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLSocket;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import jdk.incubator.http.HttpClient;
import jdk.incubator.http.HttpClient.Version;
import jdk.incubator.http.HttpResponse;
import jdk.incubator.http.HttpRequest;
import static java.lang.System.out;
import static jdk.incubator.http.HttpResponse.BodyHandler.discard;

/**
 * @test
 * @run main/othervm HandshakeFailureTest
 * @summary Verify SSLHandshakeException is received when the handshake fails,
 * either because the server closes ( EOF ) the connection during handshaking
 * or no cipher suite ( or similar ) can be negotiated.
 */
// To switch on debugging use:
// @run main/othervm -Djdk.internal.httpclient.debug=true HandshakeFailureTest
public class HandshakeFailureTest {

    // The number of iterations each testXXXClient performs. Can be increased
    // when running standalone testing.
    static final int TIMES = 10;

    public static void main(String[] args) throws Exception {
        HandshakeFailureTest test = new HandshakeFailureTest();
        List<AbstractServer> servers = List.of( new PlainServer(), new SSLServer());

        for (AbstractServer server : servers) {
            try (server) {
                out.format("%n%n------ Testing with server:%s ------%n", server);
                URI uri = new URI("https://127.0.0.1:" + server.getPort() + "/");

                test.testSyncSameClient(uri, Version.HTTP_1_1);
                test.testSyncSameClient(uri, Version.HTTP_2);
                test.testSyncDiffClient(uri, Version.HTTP_1_1);
                test.testSyncDiffClient(uri, Version.HTTP_2);

                test.testAsyncSameClient(uri, Version.HTTP_1_1);
                test.testAsyncSameClient(uri, Version.HTTP_2);
                test.testAsyncDiffClient(uri, Version.HTTP_1_1);
                test.testAsyncDiffClient(uri, Version.HTTP_2);
            }
        }
    }

    void testSyncSameClient(URI uri, Version version) throws Exception {
        out.printf("%n--- testSyncSameClient %s ---%n", version);
        HttpClient client = HttpClient.newHttpClient();
        for (int i = 0; i < TIMES; i++) {
            out.printf("iteration %d%n", i);
            HttpRequest request = HttpRequest.newBuilder(uri)
                                             .version(version)
                                             .build();
            try {
                HttpResponse<Void> response = client.send(request, discard(null));
                String msg = String.format("UNEXPECTED response=%s%n", response);
                throw new RuntimeException(msg);
            } catch (SSLHandshakeException expected) {
                out.printf("Client: caught expected exception: %s%n", expected);
            }
        }
    }

    void testSyncDiffClient(URI uri, Version version) throws Exception {
        out.printf("%n--- testSyncDiffClient %s ---%n", version);
        for (int i = 0; i < TIMES; i++) {
            out.printf("iteration %d%n", i);
            // a new client each time
            HttpClient client = HttpClient.newHttpClient();
            HttpRequest request = HttpRequest.newBuilder(uri)
                                             .version(version)
                                             .build();
            try {
                HttpResponse<Void> response = client.send(request, discard(null));
                String msg = String.format("UNEXPECTED response=%s%n", response);
                throw new RuntimeException(msg);
            } catch (SSLHandshakeException expected) {
                out.printf("Client: caught expected exception: %s%n", expected);
            }
        }
    }

    void testAsyncSameClient(URI uri, Version version) throws Exception {
        out.printf("%n--- testAsyncSameClient %s ---%n", version);
        HttpClient client = HttpClient.newHttpClient();
        for (int i = 0; i < TIMES; i++) {
            out.printf("iteration %d%n", i);
            HttpRequest request = HttpRequest.newBuilder(uri)
                                             .version(version)
                                             .build();
            CompletableFuture<HttpResponse<Void>> response =
                        client.sendAsync(request, discard(null));
            try {
                response.join();
                String msg = String.format("UNEXPECTED response=%s%n", response);
                throw new RuntimeException(msg);
            } catch (CompletionException ce) {
                if (ce.getCause() instanceof SSLHandshakeException) {
                    out.printf("Client: caught expected exception: %s%n", ce.getCause());
                } else {
                    out.printf("Client: caught UNEXPECTED exception: %s%n", ce.getCause());
                    throw ce;
                }
            }
        }
    }

    void testAsyncDiffClient(URI uri, Version version) throws Exception {
        out.printf("%n--- testAsyncDiffClient %s ---%n", version);
        for (int i = 0; i < TIMES; i++) {
            out.printf("iteration %d%n", i);
            // a new client each time
            HttpClient client = HttpClient.newHttpClient();
            HttpRequest request = HttpRequest.newBuilder(uri)
                                             .version(version)
                                             .build();
            CompletableFuture<HttpResponse<Void>> response =
                    client.sendAsync(request, discard(null));
            try {
                response.join();
                String msg = String.format("UNEXPECTED response=%s%n", response);
                throw new RuntimeException(msg);
            } catch (CompletionException ce) {
                if (ce.getCause() instanceof SSLHandshakeException) {
                    out.printf("Client: caught expected exception: %s%n", ce.getCause());
                } else {
                    out.printf("Client: caught UNEXPECTED exception: %s%n", ce.getCause());
                    throw ce;
                }
            }
        }
    }

    /** Common supertype for PlainServer and SSLServer. */
    static abstract class AbstractServer extends Thread implements AutoCloseable {
        protected final ServerSocket ss;
        protected volatile boolean closed;

        AbstractServer(String name, ServerSocket ss) throws IOException {
            super(name);
            this.ss = ss;
            this.start();
        }

        int getPort() { return ss.getLocalPort(); }

        @Override
        public void close() {
            if (closed)
                return;
            closed = true;
            try {
                ss.close();
            } catch (IOException e) {
                throw new UncheckedIOException("Unexpected", e);
            }
        }
    }

    /** Emulates a server-side, using plain cleartext Sockets, that just closes
     * the connection, after a small variable delay. */
    static class PlainServer extends AbstractServer {
        private volatile int count;

        PlainServer() throws IOException {
            super("PlainServer", new ServerSocket(0));
        }

        @Override
        public void run() {
            while (!closed) {
                try (Socket s = ss.accept()) {
                    count++;

                    /*   SSL record layer - contains the client hello
                    struct {
                        uint8 major, minor;
                    } ProtocolVersion;

                    enum {
                        change_cipher_spec(20), alert(21), handshake(22),
                        application_data(23), (255)
                    } ContentType;

                    struct {
                        ContentType type;
                        ProtocolVersion version;
                        uint16 length;
                        opaque fragment[SSLPlaintext.length];
                    } SSLPlaintext;   */
                    DataInputStream din =  new DataInputStream(s.getInputStream());
                    int contentType = din.read();
                    out.println("ContentType:" + contentType);
                    int majorVersion = din.read();
                    out.println("Major:" + majorVersion);
                    int minorVersion = din.read();
                    out.println("Minor:" + minorVersion);
                    int length = din.readShort();
                    out.println("length:" + length);
                    byte[] ba = new byte[length];
                    din.readFully(ba);

                    // simulate various delays in response
                    Thread.sleep(10 * (count % 10));
                    s.close(); // close without giving any reply
                } catch (IOException e) {
                    if (!closed)
                        out.println("Unexpected" + e);
                } catch (InterruptedException e) {
                    throw new RuntimeException(e);
                }
            }
        }
    }

    /** Emulates a server-side, using SSL Sockets, that will fail during
     * handshaking, as there are no cipher suites in common. */
    static class SSLServer extends AbstractServer {
        static final SSLContext sslContext = createUntrustingContext();
        static final ServerSocketFactory factory = sslContext.getServerSocketFactory();

        static SSLContext createUntrustingContext() {
            try {
                SSLContext sslContext = SSLContext.getInstance("TLSv1.2");
                sslContext.init(null, null, null);
                return sslContext;
            } catch (Throwable t) {
                throw new AssertionError(t);
            }
        }

        SSLServer() throws IOException {
            super("SSLServer", factory.createServerSocket(0));
        }

        @Override
        public void run() {
            while (!closed) {
                try (SSLSocket s = (SSLSocket)ss.accept()) {
                    s.getInputStream().read();  // will throw SHE here

                    throw new AssertionError("Should not reach here");
                } catch (SSLHandshakeException expected) {
                    // Expected: SSLHandshakeException: no cipher suites in common
                    out.printf("Server: caught expected exception: %s%n", expected);
                } catch (IOException e) {
                    if (!closed)
                        out.printf("UNEXPECTED %s", e);
                }
            }
        }
    }
}
