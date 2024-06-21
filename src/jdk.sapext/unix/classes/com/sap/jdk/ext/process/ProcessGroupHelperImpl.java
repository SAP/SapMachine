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
package com.sap.jdk.ext.process;

import java.io.IOException;
import java.security.AccessController;
import java.security.PrivilegedAction;

import jdk.internal.access.JavaLangProcessBuilderAccess;
import jdk.internal.access.SharedSecrets;

/**
 * ProcessGroupHelper provides the possibility to create and terminate process groups.
 */
@SuppressWarnings("removal")
public final class ProcessGroupHelperImpl {

    /**
     * The base name of the jdk extensions library.
     */
    public static final String LIB_BASE_NAME = "sapjdkext";

    private static JavaLangProcessBuilderAccess jlpba = SharedSecrets.getJavaLangProcessBuilderAccess();

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
     * Native method that calls kill.
     *
     * @param pid Process id of the process leading the process group.
     * @param force Kill forcibly or politely.
     *
     * @return 0 in case of success, not 0 otherwise.
     */
    private static native int killProcessGroup0(long pid, boolean force);

    static final void createNewProcessGroupOnSpawn(ProcessBuilder pb, boolean value) {
        jlpba.createNewProcessGroupOnSpawn(pb, value);
    }

    static final void terminateProcessGroupForLeader(Process p, boolean force)
        throws IOException
    {
        int rc = killProcessGroup0(p.toHandle().pid(), force);
        if (rc != 0) {
            throw new IOException("Failed to kill process group (errno = " + rc + ")");
        }
    }

    private ProcessGroupHelperImpl() {
        // don't instantiate
    }
}
