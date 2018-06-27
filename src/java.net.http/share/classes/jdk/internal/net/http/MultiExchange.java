/*
 * Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

package jdk.internal.net.http;

import java.io.IOException;
import java.net.ConnectException;
import java.time.Duration;
import java.util.Iterator;
import java.util.LinkedList;
import java.security.AccessControlContext;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;

import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.net.http.HttpResponse.PushPromiseHandler;
import java.net.http.HttpTimeoutException;
import jdk.internal.net.http.common.Log;
import jdk.internal.net.http.common.Logger;
import jdk.internal.net.http.common.MinimalFuture;
import jdk.internal.net.http.common.ConnectionExpiredException;
import jdk.internal.net.http.common.Utils;
import static jdk.internal.net.http.common.MinimalFuture.completedFuture;
import static jdk.internal.net.http.common.MinimalFuture.failedFuture;

/**
 * Encapsulates multiple Exchanges belonging to one HttpRequestImpl.
 * - manages filters
 * - retries due to filters.
 * - I/O errors and most other exceptions get returned directly to user
 *
 * Creates a new Exchange for each request/response interaction
 */
class MultiExchange<T> {

    static final Logger debug =
            Utils.getDebugLogger("MultiExchange"::toString, Utils.DEBUG);

    private final HttpRequest userRequest; // the user request
    private final HttpRequestImpl request; // a copy of the user request
    final AccessControlContext acc;
    final HttpClientImpl client;
    final HttpResponse.BodyHandler<T> responseHandler;
    final HttpClientImpl.DelegatingExecutor executor;
    final AtomicInteger attempts = new AtomicInteger();
    HttpRequestImpl currentreq; // used for retries & redirect
    HttpRequestImpl previousreq; // used for retries & redirect
    Exchange<T> exchange; // the current exchange
    Exchange<T> previous;
    volatile Throwable retryCause;
    volatile boolean expiredOnce;
    volatile HttpResponse<T> response = null;

    // Maximum number of times a request will be retried/redirected
    // for any reason

    static final int DEFAULT_MAX_ATTEMPTS = 5;
    static final int max_attempts = Utils.getIntegerNetProperty(
            "jdk.httpclient.redirects.retrylimit", DEFAULT_MAX_ATTEMPTS
    );

    private final LinkedList<HeaderFilter> filters;
    TimedEvent timedEvent;
    volatile boolean cancelled;
    final PushGroup<T> pushGroup;

    /**
     * Filter fields. These are attached as required by filters
     * and only used by the filter implementations. This could be
     * generalised into Objects that are passed explicitly to the filters
     * (one per MultiExchange object, and one per Exchange object possibly)
     */
    volatile AuthenticationFilter.AuthInfo serverauth, proxyauth;
    // RedirectHandler
    volatile int numberOfRedirects = 0;

    /**
     * MultiExchange with one final response.
     */
    MultiExchange(HttpRequest userRequest,
                  HttpRequestImpl requestImpl,
                  HttpClientImpl client,
                  HttpResponse.BodyHandler<T> responseHandler,
                  PushPromiseHandler<T> pushPromiseHandler,
                  AccessControlContext acc) {
        this.previous = null;
        this.userRequest = userRequest;
        this.request = requestImpl;
        this.currentreq = request;
        this.previousreq = null;
        this.client = client;
        this.filters = client.filterChain();
        this.acc = acc;
        this.executor = client.theExecutor();
        this.responseHandler = responseHandler;

        if (pushPromiseHandler != null) {
            Executor executor = acc == null
                    ? this.executor.delegate()
                    : new PrivilegedExecutor(this.executor.delegate(), acc);
            this.pushGroup = new PushGroup<>(pushPromiseHandler, request, executor);
        } else {
            pushGroup = null;
        }

        this.exchange = new Exchange<>(request, this);
    }

    private synchronized Exchange<T> getExchange() {
        return exchange;
    }

    HttpClientImpl client() {
        return client;
    }

    HttpClient.Version version() {
        HttpClient.Version vers = request.version().orElse(client.version());
        if (vers == HttpClient.Version.HTTP_2 && !request.secure() && request.proxy() != null)
            vers = HttpClient.Version.HTTP_1_1;
        return vers;
    }

    private synchronized void setExchange(Exchange<T> exchange) {
        if (this.exchange != null && exchange != this.exchange) {
            this.exchange.released();
        }
        this.exchange = exchange;
    }

    private void cancelTimer() {
        if (timedEvent != null) {
            client.cancelTimer(timedEvent);
        }
    }

    private void requestFilters(HttpRequestImpl r) throws IOException {
        Log.logTrace("Applying request filters");
        for (HeaderFilter filter : filters) {
            Log.logTrace("Applying {0}", filter);
            filter.request(r, this);
        }
        Log.logTrace("All filters applied");
    }

    private HttpRequestImpl responseFilters(Response response) throws IOException
    {
        Log.logTrace("Applying response filters");
        Iterator<HeaderFilter> reverseItr = filters.descendingIterator();
        while (reverseItr.hasNext()) {
            HeaderFilter filter = reverseItr.next();
            Log.logTrace("Applying {0}", filter);
            HttpRequestImpl newreq = filter.response(response);
            if (newreq != null) {
                Log.logTrace("New request: stopping filters");
                return newreq;
            }
        }
        Log.logTrace("All filters applied");
        return null;
    }

    public void cancel(IOException cause) {
        cancelled = true;
        getExchange().cancel(cause);
    }

    public CompletableFuture<HttpResponse<T>> responseAsync(Executor executor) {
        CompletableFuture<Void> start = new MinimalFuture<>();
        CompletableFuture<HttpResponse<T>> cf = responseAsync0(start);
        start.completeAsync( () -> null, executor); // trigger execution
        return cf;
    }

    private CompletableFuture<HttpResponse<T>>
    responseAsync0(CompletableFuture<Void> start) {
        return start.thenCompose( v -> responseAsyncImpl())
                    .thenCompose((Response r) -> {
                        Exchange<T> exch = getExchange();
                        return exch.readBodyAsync(responseHandler)
                            .thenApply((T body) -> {
                                this.response =
                                    new HttpResponseImpl<>(r.request(), r, this.response, body, exch);
                                return this.response;
                            });
                    });
    }

    private CompletableFuture<Response> responseAsyncImpl() {
        CompletableFuture<Response> cf;
        if (attempts.incrementAndGet() > max_attempts) {
            cf = failedFuture(new IOException("Too many retries", retryCause));
        } else {
            if (currentreq.timeout().isPresent()) {
                timedEvent = new TimedEvent(currentreq.timeout().get());
                client.registerTimer(timedEvent);
            }
            try {
                // 1. apply request filters
                // if currentreq == previousreq the filters have already
                // been applied once. Applying them a second time might
                // cause some headers values to be added twice: for
                // instance, the same cookie might be added again.
                if (currentreq != previousreq) {
                    requestFilters(currentreq);
                }
            } catch (IOException e) {
                return failedFuture(e);
            }
            Exchange<T> exch = getExchange();
            // 2. get response
            cf = exch.responseAsync()
                     .thenCompose((Response response) -> {
                        HttpRequestImpl newrequest;
                        try {
                            // 3. apply response filters
                            newrequest = responseFilters(response);
                        } catch (IOException e) {
                            return failedFuture(e);
                        }
                        // 4. check filter result and repeat or continue
                        if (newrequest == null) {
                            if (attempts.get() > 1) {
                                Log.logError("Succeeded on attempt: " + attempts);
                            }
                            return completedFuture(response);
                        } else {
                            this.response =
                                new HttpResponseImpl<>(currentreq, response, this.response, null, exch);
                            Exchange<T> oldExch = exch;
                            return exch.ignoreBody().handle((r,t) -> {
                                previousreq = currentreq;
                                currentreq = newrequest;
                                expiredOnce = false;
                                setExchange(new Exchange<>(currentreq, this, acc));
                                return responseAsyncImpl();
                            }).thenCompose(Function.identity());
                        } })
                     .handle((response, ex) -> {
                        // 5. handle errors and cancel any timer set
                        cancelTimer();
                        if (ex == null) {
                            assert response != null;
                            return completedFuture(response);
                        }
                        // all exceptions thrown are handled here
                        CompletableFuture<Response> errorCF = getExceptionalCF(ex);
                        if (errorCF == null) {
                            return responseAsyncImpl();
                        } else {
                            return errorCF;
                        } })
                     .thenCompose(Function.identity());
        }
        return cf;
    }

    private static boolean retryPostValue() {
        String s = Utils.getNetProperty("jdk.httpclient.enableAllMethodRetry");
        if (s == null)
            return false;
        return s.isEmpty() ? true : Boolean.parseBoolean(s);
    }

    private static boolean retryConnect() {
        String s = Utils.getNetProperty("jdk.httpclient.disableRetryConnect");
        if (s == null)
            return false;
        return s.isEmpty() ? true : Boolean.parseBoolean(s);
    }

    /** True if ALL ( even non-idempotent ) requests can be automatic retried. */
    private static final boolean RETRY_ALWAYS = retryPostValue();
    /** True if ConnectException should cause a retry. Enabled by default */
    private static final boolean RETRY_CONNECT = retryConnect();

    /** Returns true is given request has an idempotent method. */
    private static boolean isIdempotentRequest(HttpRequest request) {
        String method = request.method();
        switch (method) {
            case "GET" :
            case "HEAD" :
                return true;
            default :
                return false;
        }
    }

    /** Returns true if the given request can be automatically retried. */
    private static boolean canRetryRequest(HttpRequest request) {
        if (RETRY_ALWAYS)
            return true;
        if (isIdempotentRequest(request))
            return true;
        return false;
    }

    private boolean retryOnFailure(Throwable t) {
        return t instanceof ConnectionExpiredException
                || (RETRY_CONNECT && (t instanceof ConnectException));
    }

    private Throwable retryCause(Throwable t) {
        Throwable cause = t instanceof ConnectionExpiredException ? t.getCause() : t;
        return cause == null ? t : cause;
    }

    /**
     * Takes a Throwable and returns a suitable CompletableFuture that is
     * completed exceptionally, or null.
     */
    private CompletableFuture<Response> getExceptionalCF(Throwable t) {
        if ((t instanceof CompletionException) || (t instanceof ExecutionException)) {
            if (t.getCause() != null) {
                t = t.getCause();
            }
        }
        if (cancelled && t instanceof IOException) {
            t = new HttpTimeoutException("request timed out");
        } else if (retryOnFailure(t)) {
            Throwable cause = retryCause(t);

            if (!(t instanceof ConnectException)) {
                if (!canRetryRequest(currentreq)) {
                    return failedFuture(cause); // fails with original cause
                }
            }

            // allow the retry mechanism to do its work
            retryCause = cause;
            if (!expiredOnce) {
                if (debug.on())
                    debug.log(t.getClass().getSimpleName() + " (async): retrying...", t);
                expiredOnce = true;
                // The connection was abruptly closed.
                // We return null to retry the same request a second time.
                // The request filters have already been applied to the
                // currentreq, so we set previousreq = currentreq to
                // prevent them from being applied again.
                previousreq = currentreq;
                return null;
            } else {
                if (debug.on()) {
                    debug.log(t.getClass().getSimpleName()
                            + " (async): already retried once.", t);
                }
                t = cause;
            }
        }
        return failedFuture(t);
    }

    class TimedEvent extends TimeoutEvent {
        TimedEvent(Duration duration) {
            super(duration);
        }
        @Override
        public void handle() {
            if (debug.on()) {
                debug.log("Cancelling MultiExchange due to timeout for request %s",
                        request);
            }
            cancel(new HttpTimeoutException("request timed out"));
        }
    }
}
