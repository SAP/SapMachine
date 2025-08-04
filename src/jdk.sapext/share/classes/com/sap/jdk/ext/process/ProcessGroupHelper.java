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

import jdk.internal.access.JavaLangProcessAccess;
import jdk.internal.access.JavaLangProcessBuilderAccess;
import jdk.internal.access.SharedSecrets;

/**
 * ProcessGroupHelper provides the possibility to create and terminate process groups.
 */
public final class ProcessGroupHelper {

    private static JavaLangProcessAccess jlpa = SharedSecrets.getJavaLangProcessAccess();
    private static JavaLangProcessBuilderAccess jlpba = SharedSecrets.getJavaLangProcessBuilderAccess();

    /**
     * With this API a ProcessBuilder instance can be configured to create a new process group upon spawning the process.
     *
     * @param pb The ProcessBuilder that shall be configured.
     * @param value true if a new process group shall be created, false otherwise.
     */
    public static final void createNewProcessGroupOnSpawn(ProcessBuilder pb, boolean value) {
        jlpba.createNewProcessGroupOnSpawn(pb, value);
    }

    /**
     * Given a process supposed to be leader of a process group, attempts to kill that process group.
     *
     * @param pid Process id of the process leading the process group.
     * @param force Kill forcibly or politely.
     *
     * @throws IOException Process not alive or not a process group leader (safemode = true); Operation failed.
     * @throws UnsupportedOperationException Operation not supported on this platform
     */
    public static final void terminateProcessGroupForLeader(Process p, boolean force) throws IOException {
        jlpa.destroyProcessGroup(p, force);
    }

    private ProcessGroupHelper() {
        // don't instantiate
    }
}
