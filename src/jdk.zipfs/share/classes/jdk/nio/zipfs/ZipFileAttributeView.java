/*
 * Copyright (c) 2009, 2018, Oracle and/or its affiliates. All rights reserved.
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

package jdk.nio.zipfs;

import java.io.IOException;
import java.nio.file.attribute.BasicFileAttributeView;
import java.nio.file.attribute.FileAttributeView;
import java.nio.file.attribute.FileTime;
import java.nio.file.attribute.GroupPrincipal;
import java.nio.file.attribute.PosixFileAttributeView;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.UserPrincipal;
import java.util.LinkedHashMap;
import java.util.Map;

// SapMachine 2018-12-20 Support of PosixPermissions in zipfs
import java.util.Set;

/**
 * @author Xueming Shen, Rajendra Gutupalli, Jaya Hangal
 */
// SapMachine 2018-12-20 Support of PosixPermissions in zipfs
class ZipFileAttributeView implements PosixFileAttributeView {
    private static enum AttrID {
        size,
        creationTime,
        lastAccessTime,
        lastModifiedTime,
        isDirectory,
        isRegularFile,
        isSymbolicLink,
        isOther,
        fileKey,
        compressedSize,
        crc,
        // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
        method,
        permissions
    };

    // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
    private static enum ViewType {
        zip,
        posix,
        basic
    }

    private final ZipPath path;
    // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
    private final ViewType type;

    // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
    private ZipFileAttributeView(ZipPath path, ViewType type) {
        this.path = path;
        this.type = type;
    }

    @SuppressWarnings("unchecked") // Cast to V
    // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
    static <V extends FileAttributeView> V get(ZipPath path, Class<V> type) {
        if (type == null)
            throw new NullPointerException();
        if (type == BasicFileAttributeView.class)
            return (V)new ZipFileAttributeView(path, ViewType.basic);
        if (type == PosixFileAttributeView.class)
            return (V)new ZipFileAttributeView(path, ViewType.posix);
        if (type == ZipFileAttributeView.class)
            return (V)new ZipFileAttributeView(path, ViewType.zip);
        return null;
    }

    // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
    static ZipFileAttributeView get(ZipPath path, String type) {
        if (type == null)
            throw new NullPointerException();
        if (type.equals("basic"))
            return new ZipFileAttributeView(path, ViewType.basic);
        if (type.equals("posix"))
            return new ZipFileAttributeView(path, ViewType.posix);
        if (type.equals("zip"))
            return new ZipFileAttributeView(path, ViewType.zip);
        return null;
    }

    @Override
    public String name() {
        // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
        switch (type) {
        case zip:
            return "zip";
        case posix:
            return "posix";
        case basic:
        default:
            return "basic";
        }
    }

    public ZipFileAttributes readAttributes() throws IOException {
        return path.getAttributes();
    }

    @Override
    public void setTimes(FileTime lastModifiedTime,
                         FileTime lastAccessTime,
                         FileTime createTime)
        throws IOException
    {
        path.setTimes(lastModifiedTime, lastAccessTime, createTime);
    }

    // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
    @Override
    public UserPrincipal getOwner() throws IOException {
        throw new UnsupportedOperationException("ZipFileSystem does not support getOwner.");
    }

    @Override
    public void setOwner(UserPrincipal owner) throws IOException {
        throw new UnsupportedOperationException("ZipFileSystem does not support setOwner.");
    }

    @Override
    public void setPermissions(Set<PosixFilePermission> perms) throws IOException {
        path.setPermissions(perms);
    }

    @Override
    public void setGroup(GroupPrincipal group) throws IOException {
        throw new UnsupportedOperationException("ZipFileSystem does not support setGroup.");
    }

    // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
    @SuppressWarnings("unchecked")
    void setAttribute(String attribute, Object value)
        throws IOException
    {
        try {
            if (AttrID.valueOf(attribute) == AttrID.lastModifiedTime)
                setTimes((FileTime)value, null, null);
            if (AttrID.valueOf(attribute) == AttrID.lastAccessTime)
                setTimes(null, (FileTime)value, null);
            if (AttrID.valueOf(attribute) == AttrID.creationTime)
                setTimes(null, null, (FileTime)value);
            // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
            if (AttrID.valueOf(attribute) == AttrID.permissions)
                setPermissions((Set<PosixFilePermission>)value);
            return;
        } catch (IllegalArgumentException x) {}
        throw new UnsupportedOperationException("'" + attribute +
            "' is unknown or read-only attribute");
    }

    Map<String, Object> readAttributes(String attributes)
        throws IOException
    {
        ZipFileAttributes zfas = readAttributes();
        LinkedHashMap<String, Object> map = new LinkedHashMap<>();
        if ("*".equals(attributes)) {
            for (AttrID id : AttrID.values()) {
                try {
                    map.put(id.name(), attribute(id, zfas));
                } catch (IllegalArgumentException x) {}
            }
        } else {
            String[] as = attributes.split(",");
            for (String a : as) {
                try {
                    map.put(a, attribute(AttrID.valueOf(a), zfas));
                } catch (IllegalArgumentException x) {}
            }
        }
        return map;
    }

    Object attribute(AttrID id, ZipFileAttributes zfas) {
        switch (id) {
        case size:
            return zfas.size();
        case creationTime:
            return zfas.creationTime();
        case lastAccessTime:
            return zfas.lastAccessTime();
        case lastModifiedTime:
            return zfas.lastModifiedTime();
        case isDirectory:
            return zfas.isDirectory();
        case isRegularFile:
            return zfas.isRegularFile();
        case isSymbolicLink:
            return zfas.isSymbolicLink();
        case isOther:
            return zfas.isOther();
        case fileKey:
            return zfas.fileKey();
        // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
        case permissions:
            if (type == ViewType.zip || type == ViewType.posix) {
                try {
                    return zfas.permissions();
                } catch (UnsupportedOperationException e) {
                    return null;
                }
            }
            break;
        case compressedSize:
            // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
            if (type == ViewType.zip)
                return zfas.compressedSize();
            break;
        case crc:
            // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
            if (type == ViewType.zip)
                return zfas.crc();
            break;
        case method:
            // SapMachine 2018-12-20 Support of PosixPermissions in zipfs
            if (type == ViewType.zip)
                return zfas.method();
            break;
        }
        return null;
    }
}
