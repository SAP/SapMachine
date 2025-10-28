/*
 * Copyright (c) 2018, 2024 SAP SE. All rights reserved.
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
package com.sap.jdk.ext.util;

import java.lang.annotation.Native;
import java.security.AccessController;
import java.security.PrivilegedAction;

/**
 * This class provides methods for console handling.
 */
@SuppressWarnings({"removal", "restricted"})
public final class Console {

    /**
     * The base name of the jdk extensions library.
     */
    private static final String LIB_BASE_NAME = "jdksapext";

    static {
        if (System.getSecurityManager() == null) {
            System.loadLibrary(LIB_BASE_NAME);
        } else {
            AccessController.doPrivileged((PrivilegedAction<Void>) () -> {
                System.loadLibrary(LIB_BASE_NAME);
                return null;
            });
        }
    }

    /**
     * Enumeration of modes available for calls to {@link Console#setMode(Mode)}
     */
    public enum Mode {

        /**
         * This will restore the original console mode,
         * if it had been changed before by {@link Console#setMode(Mode)}
         */
        DEFAULT,

        /**
         * This sets the console to non canonical mode, allowing single character input.
         */
        NON_CANONICAL
    }

    // values that are used in the native implementation
    @Native private static final int MODE_DEFAULT = 0;
    @Native private static final int MODE_NON_CANONICAL = 1;

    /**
     * Operation that moves to the beginning of the buffer (ctrl-a).
     */
    @Native public static final short CHR_MOVE_TO_BEG = 1;

    /**
     * Operation that exits the command prompt.
     */
    @Native public static final short CHR_CANCEL = 3;

    /**
     * Operation that exits the command prompt.
     */
    @Native public static final short CHR_EXIT = 4;

    /**
     * Operation that moves to the end of the buffer (ctrl-e).
     */
    @Native public static final short CHR_MOVE_TO_END = 5;

    /**
     * Operation that issues a backspace.
     */
    @Native public static final short CHR_DELETE_PREV_CHAR = 8;

    /**
     * Operation that performs completion operation on the current word.
     */
    @Native public static final short CHR_COMPLETE = 9;

    /**
     * Operation that issues a newline.
     */
    @Native public static final short CHR_NEWLINE_1 = 10;

    /**
     *  Operation that deletes the buffer from the current character to the end (ctrl-k).
     */
    @Native public static final short CHR_KILL_LINE = 11;

    /**
     * Operation that issues a newline.
     */
    @Native public static final short CHR_NEWLINE_2 = 13;

    /**
     * Operation that clears whatever text is on the current line (ESC).
     */
    @Native public static final short CHR_CLEAR_LINE = 27;

    /**
     * Operation that issues a delete (DEL).
     */
    @Native public static final short CHR_DELETE_NEXT_CHAR = 127;

    /**
     * Operation that moved to the previous character in the buffer (arrow left)
     */
    @Native public static final short CHR_PREV_CHAR = 331;

    /**
     * Operation that moves to the next character in the buffer (arrow right).
     */
    @Native public static final short CHR_NEXT_CHAR = 333;

    /**
     * Operation that sets the buffer to the next history item (arrow down).
     */
    @Native public static final short CHR_NEXT_HISTORY = 336;

    /**
     * Operation that sets the buffer to the previous history item (arrow up).
     */
    @Native public static final short CHR_PREV_HISTORY = 328;

    /**
     * The console object.
     */
    private static Console console = new Console();

    // objects used for synchronizing calls
    private static Object modeLock = new Object();
    private static Object readLock = new Object();
    private static Object queryLock = new Object();

    /**
     * Private constructor to prohibit instantiation.
     */
    private Console() {}

    // native methods
    private static native void setMode0(int mode);
    private static native int readChar0();
    private static native int getWidth0();

    /**
     * Sets the console mode to one of the values of {@link Mode}.
     *
     * @param mode The mode to set.
     */
    public void setMode(Mode mode) {
        synchronized (modeLock) {
            switch (mode) {
            case DEFAULT:
                setMode0(MODE_DEFAULT);
                return;
            case NON_CANONICAL:
                setMode0(MODE_NON_CANONICAL);
                return;
            }
        }
    }

    /**
     * Reads a character from the console
     *
     * @return the character read from the console
     */
    public int readChar() {
        synchronized (readLock) {
            return readChar0();
        }
    }

    /**
     * Returns the console width or -1 if not available.
     *
     * @return the console width or -1 if not available.
     */
    public int getWidth() {
        synchronized (queryLock) {
            return getWidth0();
        }
    }

    /**
     * Returns the console instance
     *
     * @return the console instance
     */
    public static Console get() {
        return console;
    }
}
