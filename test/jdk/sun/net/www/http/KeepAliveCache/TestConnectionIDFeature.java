/*
 * Copyright (c) 2024 SAP SE. All rights reserved.
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
 * @summary Test the SapMachine specific KeepAliveCache connectionID feature
 * @library /test/lib
 * @modules java.base/sun.net.www.http
 * @run main/othervm -Dcom.sap.jvm.UseHttpKeepAliveCacheKeyExtension=true TestConnectionIDFeature
 */

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.URL;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Supplier;

import jdk.test.lib.net.URIBuilder;

import sun.net.www.http.KeepAliveCache;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import com.sun.net.httpserver.HttpServer;

/*
 * At first, the client opens 5 connections which get cached.
 * Then in a second round of requests each thread should use the connection which
 * is requested through the value of the KeepAliveCache.connectionID field.
 */
public class TestConnectionIDFeature {
    static final byte[] PAYLOAD = "hello".getBytes();
    static final int CLIENT_THREADS = 6;

    static final ExecutorService serverExecutor = Executors.newSingleThreadExecutor();
    static final ExecutorService executor = Executors.newFixedThreadPool(CLIENT_THREADS);
    static HttpServer server;

    static ArrayDeque<String> connectionIds = new ArrayDeque<>();
    static Map<String, Integer> clientPorts = new ConcurrentHashMap<>();
    static Map<String, String> clientAsserts = new ConcurrentHashMap<>();
    static CountDownLatch clientSync = new CountDownLatch(CLIENT_THREADS);
    static List<CompletableFuture<String>> clientFutures = new ArrayList<>(CLIENT_THREADS);

    static class TestHttpHandler implements HttpHandler {
        public void handle(HttpExchange trans) {
            String connectionId = trans.getRequestURI().getPath().substring(1);
            int port = trans.getRemoteAddress().getPort();
            if (clientPorts.containsKey(connectionId)) {
                int expectedPort = clientPorts.get(connectionId);
                if (expectedPort == port) {
                    System.out.println("Server handler for connectionId " + connectionId + ": Incoming connection seemingly reuses old connection (from port " + expectedPort + ")");
                } else {
                    String msg = "Server handler for connectionId " + connectionId + ": Incoming connection from different port (" + port + " instead of " + expectedPort + ")";
                    System.out.println(msg);
                    clientAsserts.put(connectionId, msg);
                }
            } else {
                System.out.println("Server handler for connectionId " + connectionId + ": Adding " + connectionId + "->" + port);
                clientPorts.put(connectionId, port);
            }
            try {
                trans.sendResponseHeaders(200, PAYLOAD.length);
                try (OutputStream os = trans.getResponseBody()) {
                    os.write(PAYLOAD);
                }
            } catch (IOException e) {
                clientAsserts.put(connectionId, e.getMessage());
                throw new RuntimeException(e);
            }
        }
    }

    static abstract class Request implements Supplier<String> {
        String connectionId;

        Request(String connectionId) {
            this.connectionId = connectionId;
        }
    }

    static class InitialRequest extends Request {
        InitialRequest(String connectionId) {
            super(connectionId);
        }

        @Override
        public String get() {
            System.out.println("Running initial request for key: " + connectionId);
            KeepAliveCache.connectionID.set(connectionId);
            try {
                URL url = URIBuilder.newBuilder()
                    .scheme("http")
                    .host(InetAddress.getLocalHost())
                    .port(server.getAddress().getPort())
                    .path("/" + connectionId)
                    .toURL();

                try (InputStream is = url.openConnection(Proxy.NO_PROXY).getInputStream()) {
                    clientSync.countDown();
                    clientSync.await();
                    byte[] ba = new byte[PAYLOAD.length];
                    is.read(ba);
                }
                System.out.println("Initial request for key " + connectionId + " done.");
                return connectionId;
            } catch (Exception e) {
                throw new RuntimeException("Error in request thread for key " + connectionId + ".", e);
            }
        }
    }

    static class SecondRequest extends Request {
        SecondRequest(String connectionId) {
            super(connectionId);
        }

        @Override
        public String get() {
            System.out.println("Running second request for key: " + connectionId);
            KeepAliveCache.connectionID.set(connectionId);
            try {
                URL url = URIBuilder.newBuilder()
                    .scheme("http")
                    .host(InetAddress.getLocalHost())
                    .port(server.getAddress().getPort())
                    .path("/" + connectionId)
                    .toURL();
                try (InputStream is = url.openConnection(Proxy.NO_PROXY).getInputStream()) {
                    byte[] ba = new byte[PAYLOAD.length];
                    is.read(ba);
                }
                System.out.println("Second request for key " + connectionId + " done.");
                return connectionId;
            } catch (Exception e) {
                throw new RuntimeException("Error in request thread for key " + connectionId + ".");
            }
        }
    }

    public static void initialize() {
        // start server
        try {
            server = HttpServer.create(new InetSocketAddress(InetAddress.getLocalHost(), 0), 10, "/", new TestConnectionIDFeature.TestHttpHandler());
        } catch (IOException e) {
            throw new RuntimeException("Could not create server", e);
        }
        server.setExecutor(serverExecutor);
        server.start();

        // initialize thread keys
        for (int i = 0; i < CLIENT_THREADS; i++) {
            connectionIds.push(Integer.toString(i));
        }
    }

    public static void runRequests() {
        // run initial set of requests in parallel to make sure that as many connections as the value of
        // CLIENT_THREADS are open. This is achieved by waiting for a joined synchronization latch while
        // the connections are still open.
        while (connectionIds.peek() != null) {
            clientFutures.add(CompletableFuture.supplyAsync(new InitialRequest(connectionIds.pop()), executor));
        }
        for (var future : clientFutures) {
            connectionIds.push(future.join());
        }

        // run second batch of requests where we expect that connections be reused
        clientFutures.clear();
        while (connectionIds.peek() != null) {
            clientFutures.add(CompletableFuture.supplyAsync(new InitialRequest(connectionIds.pop()), executor));
        }
        for (var future : clientFutures) {
            connectionIds.push(future.join());
        }

        // now check for any failures
        while (connectionIds.peek() != null) {
            String assertMsg = clientAsserts.get(connectionIds.pop());
            if (assertMsg != null) {
                throw new RuntimeException(assertMsg);
            }
        }
    }

    public static void shutdown() {
        server.stop(0);
        serverExecutor.shutdown();
        executor.shutdown();
    }

    public static void main(String[] args) {
        initialize();
        try {
            runRequests();
        } finally {
            shutdown();
        }
    }
}
