/*
 * Copyright (c) 2024 SAP SE. All rights reserved.
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
package com.sap.jdk.ext;

import java.security.AccessController;
import java.security.PrivilegedAction;

@SuppressWarnings("removal")
public final class NativeLibHelper {

    /**
     * The base name of the jdk extensions library.
     */
    private static final String LIB_BASE_NAME = "jdksapext";

    private static boolean loaded;

    private static synchronized void loadLibrary() {
        if (loaded) {
            return;
        }

        if (System.getSecurityManager() == null) {
            System.loadLibrary(LIB_BASE_NAME);
        } else {
            AccessController.doPrivileged((PrivilegedAction<Void>) () -> {
                System.loadLibrary(LIB_BASE_NAME);
                return null;
            });
        }
    }

    public static void load() {
        if (loaded == false) {
            loadLibrary();
        }
    }

    private NativeLibHelper() {
        // don't instantiate
    }
}
