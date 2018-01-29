/*
 * Copyright (c) 1998, 2018, Oracle and/or its affiliates. All rights reserved.
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

package jdk.javadoc.internal.doclets.toolkit.util;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import javax.lang.model.element.ModuleElement;
import javax.lang.model.element.PackageElement;
import javax.lang.model.element.TypeElement;

/**
 * Abstraction for immutable relative paths.
 * Paths always use '/' as a separator, and never begin or end with '/'.
 *
 *  <p><b>This is NOT part of any supported API.
 *  If you write code that depends on this, you do so at your own risk.
 *  This code and its internal interfaces are subject to change or
 *  deletion without notice.</b>
 */
public class DocPath {
    private final String path;

    /** The empty path. */
    public static final DocPath empty = new DocPath("");

    /** The empty path. */
    public static final DocPath parent = new DocPath("..");

    /**
     * Creates a path from a string.
     * @param p the string
     * @return the path
     */
    public static DocPath create(String p) {
        return (p == null) || p.isEmpty() ? empty : new DocPath(p);
    }

    /**
     * Returns the path for a class.
     * For example, if the class is java.lang.Object,
     * the path is java/lang/Object.html.
     * @param utils utility class for handling type elements
     * @param typeElement the type element
     * @return the path
     */
    public static DocPath forClass(Utils utils, TypeElement typeElement) {
        return (typeElement == null)
                ? empty
                : forPackage(utils.containingPackage(typeElement)).resolve(forName(utils, typeElement));
    }

    /**
     * Returns the path for the simple name of a class.
     * For example, if the class is java.lang.Object,
     * the path is Object.html.
     * @param utils utility class for handling type elements
     * @param typeElement the type element
     * @return the path
     */
    public static DocPath forName(Utils utils, TypeElement typeElement) {
        return (typeElement == null) ? empty : new DocPath(utils.getSimpleName(typeElement) + ".html");
    }

    /**
     * Returns the path for the name of a module.
     * For example, if the module is java.base,
     * the path is java.base.
     * @param mdle the module element
     * @return the path
     */
    public static DocPath forModule(ModuleElement mdle) {
        return mdle == null || mdle.isUnnamed()
                ? empty
                : DocPath.create(mdle.getQualifiedName().toString());
    }

    /**
     * Returns the path for the package of a class.
     * For example, if the class is java.lang.Object,
     * the path is java/lang.
     * @param utils utility class for handling type elements
     * @param typeElement the type element
     * @return the path
     */
    public static DocPath forPackage(Utils utils, TypeElement typeElement) {
        return (typeElement == null) ? empty : forPackage(utils.containingPackage(typeElement));
    }

    /**
     * Returns the path for a package.
     * For example, if the package is java.lang,
     * the path is java/lang.
     * @param pkgElement the package element
     * @return the path
     */
    public static DocPath forPackage(PackageElement pkgElement) {
        return pkgElement == null || pkgElement.isUnnamed()
                ? empty
                : DocPath.create(pkgElement.getQualifiedName().toString().replace('.', '/'));
    }

    /**
     * Returns the inverse path for a package.
     * For example, if the package is java.lang,
     * the inverse path is ../...
     * @param pkgElement the package element
     * @return the path
     */
    public static DocPath forRoot(PackageElement pkgElement) {
        String name = (pkgElement == null || pkgElement.isUnnamed())
                ? ""
                : pkgElement.getQualifiedName().toString();
        return new DocPath(name.replace('.', '/').replaceAll("[^/]+", ".."));
    }

    /**
     * Returns the relative path from one package to another.
     * @param from the initial package
     * @param to the target package
     * @return the path
     */
    public static DocPath relativePath(PackageElement from, PackageElement to) {
        return forRoot(from).resolve(forPackage(to));
    }

    protected DocPath(String p) {
        path = (p.endsWith("/") ? p.substring(0, p.length() - 1) : p);
    }

    /** {@inheritDoc} */
    @Override
    public boolean equals(Object other) {
        return (other instanceof DocPath) && path.equals(((DocPath)other).path);
    }

    /** {@inheritDoc} */
    @Override
    public int hashCode() {
        return path.hashCode();
    }

    public DocPath basename() {
        int sep = path.lastIndexOf("/");
        return (sep == -1) ? this : new DocPath(path.substring(sep + 1));
    }

    public DocPath parent() {
        int sep = path.lastIndexOf("/");
        return (sep == -1) ? empty : new DocPath(path.substring(0, sep));
    }

    /**
     * Returns the path formed by appending the specified string to the current path.
     * @param p the string
     * @return the path
     */
    public DocPath resolve(String p) {
        if (p == null || p.isEmpty())
            return this;
        if (path.isEmpty())
            return new DocPath(p);
        return new DocPath(path + "/" + p);
    }

    /**
     * Returns the path by appending the specified path to the current path.
     * @param p the path
     * @return the path
     */
    public DocPath resolve(DocPath p) {
        if (p == null || p.isEmpty())
            return this;
        if (path.isEmpty())
            return p;
        return new DocPath(path + "/" + p.getPath());
    }

    /**
     * Return the inverse path for this path.
     * For example, if the path is a/b/c, the inverse path is ../../..
     * @return the path
     */
    public DocPath invert() {
        return new DocPath(path.replaceAll("[^/]+", ".."));
    }

    /**
     * Returns the path formed by eliminating empty components,
     * '.' components, and redundant name/.. components.
     * @return the path
     */
    public DocPath normalize() {
        return path.isEmpty()
                ? this
                : new DocPath(String.join("/", normalize(path)));
    }

    private static List<String> normalize(String path) {
        return normalize(Arrays.asList(path.split("/")));
    }

    private static List<String> normalize(List<String> parts) {
        if (parts.stream().noneMatch(s -> s.isEmpty() || s.equals(".") || s.equals(".."))) {
            return parts;
        }
        List<String> normalized = new ArrayList<>();
        for (String part : parts) {
            switch (part) {
                case "":
                case ".":
                    break;
                case "..":
                    int n = normalized.size();
                    if (n > 0 && !normalized.get(n - 1).equals("..")) {
                        normalized.remove(n - 1);
                    } else {
                        normalized.add(part);
                    }
                    break;
                default:
                    normalized.add(part);
            }
        }
        return normalized;
    }

    /**
     * Normalize and relativize a path against this path,
     * assuming that this path is for a file (not a directory),
     * in which the other path will appear.
     *
     * @param other the path to be relativized.
     * @return the simplified path
     */
    public DocPath relativize(DocPath other) {
        if (other == null || other.path.isEmpty()) {
            return this;
        }

        if (path.isEmpty()) {
            return other;
        }

        List<String> originParts = normalize(path);
        int sep = path.lastIndexOf("/");
        List<String> destParts = sep == -1
                ? normalize(other.path)
                : normalize(path.substring(0, sep + 1) + other.path);
        int common = 0;
        while (common < originParts.size()
                && common < destParts.size()
                && originParts.get(common).equals(destParts.get(common))) {
            common++;
        }

        List<String> newParts;
        if (common == originParts.size()) {
            newParts = destParts.subList(common, destParts.size());
        } else {
            newParts = new ArrayList<>();
            newParts.addAll(Collections.nCopies(originParts.size() - common - 1, ".."));
            newParts.addAll(destParts.subList(common, destParts.size()));
        }
        return new DocPath(String.join("/", newParts));
    }

    /**
     * Return true if this path is empty.
     * @return true if this path is empty
     */
    public boolean isEmpty() {
        return path.isEmpty();
    }

    /**
     * Creates a DocLink formed from this path and a fragment identifier.
     * @param fragment the fragment
     * @return the link
     */
    public DocLink fragment(String fragment) {
        return new DocLink(path, null, fragment);
    }

    /**
     * Creates a DocLink formed from this path and a query string.
     * @param query the query string
     * @return the link
     */
    public DocLink query(String query) {
        return new DocLink(path, query, null);
    }

    /**
     * Returns this path as a string.
     * @return the path
     */
    // This is provided instead of using toString() to help catch
    // unintended use of toString() in string concatenation sequences.
    public String getPath() {
        return path;
    }
}
