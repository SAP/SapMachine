/*
 * Copyright (c) 2018, 2019 SAP SE. All rights reserved.
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
 * Please contact SAP SE, Dietmar-Hopp-Allee 16, 69190 Walldorf, Germany
 * or visit www.sap.com if you need additional information or have any
 * questions.
 */

import java.io.IOException;
import java.nio.file.CopyOption;
import java.nio.file.DirectoryStream;
import java.nio.file.FileSystem;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFileAttributes;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.file.spi.FileSystemProvider;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

import org.testng.annotations.Test;

import static java.nio.file.attribute.PosixFilePermission.GROUP_EXECUTE;
import static java.nio.file.attribute.PosixFilePermission.GROUP_READ;
import static java.nio.file.attribute.PosixFilePermission.GROUP_WRITE;
import static java.nio.file.attribute.PosixFilePermission.OTHERS_EXECUTE;
import static java.nio.file.attribute.PosixFilePermission.OTHERS_READ;
import static java.nio.file.attribute.PosixFilePermission.OTHERS_WRITE;
import static java.nio.file.attribute.PosixFilePermission.OWNER_EXECUTE;
import static java.nio.file.attribute.PosixFilePermission.OWNER_READ;
import static java.nio.file.attribute.PosixFilePermission.OWNER_WRITE;
import static org.testng.Assert.assertEquals;
import static org.testng.Assert.assertNotNull;
import static org.testng.Assert.assertNull;
import static org.testng.Assert.assertTrue;
import static org.testng.Assert.fail;

/**
 * @test
 * @modules jdk.zipfs
 * @run testng TestPosixPerms
 * @summary Test zip file operations handling POSIX permissions.
 */
public class TestPosixPerms {

    private static final String ZIP_FILE = "testPosixPerms.zip";
    private static final String ZIP_FILE_COPY = "testPosixPermsCopy.zip";
    private static final String ZIP_FILE_NEW = "testPosixPermsNew.zip";
    private static final String UNZIP_DIR = "unzip/";
    private static final Map<String, Object> CREATE_TRUE = new HashMap<>();
    private static final FileSystemProvider zipFSP;
    private static final CopyOption[] NO_OPTIONS = {};
    private static final CopyOption[] COPY_ATTRIBUTES = {StandardCopyOption.COPY_ATTRIBUTES};

    public static class CopyVisitor extends SimpleFileVisitor<Path> {
        private Path from, to;
        private final CopyOption[] options;

        public CopyVisitor(Path from, Path to, boolean manualCopyPosixPerms, CopyOption... options) {
            this.from = from;
            this.to = to;
            this.options = manualCopyPosixPerms ? NO_OPTIONS : COPY_ATTRIBUTES;
        }

        private static void copyPermissions(Path source, Path target) throws IOException {
            try {
                Set<PosixFilePermission> srcPerms = Files.getPosixFilePermissions(source);
                if (srcPerms != null) {
                    Files.setPosixFilePermissions(target, srcPerms);
                }
            } catch (UnsupportedOperationException e) {
                // if somebody does not support Posix file permissions, it's ok...
                return;
            }
        }

        @Override
        public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes attrs) throws IOException {
            Path target = to.resolve(from.relativize(dir).toString());
            if (!Files.exists(target)) {
                Files.copy(dir, target, options);
                if (options == NO_OPTIONS) {
                    copyPermissions(dir, target);
                }
            }
            return FileVisitResult.CONTINUE;
        }

        @Override
        public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
            Path target = to.resolve(from.relativize(file).toString());
            Files.copy(file, target, options);
            if (options == NO_OPTIONS) {
                copyPermissions(file, target);
            }
            return FileVisitResult.CONTINUE;
        }
    }

    private int entriesCreated;

    static {
        zipFSP = getZipFSProvider();
        assertNotNull(zipFSP, "ZIP filesystem provider is not installed");
        CREATE_TRUE.put("create", "true");
    }

    private static FileSystemProvider getZipFSProvider() {
        for (FileSystemProvider provider : FileSystemProvider.installedProviders()) {
            if ("jar".equals(provider.getScheme()))
                return provider;
        }
        return null;
    }

    private void checkPermissionsOfEntry(Path file, boolean directory, Set<PosixFilePermission> expected) {
        System.out.println("Checking " + file + "...");
        assertEquals(Files.isDirectory(file), directory, "Unexpected directory attribute.");
        try {
            System.out.println(Files.readAttributes(file, PosixFileAttributes.class).toString());
        } catch (IOException e) {
            fail("Failed to list file attributes (posix) for entry.", e);
        }
        Set<PosixFilePermission> permissions = null;
        try {
            permissions = Files.getPosixFilePermissions(file);
        } catch (IOException e) {
            fail("Caught unexpected exception obtaining posix file permissions.", e);
        }
        if (expected == null) {
            assertNull(permissions, "Returned permissions of entry are not null.");
        } else {
            assertNotNull(permissions, "Returned permissions of entry are null.");
            assertEquals(permissions.size(), expected.size(), "Unexpected number of permissions( " +
                permissions.size() + " received vs " + expected.size() + " expected).");
            for (PosixFilePermission p : expected) {
                assertTrue(permissions.contains(p), "Posix permission " + p + " missing.");
            }
        }
    }

    private void putFile(FileSystem fs, String name, Set<PosixFilePermission> perms) throws IOException {
        if (perms == null) {
            Files.createFile(fs.getPath(name));
        } else {
            Files.createFile(fs.getPath(name), PosixFilePermissions.asFileAttribute(perms));
        }
        entriesCreated++;
    }

    private void putDirectory(FileSystem fs, String name, Set<PosixFilePermission> perms) throws IOException {
        if (perms == null) {
            Files.createDirectory(fs.getPath(name));
        } else {
            Files.createDirectory(fs.getPath(name), PosixFilePermissions.asFileAttribute(perms));
        }
        entriesCreated++;
    }

    private FileSystem openOrcreateZipFile(Path zpath) throws Exception {
        if (Files.exists(zpath)) {
            FileSystem fs = zipFSP.newFileSystem(zpath, Collections.<String, Object>emptyMap());
            return fs;
        } else {
            System.out.println("Create " + zpath + "...");
            FileSystem fs = zipFSP.newFileSystem(zpath, CREATE_TRUE);
            putDirectory(fs, "dir", Set.of(
                OWNER_READ, OWNER_WRITE, OWNER_EXECUTE,
                GROUP_READ, GROUP_WRITE, GROUP_EXECUTE,
                OTHERS_READ, OTHERS_WRITE, OTHERS_EXECUTE));
            putFile(fs, "uread", Set.of(OWNER_READ));
            putFile(fs, "uwrite", Set.of(OWNER_WRITE));
            putFile(fs, "uexec", Set.of(OWNER_EXECUTE));
            putFile(fs, "gread", Set.of(GROUP_READ));
            putFile(fs, "gwrite", Set.of(GROUP_WRITE));
            putFile(fs, "gexec", Set.of(GROUP_EXECUTE));
            putFile(fs, "oread", Set.of(OTHERS_READ));
            putFile(fs, "owrite", Set.of(OTHERS_WRITE));
            putFile(fs, "oexec", Set.of(OTHERS_EXECUTE));
            putFile(fs, "emptyperms", Collections.<PosixFilePermission>emptySet());
            putFile(fs, "noperms", null);
            putFile(fs, "permsaddedlater", null);
            Files.setPosixFilePermissions(fs.getPath("permsaddedlater"), Set.of(OWNER_READ));
            return fs;
        }
    }

    private boolean isPosixFs(Path p) throws IOException {
        try {
            Files.getPosixFilePermissions(p);
            return true;
        } catch (UnsupportedOperationException e) {
            return false;
        }
    }

    private void checkPosixPerms(Path path) throws Exception {
        AtomicInteger entries = new AtomicInteger();

        try (DirectoryStream<Path> paths = Files.newDirectoryStream(path)) {
            paths.forEach(file -> {
                entries.getAndIncrement();
                String name = file.getFileName().toString();
                if (name.startsWith("dir")) {
                    checkPermissionsOfEntry(file, true, Set.of(
                        OWNER_READ, OWNER_WRITE, OWNER_EXECUTE,
                        GROUP_READ, GROUP_WRITE, GROUP_EXECUTE,
                        OTHERS_READ, OTHERS_WRITE, OTHERS_EXECUTE));
                } else if (name.equals("uread")) {
                    checkPermissionsOfEntry(file, false, Set.of(OWNER_READ));
                } else if (name.equals("uwrite")) {
                    checkPermissionsOfEntry(file, false, Set.of(OWNER_WRITE));
                } else if (name.equals("uexec")) {
                    checkPermissionsOfEntry(file, false, Set.of(OWNER_EXECUTE));
                } else if (name.equals("gread")) {
                    checkPermissionsOfEntry(file, false, Set.of(GROUP_READ));
                } else if (name.equals("gwrite")) {
                    checkPermissionsOfEntry(file, false, Set.of(GROUP_WRITE));
                } else if (name.equals("gexec")) {
                    checkPermissionsOfEntry(file, false, Set.of(GROUP_EXECUTE));
                } else if (name.equals("oread")) {
                    checkPermissionsOfEntry(file, false, Set.of(OTHERS_READ));
                } else if (name.equals("owrite")) {
                    checkPermissionsOfEntry(file, false, Set.of(OTHERS_WRITE));
                } else if (name.equals("oexec")) {
                    checkPermissionsOfEntry(file, false, Set.of(OTHERS_EXECUTE));
                } else if (name.equals("emptyperms")) {
                    checkPermissionsOfEntry(file, false, Collections.<PosixFilePermission>emptySet());
                } else if (name.equals("noperms")) {
                    // Only check for "no permissions" in files in the zip file system
                    // If such a file gets extracted into a "normal" file system it will
                    // get the system default permissions (i.e. "umask")
                    if ("jar".equals(path.getFileSystem().provider().getScheme())) {
                        checkPermissionsOfEntry(file, false, Collections.<PosixFilePermission>emptySet());
                    }
                } else if (name.equals("permsaddedlater")) {
                    checkPermissionsOfEntry(file, false, Set.of(OWNER_READ));
                } else {
                    fail("Found unknown entry " + name + ".");
                }
            });
        }
        System.out.println("Number of entries: " + entries.get() + ".");
        assertEquals(entries.get(), entriesCreated, "File contained wrong number of entries.");
    }

    @Test(priority=0)
    public void testWriteAndReadArchiveWithPosixPerms() throws Exception {
        Path in = Paths.get(System.getProperty("test.dir", "."), ZIP_FILE);

        try (FileSystem zipIn = openOrcreateZipFile(in)) {
            System.out.println("Test reading " + in + "...");
            checkPosixPerms(zipIn.getPath("/"));
        }
    }

    @Test(priority=1)
    public void testPosixPermsAfterCopy() throws Exception {
        Path in = Paths.get(System.getProperty("test.dir", "."), ZIP_FILE);
        Path out = Paths.get(System.getProperty("test.dir", "."), ZIP_FILE_COPY);

        try (FileSystem zipIn = openOrcreateZipFile(in);
             FileSystem zipOut = zipFSP.newFileSystem(out, CREATE_TRUE)) {
            Path from = zipIn.getPath("/");
            Path to = zipOut.getPath("/");
            Files.walkFileTree(from, new CopyVisitor(from, to, false, StandardCopyOption.COPY_ATTRIBUTES));
            System.out.println("Test reading " + out + "...");
            checkPosixPerms(to);
        }
    }

    @Test(priority=2)
    public void testPosixPermsAfterUnzip() throws Exception {
        Path in = Paths.get(System.getProperty("test.dir", "."), ZIP_FILE);
        Path out = Files.createDirectory(Paths.get(System.getProperty("test.dir", "."), UNZIP_DIR));

        try (FileSystem zipIn = openOrcreateZipFile(in)) {
            Path from = zipIn.getPath("/");
            Files.walkFileTree(from, new CopyVisitor(from, out, true, StandardCopyOption.COPY_ATTRIBUTES));
            System.out.println("Test reading " + out + "...");
            if (isPosixFs(out)) {
                checkPosixPerms(out);
            }
        }
    }

    @Test(priority=3)
    public void testPosixPermsAfterZip() throws Exception {
        Path in = Paths.get(System.getProperty("test.dir", "."), UNZIP_DIR);
        Path out = Paths.get(System.getProperty("test.dir", "."), ZIP_FILE_NEW);
        boolean posixFS = isPosixFs(in);

        try (FileSystem zipOut = zipFSP.newFileSystem(out, CREATE_TRUE)) {
            out = zipOut.getPath("/");

            // Have to hack this manually otherwise we won't be able to read these files at all
            Set<PosixFilePermission> uwrite_p = null, uexec_p = null, gread_p = null,
                gwrite_p = null, gexec_p = null, oread_p = null, owrite_p = null,
                oexec_p = null, emptyperms_p = null, noperms_p = null;
            if (posixFS) {
                // Make some files owner readable to be able to copy them into the zipfs
                Path uwrite = in.resolve("uwrite");
                uwrite_p = Files.getPosixFilePermissions(uwrite);
                uwrite_p.add(OWNER_READ);
                Files.setPosixFilePermissions(uwrite, uwrite_p);
                Path uexec = in.resolve("uexec");
                uexec_p = Files.getPosixFilePermissions(uexec);
                uexec_p.add(OWNER_READ);
                Files.setPosixFilePermissions(uexec, uexec_p);
                Path gread = in.resolve("gread");
                gread_p = Files.getPosixFilePermissions(gread);
                gread_p.add(OWNER_READ);
                Files.setPosixFilePermissions(gread, gread_p);
                Path gwrite = in.resolve("gwrite");
                gwrite_p = Files.getPosixFilePermissions(gwrite);
                gwrite_p.add(OWNER_READ);
                Files.setPosixFilePermissions(gwrite, gwrite_p);
                Path gexec = in.resolve("gexec");
                gexec_p = Files.getPosixFilePermissions(gexec);
                gexec_p.add(OWNER_READ);
                Files.setPosixFilePermissions(gexec, gexec_p);
                Path oread = in.resolve("oread");
                oread_p = Files.getPosixFilePermissions(oread);
                oread_p.add(OWNER_READ);
                Files.setPosixFilePermissions(oread, oread_p);
                Path owrite = in.resolve("owrite");
                owrite_p = Files.getPosixFilePermissions(owrite);
                owrite_p.add(OWNER_READ);
                Files.setPosixFilePermissions(owrite, owrite_p);
                Path oexec = in.resolve("oexec");
                oexec_p = Files.getPosixFilePermissions(oexec);
                oexec_p.add(OWNER_READ);
                Files.setPosixFilePermissions(oexec, oexec_p);
                Path emptyperms = in.resolve("emptyperms");
                emptyperms_p = Files.getPosixFilePermissions(emptyperms);
                emptyperms_p.add(OWNER_READ);
                Files.setPosixFilePermissions(emptyperms, emptyperms_p);
                Path noperms = in.resolve("noperms");
                noperms_p = Files.getPosixFilePermissions(noperms);
                noperms_p.add(OWNER_READ);
                Files.setPosixFilePermissions(noperms, noperms_p);
            }

            Files.walkFileTree(in, new CopyVisitor(in, out, true, StandardCopyOption.COPY_ATTRIBUTES));

            if (posixFS) {
                // Fix back all the files in the target zip file which have been made readable before
                uwrite_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("uwrite"), uwrite_p);
                uexec_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("uexec"), uexec_p);
                gread_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("gread"), gread_p);
                gwrite_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("gwrite"), gwrite_p);
                gexec_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("gexec"), gexec_p);
                oread_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("oread"), oread_p);
                owrite_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("owrite"), owrite_p);
                oexec_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("oexec"), oexec_p);
                emptyperms_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("emptyperms"), emptyperms_p);
                noperms_p.remove(OWNER_READ);
                Files.setPosixFilePermissions(zipOut.getPath("noperms"), noperms_p);
            }

            System.out.println("Test reading " + out + "...");
            if (posixFS) {
                checkPosixPerms(out);
            }
        }
    }
}
