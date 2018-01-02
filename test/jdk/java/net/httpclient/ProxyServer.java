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

import java.net.*;
import java.io.*;
import java.util.*;
import java.security.*;

/**
 * A minimal proxy server that supports CONNECT tunneling. It does not do
 * any header transformations. In future this could be added.
 * Two threads are created per client connection. So, it's not
 * intended for large numbers of parallel connections.
 */
public class ProxyServer extends Thread implements Closeable {

    ServerSocket listener;
    int port;
    volatile boolean debug;

    /**
     * Create proxy on port (zero means don't care). Call getPort()
     * to get the assigned port.
     */
    public ProxyServer(Integer port) throws IOException {
        this(port, false);
    }

    public ProxyServer(Integer port, Boolean debug) throws IOException {
        this.debug = debug;
        listener = new ServerSocket(port);
        this.port = listener.getLocalPort();
        setName("ProxyListener");
        setDaemon(true);
        connections = new LinkedList<>();
        start();
    }

    public ProxyServer(String s) {  }

    /**
     * Returns the port number this proxy is listening on
     */
    public int getPort() {
        return port;
    }

    /**
     * Shuts down the proxy, probably aborting any connections
     * currently open
     */
    public void close() throws IOException {
        if (debug) System.out.println("Proxy: closing");
            done = true;
        listener.close();
        for (Connection c : connections) {
            if (c.running()) {
                c.close();
            }
        }
    }

    List<Connection> connections;

    volatile boolean done;

    public void run() {
        if (System.getSecurityManager() == null) {
            execute();
        } else {
            // so calling domain does not need to have socket permission
            AccessController.doPrivileged(new PrivilegedAction<Void>() {
                public Void run() {
                    execute();
                    return null;
                }
            });
        }
    }

    public void execute() {
        try {
            while(!done) {
                Socket s = listener.accept();
                if (debug)
                    System.out.println("Client: " + s);
                Connection c = new Connection(s);
                connections.add(c);
            }
        } catch(Throwable e) {
            if (debug && !done) {
                System.out.println("Fatal error: Listener: " + e);
                e.printStackTrace();
            }
        }
    }

    /**
     * Transparently forward everything, once we know what the destination is
     */
    class Connection {

        Socket clientSocket, serverSocket;
        Thread out, in;
        volatile InputStream clientIn, serverIn;
        volatile OutputStream clientOut, serverOut;

        boolean forwarding = false;

        final static int CR = 13;
        final static int LF = 10;

        Connection(Socket s) throws IOException {
            this.clientSocket= s;
            this.clientIn = new BufferedInputStream(s.getInputStream());
            this.clientOut = s.getOutputStream();
            init();
        }

        byte[] readHeaders(InputStream is) throws IOException {
            byte[] outbuffer = new byte[8000];
            int crlfcount = 0;
            int bytecount = 0;
            int c;
            while ((c=is.read()) != -1 && bytecount < outbuffer.length) {
                outbuffer[bytecount++] = (byte)c;
                if (debug) System.out.write(c);
                // were looking for CRLFCRLF sequence
                if (c == CR || c == LF) {
                    switch(crlfcount) {
                        case 0:
                            if (c == CR) crlfcount ++;
                            break;
                        case 1:
                            if (c == LF) crlfcount ++;
                            break;
                        case 2:
                            if (c == CR) crlfcount ++;
                            break;
                        case 3:
                            if (c == LF) crlfcount ++;
                            break;
                    }
                } else {
                    crlfcount = 0;
                }
                if (crlfcount == 4) {
                    break;
                }
            }
            byte[] ret = new byte[bytecount];
            System.arraycopy(outbuffer, 0, ret, 0, bytecount);
            return ret;
        }

        boolean running() {
            return out.isAlive() || in.isAlive();
        }

        public void close() throws IOException {
            if (debug) System.out.println("Closing connection (proxy)");
            if (serverSocket != null) serverSocket.close();
            if (clientSocket != null) clientSocket.close();
        }

        int findCRLF(byte[] b) {
            for (int i=0; i<b.length-1; i++) {
                if (b[i] == CR && b[i+1] == LF) {
                    return i;
                }
            }
            return -1;
        }

        public void init() {
            try {
                byte[] buf = readHeaders(clientIn);
                int p = findCRLF(buf);
                if (p == -1) {
                    close();
                    return;
                }
                String cmd = new String(buf, 0, p, "US-ASCII");
                String[] params = cmd.split(" ");
                if (params[0].equals("CONNECT")) {
                    doTunnel(params[1]);
                } else {
                    doProxy(params[1], buf, p, cmd);
                }
            } catch (Throwable e) {
                if (debug) {
                    System.out.println (e);
                }
                try {close(); } catch (IOException e1) {}
            }
        }

        void doProxy(String dest, byte[] buf, int p, String cmdLine)
            throws IOException
        {
            try {
                URI uri = new URI(dest);
                if (!uri.isAbsolute()) {
                    throw new IOException("request URI not absolute");
                }
                dest = uri.getAuthority();
                // now extract the path from the URI and recreate the cmd line
                int sp = cmdLine.indexOf(' ');
                String method = cmdLine.substring(0, sp);
                cmdLine = method + " " + uri.getPath() + " HTTP/1.1";
                int x = cmdLine.length() - 1;
                int i = p;
                while (x >=0) {
                    buf[i--] = (byte)cmdLine.charAt(x--);
                }
                i++;

                commonInit(dest, 80);
                serverOut.write(buf, i, buf.length-i);
                proxyCommon();

            } catch (URISyntaxException e) {
                throw new IOException(e);
            }
        }

        void commonInit(String dest, int defaultPort) throws IOException {
            int port;
            String[] hostport = dest.split(":");
            if (hostport.length == 1) {
                port = defaultPort;
            } else {
                port = Integer.parseInt(hostport[1]);
            }
            if (debug) System.out.printf("Server: (%s/%d)\n", hostport[0], port);
            serverSocket = new Socket(hostport[0], port);
            serverOut = serverSocket.getOutputStream();

            serverIn = new BufferedInputStream(serverSocket.getInputStream());
        }

        void proxyCommon() throws IOException {
            out = new Thread(() -> {
                try {
                    byte[] bb = new byte[8000];
                    int n;
                    while ((n = clientIn.read(bb)) != -1) {
                        serverOut.write(bb, 0, n);
                    }
                    serverSocket.close();
                    clientSocket.close();
                } catch (IOException e) {
                    if (debug) {
                        System.out.println (e);
                    }
                }
            });
            in = new Thread(() -> {
                try {
                    byte[] bb = new byte[8000];
                    int n;
                    while ((n = serverIn.read(bb)) != -1) {
                        clientOut.write(bb, 0, n);
                    }
                    serverSocket.close();
                    clientSocket.close();
                } catch (IOException e) {
                    if (debug) {
                        System.out.println(e);
                        e.printStackTrace();
                    }
                }
            });
            out.setName("Proxy-outbound");
            out.setDaemon(true);
            in.setDaemon(true);
            in.setName("Proxy-inbound");
            out.start();
            in.start();
        }

        void doTunnel(String dest) throws IOException {
            commonInit(dest, 443);
            clientOut.write("HTTP/1.1 200 OK\r\n\r\n".getBytes());
            proxyCommon();
        }
    }

    public static void main(String[] args) throws Exception {
        int port = Integer.parseInt(args[0]);
        boolean debug = args.length > 1 && args[1].equals("-debug");
        System.out.println("Debugging : " + debug);
        ProxyServer ps = new ProxyServer(port, debug);
        System.out.println("Proxy server listening on port " + ps.getPort());
        while (true) {
            Thread.sleep(5000);
        }
    }
}
