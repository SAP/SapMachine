/*
 * Copyright (c) 2026 SAP SE. All rights reserved.
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

import java.nio.file.Path;

import jdk.internal.access.JdkNioZipfsAccess;
import jdk.internal.access.SharedSecrets;
 
public class ZipfsUtils {

    // Silence warning about implicit ctor.
    private ZipfsUtils() {
    }

    /**
     * Returns <code>true</code> if the given path is a {@link jdk.nio.zipfs.ZipPath} which
     * represents a symbolic link.
     *
     * @param path The path in the zipfs.
     * @return <code>true</code> if the path represents a symbolic link.
     */
    public static boolean isSymbolicLink(Path path) {
        JdkNioZipfsAccess access = SharedSecrets.getJdkNioZipfsAccess();

        if (access == null) {
            return false;
        }

        return access.isSymbolicLink(path);
    }
}
