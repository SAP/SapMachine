/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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

/* @test
 * @bug 8185582
 * @modules java.base/java.util.zip:open
 * @summary Check the resources of Inflater, Deflater and ZipFile are always
 *          cleaned/released when the instance is not unreachable
 */

import java.io.*;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.*;
import static java.nio.charset.StandardCharsets.US_ASCII;

public class TestCleaner {

    public static void main(String[] args) throws Throwable {
        testDeInflater();
        testZipFile();
    }

    private static long addrOf(Object obj) {
        try {
            Field addr = obj.getClass().getDeclaredField("address");
            if (!addr.trySetAccessible()) {
                return -1;
            }
            return addr.getLong(obj);
        } catch (Exception x) {
            return -1;
        }
    }

    private static class SubclassedInflater extends Inflater {
        CountDownLatch endCountDown;

        SubclassedInflater(CountDownLatch endCountDown) {
            this.endCountDown = endCountDown;
        }

        @Override
        public void end() {
            super.end();
            endCountDown.countDown();
        }
    }

    private static class SubclassedDeflater extends Deflater {
        CountDownLatch endCountDown;

        SubclassedDeflater(CountDownLatch endCountDown) {
            this.endCountDown = endCountDown;
        }

        @Override
        public void end() {
            super.end();
            endCountDown.countDown();
        }
    }

    // verify the "native resource" of In/Deflater has been cleaned
    private static void testDeInflater() throws Throwable {
        Field zsRefDef = Deflater.class.getDeclaredField("zsRef");
        Field zsRefInf = Inflater.class.getDeclaredField("zsRef");
        if (!zsRefDef.trySetAccessible() || !zsRefInf.trySetAccessible()) {
            throw new RuntimeException("'zsRef' is not accesible");
        }
        if (addrOf(zsRefDef.get(new Deflater())) == -1 ||
            addrOf(zsRefInf.get(new Inflater())) == -1) {
            throw new RuntimeException("'addr' is not accesible");
        }
        List<Object> list = new ArrayList<>();
        byte[] buf1 = new byte[1024];
        byte[] buf2 = new byte[1024];
        for (int i = 0; i < 10; i++) {
            var def = new Deflater();
            list.add(zsRefDef.get(def));
            def.setInput("hello".getBytes());
            def.finish();
            int n = def.deflate(buf1);

            var inf = new Inflater();
            list.add(zsRefInf.get(inf));
            inf.setInput(buf1, 0, n);
            n = inf.inflate(buf2);
            if (!"hello".equals(new String(buf2, 0, n))) {
                throw new RuntimeException("compression/decompression failed");
            }
        }

        int n = 10;
        long cnt = list.size();
        while (n-- > 0 && cnt != 0) {
            Thread.sleep(100);
            System.gc();
            cnt = list.stream().filter(o -> addrOf(o) != 0).count();
        }
        if (cnt != 0)
            throw new RuntimeException("cleaner failed to clean : " + cnt);

        // test subclassed Deflater/Inflater, for behavioral compatibility.
        // should be removed if the finalize() method is finally removed.
        var endCountDown = new CountDownLatch(20);
        for (int i = 0; i < 10; i++) {
            var def = new SubclassedDeflater(endCountDown);
            def.setInput("hello".getBytes());
            def.finish();
            n = def.deflate(buf1);

            var inf = new SubclassedInflater(endCountDown);
            inf.setInput(buf1, 0, n);
            n = inf.inflate(buf2);
            if (!"hello".equals(new String(buf2, 0, n))) {
                throw new RuntimeException("compression/decompression failed");
            }
        }
        while (!endCountDown.await(10, TimeUnit.MILLISECONDS)) {
            System.gc();
        }
        if (endCountDown.getCount() != 0)
            throw new RuntimeException("finalizer failed to clean : " +
                endCountDown.getCount());
    }

    private static class SubclassedZipFile extends ZipFile {
        CountDownLatch closeCountDown;

        SubclassedZipFile(File f, CountDownLatch closeCountDown)
            throws IOException {
            super(f);
            this.closeCountDown = closeCountDown;
        }

        @Override
        public void close() throws IOException {
            closeCountDown.countDown();
            super.close();
        }
    }

    private static void testZipFile() throws Throwable {
        File dir = new File(System.getProperty("test.dir", "."));
        File zip = File.createTempFile("testzf", "zip", dir);
        Object zsrc = null;
        try {
            try (var fos = new FileOutputStream(zip);
                 var zos = new ZipOutputStream(fos)) {
                zos.putNextEntry(new ZipEntry("hello"));
                zos.write("hello".getBytes(US_ASCII));
                zos.closeEntry();
            }

            var zf = new ZipFile(zip);
            var es = zf.entries();
            while (es.hasMoreElements()) {
                zf.getInputStream(es.nextElement()).read();
            }

            Field fieldRes = ZipFile.class.getDeclaredField("res");
            if (!fieldRes.trySetAccessible()) {
                throw new RuntimeException("'ZipFile.res' is not accesible");
            }
            Object zfRes = fieldRes.get(zf);
            if (zfRes == null) {
                throw new RuntimeException("'ZipFile.res' is null");
            }
            Field fieldZsrc = zfRes.getClass().getDeclaredField("zsrc");
            if (!fieldZsrc.trySetAccessible()) {
                throw new RuntimeException("'ZipFile.zsrc' is not accesible");
            }
            zsrc = fieldZsrc.get(zfRes);

        } finally {
            zip.delete();
        }

        if (zsrc != null) {
            Field zfileField = zsrc.getClass().getDeclaredField("zfile");
            if (!zfileField.trySetAccessible()) {
                throw new RuntimeException("'ZipFile.Source.zfile' is not accesible");
            }
            //System.out.println("zffile: " +  zfileField.get(zsrc));
            int n = 10;
            while (n-- > 0 && zfileField.get(zsrc) != null) {
                System.out.println("waiting gc ... " + n);
                System.gc();
                Thread.sleep(100);
            }
            if (zfileField.get(zsrc) != null) {
                throw new RuntimeException("cleaner failed to clean zipfile.");
            }
        }

        // test subclassed ZipFile, for behavioral compatibility.
        // should be removed if the finalize() method is finally removed.
        var closeCountDown = new CountDownLatch(1);
        try {
            try (var fos = new FileOutputStream(zip);
                 var zos = new ZipOutputStream(fos)) {
                zos.putNextEntry(new ZipEntry("hello"));
                zos.write("hello".getBytes(US_ASCII));
                zos.closeEntry();
            }
            var zf = new SubclassedZipFile(zip, closeCountDown);
            var es = zf.entries();
            while (es.hasMoreElements()) {
                zf.getInputStream(es.nextElement()).read();
            }
            es = null;
            zf = null;
        } finally {
            zip.delete();
        }
        while (!closeCountDown.await(10, TimeUnit.MILLISECONDS)) {
            System.gc();
        }
        if (closeCountDown.getCount() != 0)
            throw new RuntimeException("finalizer failed to clean : " +
                closeCountDown.getCount());
    }
}
