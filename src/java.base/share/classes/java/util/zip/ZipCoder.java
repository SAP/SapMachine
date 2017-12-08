/*
 * Copyright (c) 2009, 2017, Oracle and/or its affiliates. All rights reserved.
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

package java.util.zip;

import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.charset.CharsetDecoder;
import java.nio.charset.CharsetEncoder;
import java.nio.charset.CoderResult;
import java.nio.charset.CodingErrorAction;
import java.util.Arrays;
import sun.nio.cs.ArrayDecoder;
import sun.nio.cs.ArrayEncoder;

/**
 * Utility class for zipfile name and comment decoding and encoding
 */

final class ZipCoder {

    private static boolean isASCII(byte[] ba, int off, int len) {
        for (int i = off; i < off + len; i++) {
            if (ba[i] < 0)
                return false;
        }
        return true;
    }

    private static boolean hasReplaceChar(byte[] ba) {
        for (int i = 0; i < ba.length; i++) {
            if (ba[i] == (byte)'?')
                return true;
        }
        return false;
    }

    String toString(byte[] ba, int off, int length) {

        // fastpath for UTF-8 cs and ascii only name, leverage the
        // compact string impl to avoid the unnecessary char[] copy/
        // paste. A temporary workaround before we have better approach,
        // such as a String constructor that throws exception for
        // malformed and/or unmappable characters, instead of silently
        // replacing with repl char
        if (isUTF8 && isASCII(ba, off, length)) {
            return new String(ba, off, length, cs);
        }

        CharsetDecoder cd = decoder().reset();
        int len = (int)(length * cd.maxCharsPerByte());
        char[] ca = new char[len];
        if (len == 0)
            return new String(ca);
        // UTF-8 only for now. Other ArrayDeocder only handles
        // CodingErrorAction.REPLACE mode. ZipCoder uses
        // REPORT mode.
        if (isUTF8 && cd instanceof ArrayDecoder) {
            int clen = ((ArrayDecoder)cd).decode(ba, off, length, ca);
            if (clen == -1)    // malformed
                throw new IllegalArgumentException("MALFORMED");
            return new String(ca, 0, clen);
        }
        ByteBuffer bb = ByteBuffer.wrap(ba, off, length);
        CharBuffer cb = CharBuffer.wrap(ca);
        CoderResult cr = cd.decode(bb, cb, true);
        if (!cr.isUnderflow())
            throw new IllegalArgumentException(cr.toString());
        cr = cd.flush(cb);
        if (!cr.isUnderflow())
            throw new IllegalArgumentException(cr.toString());
        return new String(ca, 0, cb.position());
    }

    String toString(byte[] ba, int length) {
        return toString(ba, 0, length);
    }

    String toString(byte[] ba) {
        return toString(ba, 0, ba.length);
    }

    byte[] getBytes(String s) {
        if (isUTF8) {
            // fastpath for UTF8. should only occur when the string
            // has malformed surrogates. A postscan should still be
            // faster and use less memory.
            byte[] ba = s.getBytes(cs);
            if (!hasReplaceChar(ba)) {
                return ba;
            }
        }
        CharsetEncoder ce = encoder().reset();
        char[] ca = s.toCharArray();
        int len = (int)(ca.length * ce.maxBytesPerChar());
        byte[] ba = new byte[len];
        if (len == 0)
            return ba;
        // UTF-8 only for now. Other ArrayDeocder only handles
        // CodingErrorAction.REPLACE mode.
        if (isUTF8 && ce instanceof ArrayEncoder) {
            int blen = ((ArrayEncoder)ce).encode(ca, 0, ca.length, ba);
            if (blen == -1)    // malformed
                throw new IllegalArgumentException("MALFORMED");
            return Arrays.copyOf(ba, blen);
        }
        ByteBuffer bb = ByteBuffer.wrap(ba);
        CharBuffer cb = CharBuffer.wrap(ca);
        CoderResult cr = ce.encode(cb, bb, true);
        if (!cr.isUnderflow())
            throw new IllegalArgumentException(cr.toString());
        cr = ce.flush(bb);
        if (!cr.isUnderflow())
            throw new IllegalArgumentException(cr.toString());
        if (bb.position() == ba.length)  // defensive copy?
            return ba;
        else
            return Arrays.copyOf(ba, bb.position());
    }

    // assume invoked only if "this" is not utf8
    byte[] getBytesUTF8(String s) {
        if (isUTF8)
            return getBytes(s);
        if (utf8 == null)
            utf8 = new ZipCoder(StandardCharsets.UTF_8);
        return utf8.getBytes(s);
    }

    String toStringUTF8(byte[] ba, int len) {
        return toStringUTF8(ba, 0, len);
    }

    String toStringUTF8(byte[] ba, int off, int len) {
        if (isUTF8)
            return toString(ba, off, len);
        if (utf8 == null)
            utf8 = new ZipCoder(StandardCharsets.UTF_8);
        return utf8.toString(ba, off, len);
    }

    boolean isUTF8() {
        return isUTF8;
    }

    private Charset cs;
    private CharsetDecoder dec;
    private CharsetEncoder enc;
    private boolean isUTF8;
    private ZipCoder utf8;

    private ZipCoder(Charset cs) {
        this.cs = cs;
        this.isUTF8 = cs.name().equals(StandardCharsets.UTF_8.name());
    }

    static ZipCoder get(Charset charset) {
        return new ZipCoder(charset);
    }

    private CharsetDecoder decoder() {
        if (dec == null) {
            dec = cs.newDecoder()
              .onMalformedInput(CodingErrorAction.REPORT)
              .onUnmappableCharacter(CodingErrorAction.REPORT);
        }
        return dec;
    }

    private CharsetEncoder encoder() {
        if (enc == null) {
            enc = cs.newEncoder()
              .onMalformedInput(CodingErrorAction.REPORT)
              .onUnmappableCharacter(CodingErrorAction.REPORT);
        }
        return enc;
    }
}
