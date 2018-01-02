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

/*
 * @test
 * @summary Test for CONTINUATION frame handling
 * @modules java.base/sun.net.www.http
 *          jdk.incubator.httpclient/jdk.incubator.http.internal.common
 *          jdk.incubator.httpclient/jdk.incubator.http.internal.frame
 *          jdk.incubator.httpclient/jdk.incubator.http.internal.hpack
 * @library /lib/testlibrary server
 * @build Http2TestServer
 * @build jdk.testlibrary.SimpleSSLContext
 * @run testng/othervm ContinuationFrameTest
 */

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URI;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BiFunction;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSession;
import jdk.incubator.http.HttpClient;
import jdk.incubator.http.HttpRequest;
import jdk.incubator.http.HttpResponse;
import jdk.incubator.http.internal.common.HttpHeadersImpl;
import jdk.incubator.http.internal.frame.ContinuationFrame;
import jdk.incubator.http.internal.frame.HeaderFrame;
import jdk.incubator.http.internal.frame.HeadersFrame;
import jdk.incubator.http.internal.frame.Http2Frame;
import jdk.testlibrary.SimpleSSLContext;
import org.testng.annotations.AfterTest;
import org.testng.annotations.BeforeTest;
import org.testng.annotations.DataProvider;
import org.testng.annotations.Test;
import static java.lang.System.out;
import static jdk.incubator.http.HttpClient.Version.HTTP_2;
import static jdk.incubator.http.HttpRequest.BodyPublisher.fromString;
import static jdk.incubator.http.HttpResponse.BodyHandler.asString;
import static org.testng.Assert.assertEquals;
import static org.testng.Assert.assertTrue;

public class ContinuationFrameTest {

    SSLContext sslContext;
    Http2TestServer http2TestServer;   // HTTP/2 ( h2c )
    Http2TestServer https2TestServer;  // HTTP/2 ( h2  )
    String http2URI;
    String https2URI;

    /**
     * A function that returns a list of 1) a HEADERS frame ( with an empty
     * payload ), and 2) a CONTINUATION frame with the actual headers.
     */
    static BiFunction<Integer,List<ByteBuffer>,List<Http2Frame>> oneContinuation =
        (Integer streamid, List<ByteBuffer> encodedHeaders) -> {
            List<ByteBuffer> empty =  List.of(ByteBuffer.wrap(new byte[0]));
            HeadersFrame hf = new HeadersFrame(streamid, 0, empty);
            ContinuationFrame cf = new ContinuationFrame(streamid,
                                                         HeaderFrame.END_HEADERS,
                                                         encodedHeaders);
            return List.of(hf, cf);
        };

    /**
     * A function that returns a list of a HEADERS frame followed by a number of
     * CONTINUATION frames. Each frame contains just a single byte of payload.
     */
    static BiFunction<Integer,List<ByteBuffer>,List<Http2Frame>> byteAtATime =
        (Integer streamid, List<ByteBuffer> encodedHeaders) -> {
            assert encodedHeaders.get(0).hasRemaining();
            List<Http2Frame> frames = new ArrayList<>();
            ByteBuffer hb = ByteBuffer.wrap(new byte[] {encodedHeaders.get(0).get()});
            HeadersFrame hf = new HeadersFrame(streamid, 0, hb);
            frames.add(hf);
            for (ByteBuffer bb : encodedHeaders) {
                while (bb.hasRemaining()) {
                    List<ByteBuffer> data = List.of(ByteBuffer.wrap(new byte[] {bb.get()}));
                    ContinuationFrame cf = new ContinuationFrame(streamid, 0, data);
                    frames.add(cf);
                }
            }
            frames.get(frames.size() - 1).setFlag(HeaderFrame.END_HEADERS);
            return frames;
        };

    @DataProvider(name = "variants")
    public Object[][] variants() {
        return new Object[][] {
                { http2URI,  false, oneContinuation },
                { https2URI, false, oneContinuation },
                { http2URI,  true,  oneContinuation },
                { https2URI, true,  oneContinuation },

                { http2URI,  false, byteAtATime },
                { https2URI, false, byteAtATime },
                { http2URI,  true,  byteAtATime },
                { https2URI, true,  byteAtATime },
        };
    }

    static final int ITERATION_COUNT = 20;

    @Test(dataProvider = "variants")
    void test(String uri,
              boolean sameClient,
              BiFunction<Integer,List<ByteBuffer>,List<Http2Frame>> headerFramesSupplier)
        throws Exception
    {
        CFTHttp2TestExchange.setHeaderFrameSupplier(headerFramesSupplier);

        HttpClient client = null;
        for (int i=0; i< ITERATION_COUNT; i++) {
            if (!sameClient || client == null)
                client = HttpClient.newBuilder().sslContext(sslContext).build();

            HttpRequest request = HttpRequest.newBuilder(URI.create(uri))
                                             .POST(fromString("Hello there!"))
                                             .build();
            HttpResponse<String> resp;
            if (i % 2 == 0) {
                resp = client.send(request, asString());
            } else {
                resp = client.sendAsync(request, asString()).join();
            }

            out.println("Got response: " + resp);
            out.println("Got body: " + resp.body());
            assertTrue(resp.statusCode() == 200,
                       "Expected 200, got:" + resp.statusCode());
            assertEquals(resp.body(), "Hello there!");
            assertEquals(resp.version(), HTTP_2);
        }
    }

    @BeforeTest
    public void setup() throws Exception {
        sslContext = new SimpleSSLContext().get();
        if (sslContext == null)
            throw new AssertionError("Unexpected null sslContext");

        http2TestServer = new Http2TestServer("127.0.0.1", false, 0);
        http2TestServer.addHandler(new Http2EchoHandler(), "/http2/echo");
        int port = http2TestServer.getAddress().getPort();
        http2URI = "http://127.0.0.1:" + port + "/http2/echo";

        https2TestServer = new Http2TestServer("127.0.0.1", true, 0);
        https2TestServer.addHandler(new Http2EchoHandler(), "/https2/echo");
        port = https2TestServer.getAddress().getPort();
        https2URI = "https://127.0.0.1:" + port + "/https2/echo";

        // Override the default exchange supplier with a custom one to enable
        // particular test scenarios
        http2TestServer.setExchangeSupplier(CFTHttp2TestExchange::new);
        https2TestServer.setExchangeSupplier(CFTHttp2TestExchange::new);

        http2TestServer.start();
        https2TestServer.start();
    }

    @AfterTest
    public void teardown() throws Exception {
        http2TestServer.stop();
        https2TestServer.stop();
    }

    static class Http2EchoHandler implements Http2Handler {
        @Override
        public void handle(Http2TestExchange t) throws IOException {
            try (InputStream is = t.getRequestBody();
                 OutputStream os = t.getResponseBody()) {
                byte[] bytes = is.readAllBytes();
                t.getResponseHeaders().addHeader("just some", "noise");
                t.getResponseHeaders().addHeader("to add ", "payload in ");
                t.getResponseHeaders().addHeader("the header", "frames");
                t.sendResponseHeaders(200, bytes.length);
                os.write(bytes);
            }
        }
    }

    // A custom Http2TestExchangeImpl that overrides sendResponseHeaders to
    // allow headers to be sent with a number of CONTINUATION frames.
    static class CFTHttp2TestExchange extends Http2TestExchangeImpl {
        static volatile BiFunction<Integer,List<ByteBuffer>,List<Http2Frame>> headerFrameSupplier;

        static void setHeaderFrameSupplier(BiFunction<Integer,List<ByteBuffer>,List<Http2Frame>> hfs) {
            headerFrameSupplier = hfs;
        }

        CFTHttp2TestExchange(int streamid, String method, HttpHeadersImpl reqheaders,
                             HttpHeadersImpl rspheaders, URI uri, InputStream is,
                             SSLSession sslSession, BodyOutputStream os,
                             Http2TestServerConnection conn, boolean pushAllowed) {
            super(streamid, method, reqheaders, rspheaders, uri, is, sslSession,
                  os, conn, pushAllowed);

        }

        @Override
        public void sendResponseHeaders(int rCode, long responseLength) throws IOException {
            this.responseLength = responseLength;
            if (responseLength > 0 || responseLength < 0) {
                long clen = responseLength > 0 ? responseLength : 0;
                rspheaders.setHeader("Content-length", Long.toString(clen));
            }
            rspheaders.setHeader(":status", Integer.toString(rCode));

            List<ByteBuffer> encodeHeaders = conn.encodeHeaders(rspheaders);
            List<Http2Frame> headerFrames = headerFrameSupplier.apply(streamid, encodeHeaders);
            assert headerFrames.size() > 0;  // there must always be at least 1

            if (responseLength < 0) {
                headerFrames.get(headerFrames.size() -1).setFlag(HeadersFrame.END_STREAM);
                os.closeInternal();
            }

            for (Http2Frame f : headerFrames)
                conn.outputQ.put(f);

            os.goodToGo();
            System.err.println("Sent response headers " + rCode);
        }
    }
}
