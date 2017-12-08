/*
 * Copyright (c) 1999, 2017, Oracle and/or its affiliates. All rights reserved.
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

package com.sun.tools.javac.jvm;

import java.io.*;
import java.net.URI;
import java.net.URISyntaxException;
import java.nio.CharBuffer;
import java.nio.file.ClosedFileSystemException;
import java.util.Arrays;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

import javax.lang.model.element.Modifier;
import javax.lang.model.element.NestingKind;
import javax.tools.JavaFileManager;
import javax.tools.JavaFileObject;

import com.sun.tools.javac.comp.Annotate;
import com.sun.tools.javac.comp.Annotate.AnnotationTypeCompleter;
import com.sun.tools.javac.code.*;
import com.sun.tools.javac.code.Directive.*;
import com.sun.tools.javac.code.Lint.LintCategory;
import com.sun.tools.javac.code.Scope.WriteableScope;
import com.sun.tools.javac.code.Symbol.*;
import com.sun.tools.javac.code.Symtab;
import com.sun.tools.javac.code.Type.*;
import com.sun.tools.javac.comp.Annotate.AnnotationTypeMetadata;
import com.sun.tools.javac.file.BaseFileManager;
import com.sun.tools.javac.file.PathFileObject;
import com.sun.tools.javac.jvm.ClassFile.NameAndType;
import com.sun.tools.javac.jvm.ClassFile.Version;
import com.sun.tools.javac.main.Option;
import com.sun.tools.javac.resources.CompilerProperties.Fragments;
import com.sun.tools.javac.resources.CompilerProperties.Warnings;
import com.sun.tools.javac.util.*;
import com.sun.tools.javac.util.DefinedBy.Api;
import com.sun.tools.javac.util.JCDiagnostic.DiagnosticPosition;

import static com.sun.tools.javac.code.Flags.*;
import static com.sun.tools.javac.code.Kinds.Kind.*;

import com.sun.tools.javac.code.Scope.LookupKind;

import static com.sun.tools.javac.code.TypeTag.ARRAY;
import static com.sun.tools.javac.code.TypeTag.CLASS;
import static com.sun.tools.javac.code.TypeTag.TYPEVAR;
import static com.sun.tools.javac.jvm.ClassFile.*;
import static com.sun.tools.javac.jvm.ClassFile.Version.*;

import static com.sun.tools.javac.main.Option.PARAMETERS;

/** This class provides operations to read a classfile into an internal
 *  representation. The internal representation is anchored in a
 *  ClassSymbol which contains in its scope symbol representations
 *  for all other definitions in the classfile. Top-level Classes themselves
 *  appear as members of the scopes of PackageSymbols.
 *
 *  <p><b>This is NOT part of any supported API.
 *  If you write code that depends on this, you do so at your own risk.
 *  This code and its internal interfaces are subject to change or
 *  deletion without notice.</b>
 */
public class ClassReader {
    /** The context key for the class reader. */
    protected static final Context.Key<ClassReader> classReaderKey = new Context.Key<>();

    public static final int INITIAL_BUFFER_SIZE = 0x0fff0;

    private final Annotate annotate;

    /** Switch: verbose output.
     */
    boolean verbose;

    /** Switch: read constant pool and code sections. This switch is initially
     *  set to false but can be turned on from outside.
     */
    public boolean readAllOfClassFile = false;

    /** Switch: allow simplified varargs.
     */
    boolean allowSimplifiedVarargs;

    /** Switch: allow modules.
     */
    boolean allowModules;

   /** Lint option: warn about classfile issues
     */
    boolean lintClassfile;

    /** Switch: preserve parameter names from the variable table.
     */
    public boolean saveParameterNames;

    /**
     * The currently selected profile.
     */
    public final Profile profile;

    /** The log to use for verbose output
     */
    final Log log;

    /** The symbol table. */
    Symtab syms;

    Types types;

    /** The name table. */
    final Names names;

    /** Access to files
     */
    private final JavaFileManager fileManager;

    /** Factory for diagnostics
     */
    JCDiagnostic.Factory diagFactory;

    /** The current scope where type variables are entered.
     */
    protected WriteableScope typevars;

    private List<InterimUsesDirective> interimUses = List.nil();
    private List<InterimProvidesDirective> interimProvides = List.nil();

    /** The path name of the class file currently being read.
     */
    protected JavaFileObject currentClassFile = null;

    /** The class or method currently being read.
     */
    protected Symbol currentOwner = null;

    /** The module containing the class currently being read.
     */
    protected ModuleSymbol currentModule = null;

    /** The buffer containing the currently read class file.
     */
    byte[] buf = new byte[INITIAL_BUFFER_SIZE];

    /** The current input pointer.
     */
    protected int bp;

    /** The objects of the constant pool.
     */
    Object[] poolObj;

    /** For every constant pool entry, an index into buf where the
     *  defining section of the entry is found.
     */
    int[] poolIdx;

    /** The major version number of the class file being read. */
    int majorVersion;
    /** The minor version number of the class file being read. */
    int minorVersion;

    /** A table to hold the constant pool indices for method parameter
     * names, as given in LocalVariableTable attributes.
     */
    int[] parameterNameIndices;

    /**
     * Whether or not any parameter names have been found.
     */
    boolean haveParameterNameIndices;

    /** Set this to false every time we start reading a method
     * and are saving parameter names.  Set it to true when we see
     * MethodParameters, if it's set when we see a LocalVariableTable,
     * then we ignore the parameter names from the LVT.
     */
    boolean sawMethodParameters;

    /**
     * The set of attribute names for which warnings have been generated for the current class
     */
    Set<Name> warnedAttrs = new HashSet<>();

    /**
     * The prototype @Target Attribute.Compound if this class is an annotation annotated with
     * @Target
     */
    CompoundAnnotationProxy target;

    /**
     * The prototype @Repetable Attribute.Compound if this class is an annotation annotated with
     * @Repeatable
     */
    CompoundAnnotationProxy repeatable;

    /** Get the ClassReader instance for this invocation. */
    public static ClassReader instance(Context context) {
        ClassReader instance = context.get(classReaderKey);
        if (instance == null)
            instance = new ClassReader(context);
        return instance;
    }

    /** Construct a new class reader. */
    protected ClassReader(Context context) {
        context.put(classReaderKey, this);
        annotate = Annotate.instance(context);
        names = Names.instance(context);
        syms = Symtab.instance(context);
        types = Types.instance(context);
        fileManager = context.get(JavaFileManager.class);
        if (fileManager == null)
            throw new AssertionError("FileManager initialization error");
        diagFactory = JCDiagnostic.Factory.instance(context);

        log = Log.instance(context);

        Options options = Options.instance(context);
        verbose         = options.isSet(Option.VERBOSE);

        Source source = Source.instance(context);
        allowSimplifiedVarargs = source.allowSimplifiedVarargs();
        allowModules     = source.allowModules();

        saveParameterNames = options.isSet(PARAMETERS);

        profile = Profile.instance(context);

        typevars = WriteableScope.create(syms.noSymbol);

        lintClassfile = Lint.instance(context).isEnabled(LintCategory.CLASSFILE);

        initAttributeReaders();
    }

    /** Add member to class unless it is synthetic.
     */
    private void enterMember(ClassSymbol c, Symbol sym) {
        // Synthetic members are not entered -- reason lost to history (optimization?).
        // Lambda methods must be entered because they may have inner classes (which reference them)
        if ((sym.flags_field & (SYNTHETIC|BRIDGE)) != SYNTHETIC || sym.name.startsWith(names.lambda))
            c.members_field.enter(sym);
    }

/************************************************************************
 * Error Diagnoses
 ***********************************************************************/

    public ClassFinder.BadClassFile badClassFile(String key, Object... args) {
        return new ClassFinder.BadClassFile (
            currentOwner.enclClass(),
            currentClassFile,
            diagFactory.fragment(key, args),
            diagFactory);
    }

    public ClassFinder.BadEnclosingMethodAttr badEnclosingMethod(Symbol sym) {
        return new ClassFinder.BadEnclosingMethodAttr (
            currentOwner.enclClass(),
            currentClassFile,
            diagFactory.fragment(Fragments.BadEnclosingMethod(sym)),
            diagFactory);
    }

/************************************************************************
 * Buffer Access
 ***********************************************************************/

    /** Read a character.
     */
    char nextChar() {
        return (char)(((buf[bp++] & 0xFF) << 8) + (buf[bp++] & 0xFF));
    }

    /** Read a byte.
     */
    int nextByte() {
        return buf[bp++] & 0xFF;
    }

    /** Read an integer.
     */
    int nextInt() {
        return
            ((buf[bp++] & 0xFF) << 24) +
            ((buf[bp++] & 0xFF) << 16) +
            ((buf[bp++] & 0xFF) << 8) +
            (buf[bp++] & 0xFF);
    }

    /** Extract a character at position bp from buf.
     */
    char getChar(int bp) {
        return
            (char)(((buf[bp] & 0xFF) << 8) + (buf[bp+1] & 0xFF));
    }

    /** Extract an integer at position bp from buf.
     */
    int getInt(int bp) {
        return
            ((buf[bp] & 0xFF) << 24) +
            ((buf[bp+1] & 0xFF) << 16) +
            ((buf[bp+2] & 0xFF) << 8) +
            (buf[bp+3] & 0xFF);
    }


    /** Extract a long integer at position bp from buf.
     */
    long getLong(int bp) {
        DataInputStream bufin =
            new DataInputStream(new ByteArrayInputStream(buf, bp, 8));
        try {
            return bufin.readLong();
        } catch (IOException e) {
            throw new AssertionError(e);
        }
    }

    /** Extract a float at position bp from buf.
     */
    float getFloat(int bp) {
        DataInputStream bufin =
            new DataInputStream(new ByteArrayInputStream(buf, bp, 4));
        try {
            return bufin.readFloat();
        } catch (IOException e) {
            throw new AssertionError(e);
        }
    }

    /** Extract a double at position bp from buf.
     */
    double getDouble(int bp) {
        DataInputStream bufin =
            new DataInputStream(new ByteArrayInputStream(buf, bp, 8));
        try {
            return bufin.readDouble();
        } catch (IOException e) {
            throw new AssertionError(e);
        }
    }

/************************************************************************
 * Constant Pool Access
 ***********************************************************************/

    /** Index all constant pool entries, writing their start addresses into
     *  poolIdx.
     */
    void indexPool() {
        poolIdx = new int[nextChar()];
        poolObj = new Object[poolIdx.length];
        int i = 1;
        while (i < poolIdx.length) {
            poolIdx[i++] = bp;
            byte tag = buf[bp++];
            switch (tag) {
            case CONSTANT_Utf8: case CONSTANT_Unicode: {
                int len = nextChar();
                bp = bp + len;
                break;
            }
            case CONSTANT_Class:
            case CONSTANT_String:
            case CONSTANT_MethodType:
            case CONSTANT_Module:
            case CONSTANT_Package:
                bp = bp + 2;
                break;
            case CONSTANT_MethodHandle:
                bp = bp + 3;
                break;
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref:
            case CONSTANT_NameandType:
            case CONSTANT_Integer:
            case CONSTANT_Float:
            case CONSTANT_InvokeDynamic:
                bp = bp + 4;
                break;
            case CONSTANT_Long:
            case CONSTANT_Double:
                bp = bp + 8;
                i++;
                break;
            default:
                throw badClassFile("bad.const.pool.tag.at",
                                   Byte.toString(tag),
                                   Integer.toString(bp -1));
            }
        }
    }

    /** Read constant pool entry at start address i, use pool as a cache.
     */
    Object readPool(int i) {
        Object result = poolObj[i];
        if (result != null) return result;

        int index = poolIdx[i];
        if (index == 0) return null;

        byte tag = buf[index];
        switch (tag) {
        case CONSTANT_Utf8:
            poolObj[i] = names.fromUtf(buf, index + 3, getChar(index + 1));
            break;
        case CONSTANT_Unicode:
            throw badClassFile("unicode.str.not.supported");
        case CONSTANT_Class:
            poolObj[i] = readClassOrType(getChar(index + 1));
            break;
        case CONSTANT_String:
            // FIXME: (footprint) do not use toString here
            poolObj[i] = readName(getChar(index + 1)).toString();
            break;
        case CONSTANT_Fieldref: {
            ClassSymbol owner = readClassSymbol(getChar(index + 1));
            NameAndType nt = readNameAndType(getChar(index + 3));
            poolObj[i] = new VarSymbol(0, nt.name, nt.uniqueType.type, owner);
            break;
        }
        case CONSTANT_Methodref:
        case CONSTANT_InterfaceMethodref: {
            ClassSymbol owner = readClassSymbol(getChar(index + 1));
            NameAndType nt = readNameAndType(getChar(index + 3));
            poolObj[i] = new MethodSymbol(0, nt.name, nt.uniqueType.type, owner);
            break;
        }
        case CONSTANT_NameandType:
            poolObj[i] = new NameAndType(
                readName(getChar(index + 1)),
                readType(getChar(index + 3)), types);
            break;
        case CONSTANT_Integer:
            poolObj[i] = getInt(index + 1);
            break;
        case CONSTANT_Float:
            poolObj[i] = Float.valueOf(getFloat(index + 1));
            break;
        case CONSTANT_Long:
            poolObj[i] = Long.valueOf(getLong(index + 1));
            break;
        case CONSTANT_Double:
            poolObj[i] = Double.valueOf(getDouble(index + 1));
            break;
        case CONSTANT_MethodHandle:
            skipBytes(4);
            break;
        case CONSTANT_MethodType:
            skipBytes(3);
            break;
        case CONSTANT_InvokeDynamic:
            skipBytes(5);
            break;
        case CONSTANT_Module:
        case CONSTANT_Package:
            // this is temporary for now: treat as a simple reference to the underlying Utf8.
            poolObj[i] = readName(getChar(index + 1));
            break;
        default:
            throw badClassFile("bad.const.pool.tag", Byte.toString(tag));
        }
        return poolObj[i];
    }

    /** Read signature and convert to type.
     */
    Type readType(int i) {
        int index = poolIdx[i];
        return sigToType(buf, index + 3, getChar(index + 1));
    }

    /** If name is an array type or class signature, return the
     *  corresponding type; otherwise return a ClassSymbol with given name.
     */
    Object readClassOrType(int i) {
        int index =  poolIdx[i];
        int len = getChar(index + 1);
        int start = index + 3;
        Assert.check(buf[start] == '[' || buf[start + len - 1] != ';');
        // by the above assertion, the following test can be
        // simplified to (buf[start] == '[')
        return (buf[start] == '[' || buf[start + len - 1] == ';')
            ? (Object)sigToType(buf, start, len)
            : (Object)enterClass(names.fromUtf(internalize(buf, start,
                                                           len)));
    }

    /** Read signature and convert to type parameters.
     */
    List<Type> readTypeParams(int i) {
        int index = poolIdx[i];
        return sigToTypeParams(buf, index + 3, getChar(index + 1));
    }

    /** Read class entry.
     */
    ClassSymbol readClassSymbol(int i) {
        Object obj = readPool(i);
        if (obj != null && !(obj instanceof ClassSymbol))
            throw badClassFile("bad.const.pool.entry",
                               currentClassFile.toString(),
                               "CONSTANT_Class_info", i);
        return (ClassSymbol)obj;
    }

    Name readClassName(int i) {
        int index = poolIdx[i];
        if (index == 0) return null;
        byte tag = buf[index];
        if (tag != CONSTANT_Class) {
            throw badClassFile("bad.const.pool.entry",
                               currentClassFile.toString(),
                               "CONSTANT_Class_info", i);
        }
        int nameIndex =  poolIdx[getChar(index + 1)];
        int len = getChar(nameIndex + 1);
        int start = nameIndex + 3;
        if (buf[start] == '[' || buf[start + len - 1] == ';')
            throw badClassFile("wrong class name"); //TODO: proper diagnostics
        return names.fromUtf(internalize(buf, start, len));
    }

    /** Read name.
     */
    Name readName(int i) {
        Object obj = readPool(i);
        if (obj != null && !(obj instanceof Name))
            throw badClassFile("bad.const.pool.entry",
                               currentClassFile.toString(),
                               "CONSTANT_Utf8_info or CONSTANT_String_info", i);
        return (Name)obj;
    }

    /** Read name and type.
     */
    NameAndType readNameAndType(int i) {
        Object obj = readPool(i);
        if (obj != null && !(obj instanceof NameAndType))
            throw badClassFile("bad.const.pool.entry",
                               currentClassFile.toString(),
                               "CONSTANT_NameAndType_info", i);
        return (NameAndType)obj;
    }

    /** Read the name of a module.
     * The name is stored in a CONSTANT_Module entry, in
     * JVMS 4.2 binary form (using ".", not "/")
     */
    Name readModuleName(int i) {
        return readName(i);
    }

    /** Read module_flags.
     */
    Set<ModuleFlags> readModuleFlags(int flags) {
        Set<ModuleFlags> set = EnumSet.noneOf(ModuleFlags.class);
        for (ModuleFlags f : ModuleFlags.values()) {
            if ((flags & f.value) != 0)
                set.add(f);
        }
        return set;
    }

    /** Read resolution_flags.
     */
    Set<ModuleResolutionFlags> readModuleResolutionFlags(int flags) {
        Set<ModuleResolutionFlags> set = EnumSet.noneOf(ModuleResolutionFlags.class);
        for (ModuleResolutionFlags f : ModuleResolutionFlags.values()) {
            if ((flags & f.value) != 0)
                set.add(f);
        }
        return set;
    }

    /** Read exports_flags.
     */
    Set<ExportsFlag> readExportsFlags(int flags) {
        Set<ExportsFlag> set = EnumSet.noneOf(ExportsFlag.class);
        for (ExportsFlag f: ExportsFlag.values()) {
            if ((flags & f.value) != 0)
                set.add(f);
        }
        return set;
    }

    /** Read opens_flags.
     */
    Set<OpensFlag> readOpensFlags(int flags) {
        Set<OpensFlag> set = EnumSet.noneOf(OpensFlag.class);
        for (OpensFlag f: OpensFlag.values()) {
            if ((flags & f.value) != 0)
                set.add(f);
        }
        return set;
    }

    /** Read requires_flags.
     */
    Set<RequiresFlag> readRequiresFlags(int flags) {
        Set<RequiresFlag> set = EnumSet.noneOf(RequiresFlag.class);
        for (RequiresFlag f: RequiresFlag.values()) {
            if ((flags & f.value) != 0)
                set.add(f);
        }
        return set;
    }

/************************************************************************
 * Reading Types
 ***********************************************************************/

    /** The unread portion of the currently read type is
     *  signature[sigp..siglimit-1].
     */
    byte[] signature;
    int sigp;
    int siglimit;
    boolean sigEnterPhase = false;

    /** Convert signature to type, where signature is a byte array segment.
     */
    Type sigToType(byte[] sig, int offset, int len) {
        signature = sig;
        sigp = offset;
        siglimit = offset + len;
        return sigToType();
    }

    /** Convert signature to type, where signature is implicit.
     */
    Type sigToType() {
        switch ((char) signature[sigp]) {
        case 'T':
            sigp++;
            int start = sigp;
            while (signature[sigp] != ';') sigp++;
            sigp++;
            return sigEnterPhase
                ? Type.noType
                : findTypeVar(names.fromUtf(signature, start, sigp - 1 - start));
        case '+': {
            sigp++;
            Type t = sigToType();
            return new WildcardType(t, BoundKind.EXTENDS, syms.boundClass);
        }
        case '*':
            sigp++;
            return new WildcardType(syms.objectType, BoundKind.UNBOUND,
                                    syms.boundClass);
        case '-': {
            sigp++;
            Type t = sigToType();
            return new WildcardType(t, BoundKind.SUPER, syms.boundClass);
        }
        case 'B':
            sigp++;
            return syms.byteType;
        case 'C':
            sigp++;
            return syms.charType;
        case 'D':
            sigp++;
            return syms.doubleType;
        case 'F':
            sigp++;
            return syms.floatType;
        case 'I':
            sigp++;
            return syms.intType;
        case 'J':
            sigp++;
            return syms.longType;
        case 'L':
            {
                // int oldsigp = sigp;
                Type t = classSigToType();
                if (sigp < siglimit && signature[sigp] == '.')
                    throw badClassFile("deprecated inner class signature syntax " +
                                       "(please recompile from source)");
                /*
                System.err.println(" decoded " +
                                   new String(signature, oldsigp, sigp-oldsigp) +
                                   " => " + t + " outer " + t.outer());
                */
                return t;
            }
        case 'S':
            sigp++;
            return syms.shortType;
        case 'V':
            sigp++;
            return syms.voidType;
        case 'Z':
            sigp++;
            return syms.booleanType;
        case '[':
            sigp++;
            return new ArrayType(sigToType(), syms.arrayClass);
        case '(':
            sigp++;
            List<Type> argtypes = sigToTypes(')');
            Type restype = sigToType();
            List<Type> thrown = List.nil();
            while (signature[sigp] == '^') {
                sigp++;
                thrown = thrown.prepend(sigToType());
            }
            // if there is a typevar in the throws clause we should state it.
            for (List<Type> l = thrown; l.nonEmpty(); l = l.tail) {
                if (l.head.hasTag(TYPEVAR)) {
                    l.head.tsym.flags_field |= THROWS;
                }
            }
            return new MethodType(argtypes,
                                  restype,
                                  thrown.reverse(),
                                  syms.methodClass);
        case '<':
            typevars = typevars.dup(currentOwner);
            Type poly = new ForAll(sigToTypeParams(), sigToType());
            typevars = typevars.leave();
            return poly;
        default:
            throw badClassFile("bad.signature",
                               Convert.utf2string(signature, sigp, 10));
        }
    }

    byte[] signatureBuffer = new byte[0];
    int sbp = 0;
    /** Convert class signature to type, where signature is implicit.
     */
    Type classSigToType() {
        if (signature[sigp] != 'L')
            throw badClassFile("bad.class.signature",
                               Convert.utf2string(signature, sigp, 10));
        sigp++;
        Type outer = Type.noType;
        int startSbp = sbp;

        while (true) {
            final byte c = signature[sigp++];
            switch (c) {

            case ';': {         // end
                ClassSymbol t = enterClass(names.fromUtf(signatureBuffer,
                                                         startSbp,
                                                         sbp - startSbp));

                try {
                    return (outer == Type.noType) ?
                            t.erasure(types) :
                        new ClassType(outer, List.nil(), t);
                } finally {
                    sbp = startSbp;
                }
            }

            case '<':           // generic arguments
                ClassSymbol t = enterClass(names.fromUtf(signatureBuffer,
                                                         startSbp,
                                                         sbp - startSbp));
                outer = new ClassType(outer, sigToTypes('>'), t) {
                        boolean completed = false;
                        @Override @DefinedBy(Api.LANGUAGE_MODEL)
                        public Type getEnclosingType() {
                            if (!completed) {
                                completed = true;
                                tsym.complete();
                                Type enclosingType = tsym.type.getEnclosingType();
                                if (enclosingType != Type.noType) {
                                    List<Type> typeArgs =
                                        super.getEnclosingType().allparams();
                                    List<Type> typeParams =
                                        enclosingType.allparams();
                                    if (typeParams.length() != typeArgs.length()) {
                                        // no "rare" types
                                        super.setEnclosingType(types.erasure(enclosingType));
                                    } else {
                                        super.setEnclosingType(types.subst(enclosingType,
                                                                           typeParams,
                                                                           typeArgs));
                                    }
                                } else {
                                    super.setEnclosingType(Type.noType);
                                }
                            }
                            return super.getEnclosingType();
                        }
                        @Override
                        public void setEnclosingType(Type outer) {
                            throw new UnsupportedOperationException();
                        }
                    };
                switch (signature[sigp++]) {
                case ';':
                    if (sigp < signature.length && signature[sigp] == '.') {
                        // support old-style GJC signatures
                        // The signature produced was
                        // Lfoo/Outer<Lfoo/X;>;.Lfoo/Outer$Inner<Lfoo/Y;>;
                        // rather than say
                        // Lfoo/Outer<Lfoo/X;>.Inner<Lfoo/Y;>;
                        // so we skip past ".Lfoo/Outer$"
                        sigp += (sbp - startSbp) + // "foo/Outer"
                            3;  // ".L" and "$"
                        signatureBuffer[sbp++] = (byte)'$';
                        break;
                    } else {
                        sbp = startSbp;
                        return outer;
                    }
                case '.':
                    signatureBuffer[sbp++] = (byte)'$';
                    break;
                default:
                    throw new AssertionError(signature[sigp-1]);
                }
                continue;

            case '.':
                //we have seen an enclosing non-generic class
                if (outer != Type.noType) {
                    t = enterClass(names.fromUtf(signatureBuffer,
                                                 startSbp,
                                                 sbp - startSbp));
                    outer = new ClassType(outer, List.nil(), t);
                }
                signatureBuffer[sbp++] = (byte)'$';
                continue;
            case '/':
                signatureBuffer[sbp++] = (byte)'.';
                continue;
            default:
                signatureBuffer[sbp++] = c;
                continue;
            }
        }
    }

    /** Convert (implicit) signature to list of types
     *  until `terminator' is encountered.
     */
    List<Type> sigToTypes(char terminator) {
        List<Type> head = List.of(null);
        List<Type> tail = head;
        while (signature[sigp] != terminator)
            tail = tail.setTail(List.of(sigToType()));
        sigp++;
        return head.tail;
    }

    /** Convert signature to type parameters, where signature is a byte
     *  array segment.
     */
    List<Type> sigToTypeParams(byte[] sig, int offset, int len) {
        signature = sig;
        sigp = offset;
        siglimit = offset + len;
        return sigToTypeParams();
    }

    /** Convert signature to type parameters, where signature is implicit.
     */
    List<Type> sigToTypeParams() {
        List<Type> tvars = List.nil();
        if (signature[sigp] == '<') {
            sigp++;
            int start = sigp;
            sigEnterPhase = true;
            while (signature[sigp] != '>')
                tvars = tvars.prepend(sigToTypeParam());
            sigEnterPhase = false;
            sigp = start;
            while (signature[sigp] != '>')
                sigToTypeParam();
            sigp++;
        }
        return tvars.reverse();
    }

    /** Convert (implicit) signature to type parameter.
     */
    Type sigToTypeParam() {
        int start = sigp;
        while (signature[sigp] != ':') sigp++;
        Name name = names.fromUtf(signature, start, sigp - start);
        TypeVar tvar;
        if (sigEnterPhase) {
            tvar = new TypeVar(name, currentOwner, syms.botType);
            typevars.enter(tvar.tsym);
        } else {
            tvar = (TypeVar)findTypeVar(name);
        }
        List<Type> bounds = List.nil();
        boolean allInterfaces = false;
        if (signature[sigp] == ':' && signature[sigp+1] == ':') {
            sigp++;
            allInterfaces = true;
        }
        while (signature[sigp] == ':') {
            sigp++;
            bounds = bounds.prepend(sigToType());
        }
        if (!sigEnterPhase) {
            types.setBounds(tvar, bounds.reverse(), allInterfaces);
        }
        return tvar;
    }

    /** Find type variable with given name in `typevars' scope.
     */
    Type findTypeVar(Name name) {
        Symbol s = typevars.findFirst(name);
        if (s != null) {
            return s.type;
        } else {
            if (readingClassAttr) {
                // While reading the class attribute, the supertypes
                // might refer to a type variable from an enclosing element
                // (method or class).
                // If the type variable is defined in the enclosing class,
                // we can actually find it in
                // currentOwner.owner.type.getTypeArguments()
                // However, until we have read the enclosing method attribute
                // we don't know for sure if this owner is correct.  It could
                // be a method and there is no way to tell before reading the
                // enclosing method attribute.
                TypeVar t = new TypeVar(name, currentOwner, syms.botType);
                missingTypeVariables = missingTypeVariables.prepend(t);
                // System.err.println("Missing type var " + name);
                return t;
            }
            throw badClassFile("undecl.type.var", name);
        }
    }

/************************************************************************
 * Reading Attributes
 ***********************************************************************/

    protected enum AttributeKind { CLASS, MEMBER }

    protected abstract class AttributeReader {
        protected AttributeReader(Name name, ClassFile.Version version, Set<AttributeKind> kinds) {
            this.name = name;
            this.version = version;
            this.kinds = kinds;
        }

        protected boolean accepts(AttributeKind kind) {
            if (kinds.contains(kind)) {
                if (majorVersion > version.major || (majorVersion == version.major && minorVersion >= version.minor))
                    return true;

                if (lintClassfile && !warnedAttrs.contains(name)) {
                    JavaFileObject prev = log.useSource(currentClassFile);
                    try {
                        log.warning(LintCategory.CLASSFILE, (DiagnosticPosition) null,
                                    Warnings.FutureAttr(name, version.major, version.minor, majorVersion, minorVersion));
                    } finally {
                        log.useSource(prev);
                    }
                    warnedAttrs.add(name);
                }
            }
            return false;
        }

        protected abstract void read(Symbol sym, int attrLen);

        protected final Name name;
        protected final ClassFile.Version version;
        protected final Set<AttributeKind> kinds;
    }

    protected Set<AttributeKind> CLASS_ATTRIBUTE =
            EnumSet.of(AttributeKind.CLASS);
    protected Set<AttributeKind> MEMBER_ATTRIBUTE =
            EnumSet.of(AttributeKind.MEMBER);
    protected Set<AttributeKind> CLASS_OR_MEMBER_ATTRIBUTE =
            EnumSet.of(AttributeKind.CLASS, AttributeKind.MEMBER);

    protected Map<Name, AttributeReader> attributeReaders = new HashMap<>();

    private void initAttributeReaders() {
        AttributeReader[] readers = {
            // v45.3 attributes

            new AttributeReader(names.Code, V45_3, MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    if (readAllOfClassFile || saveParameterNames)
                        ((MethodSymbol)sym).code = readCode(sym);
                    else
                        bp = bp + attrLen;
                }
            },

            new AttributeReader(names.ConstantValue, V45_3, MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    Object v = readPool(nextChar());
                    // Ignore ConstantValue attribute if field not final.
                    if ((sym.flags() & FINAL) == 0) {
                        return;
                    }
                    VarSymbol var = (VarSymbol) sym;
                    switch (var.type.getTag()) {
                       case BOOLEAN:
                       case BYTE:
                       case CHAR:
                       case SHORT:
                       case INT:
                           checkType(var, Integer.class, v);
                           break;
                       case LONG:
                           checkType(var, Long.class, v);
                           break;
                       case FLOAT:
                           checkType(var, Float.class, v);
                           break;
                       case DOUBLE:
                           checkType(var, Double.class, v);
                           break;
                       case CLASS:
                           Assert.check(var.type.tsym == syms.stringType.tsym);
                           checkType(var, String.class, v);
                           break;
                       default:
                           // ignore ConstantValue attribute if type is not primitive or String
                           return;
                    }
                    if (v instanceof Integer && !var.type.getTag().checkRange((Integer) v)) {
                        throw badClassFile("bad.constant.range", v, var, var.type);
                    }
                    var.setData(v);
                }

                void checkType(Symbol var, Class<?> clazz, Object value) {
                    if (!clazz.isInstance(value)) {
                        throw badClassFile("bad.constant.value", value, var, clazz.getSimpleName());
                    }
                }
            },

            new AttributeReader(names.Deprecated, V45_3, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    Symbol s = sym.owner.kind == MDL ? sym.owner : sym;

                    s.flags_field |= DEPRECATED;
                }
            },

            new AttributeReader(names.Exceptions, V45_3, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    int nexceptions = nextChar();
                    List<Type> thrown = List.nil();
                    for (int j = 0; j < nexceptions; j++)
                        thrown = thrown.prepend(readClassSymbol(nextChar()).type);
                    if (sym.type.getThrownTypes().isEmpty())
                        sym.type.asMethodType().thrown = thrown.reverse();
                }
            },

            new AttributeReader(names.InnerClasses, V45_3, CLASS_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    ClassSymbol c = (ClassSymbol) sym;
                    if (currentModule.module_info == c) {
                        //prevent entering the classes too soon:
                        skipInnerClasses();
                    } else {
                        readInnerClasses(c);
                    }
                }
            },

            new AttributeReader(names.LocalVariableTable, V45_3, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    int newbp = bp + attrLen;
                    if (saveParameterNames && !sawMethodParameters) {
                        // Pick up parameter names from the variable table.
                        // Parameter names are not explicitly identified as such,
                        // but all parameter name entries in the LocalVariableTable
                        // have a start_pc of 0.  Therefore, we record the name
                        // indicies of all slots with a start_pc of zero in the
                        // parameterNameIndicies array.
                        // Note that this implicitly honors the JVMS spec that
                        // there may be more than one LocalVariableTable, and that
                        // there is no specified ordering for the entries.
                        int numEntries = nextChar();
                        for (int i = 0; i < numEntries; i++) {
                            int start_pc = nextChar();
                            int length = nextChar();
                            int nameIndex = nextChar();
                            int sigIndex = nextChar();
                            int register = nextChar();
                            if (start_pc == 0) {
                                // ensure array large enough
                                if (register >= parameterNameIndices.length) {
                                    int newSize =
                                            Math.max(register + 1, parameterNameIndices.length + 8);
                                    parameterNameIndices =
                                            Arrays.copyOf(parameterNameIndices, newSize);
                                }
                                parameterNameIndices[register] = nameIndex;
                                haveParameterNameIndices = true;
                            }
                        }
                    }
                    bp = newbp;
                }
            },

            new AttributeReader(names.SourceFile, V45_3, CLASS_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    ClassSymbol c = (ClassSymbol) sym;
                    Name n = readName(nextChar());
                    c.sourcefile = new SourceFileObject(n, c.flatname);
                    // If the class is a toplevel class, originating from a Java source file,
                    // but the class name does not match the file name, then it is
                    // an auxiliary class.
                    String sn = n.toString();
                    if (c.owner.kind == PCK &&
                        sn.endsWith(".java") &&
                        !sn.equals(c.name.toString()+".java")) {
                        c.flags_field |= AUXILIARY;
                    }
                }
            },

            new AttributeReader(names.Synthetic, V45_3, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    sym.flags_field |= SYNTHETIC;
                }
            },

            // standard v49 attributes

            new AttributeReader(names.EnclosingMethod, V49, CLASS_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    int newbp = bp + attrLen;
                    readEnclosingMethodAttr(sym);
                    bp = newbp;
                }
            },

            new AttributeReader(names.Signature, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    if (sym.kind == TYP) {
                        ClassSymbol c = (ClassSymbol) sym;
                        readingClassAttr = true;
                        try {
                            ClassType ct1 = (ClassType)c.type;
                            Assert.check(c == currentOwner);
                            ct1.typarams_field = readTypeParams(nextChar());
                            ct1.supertype_field = sigToType();
                            ListBuffer<Type> is = new ListBuffer<>();
                            while (sigp != siglimit) is.append(sigToType());
                            ct1.interfaces_field = is.toList();
                        } finally {
                            readingClassAttr = false;
                        }
                    } else {
                        List<Type> thrown = sym.type.getThrownTypes();
                        sym.type = readType(nextChar());
                        //- System.err.println(" # " + sym.type);
                        if (sym.kind == MTH && sym.type.getThrownTypes().isEmpty())
                            sym.type.asMethodType().thrown = thrown;

                    }
                }
            },

            // v49 annotation attributes

            new AttributeReader(names.AnnotationDefault, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    attachAnnotationDefault(sym);
                }
            },

            new AttributeReader(names.RuntimeInvisibleAnnotations, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    attachAnnotations(sym);
                }
            },

            new AttributeReader(names.RuntimeInvisibleParameterAnnotations, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    attachParameterAnnotations(sym);
                }
            },

            new AttributeReader(names.RuntimeVisibleAnnotations, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    attachAnnotations(sym);
                }
            },

            new AttributeReader(names.RuntimeVisibleParameterAnnotations, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    attachParameterAnnotations(sym);
                }
            },

            // additional "legacy" v49 attributes, superceded by flags

            new AttributeReader(names.Annotation, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    sym.flags_field |= ANNOTATION;
                }
            },

            new AttributeReader(names.Bridge, V49, MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    sym.flags_field |= BRIDGE;
                }
            },

            new AttributeReader(names.Enum, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    sym.flags_field |= ENUM;
                }
            },

            new AttributeReader(names.Varargs, V49, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    sym.flags_field |= VARARGS;
                }
            },

            new AttributeReader(names.RuntimeVisibleTypeAnnotations, V52, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    attachTypeAnnotations(sym);
                }
            },

            new AttributeReader(names.RuntimeInvisibleTypeAnnotations, V52, CLASS_OR_MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrLen) {
                    attachTypeAnnotations(sym);
                }
            },

            // The following attributes for a Code attribute are not currently handled
            // StackMapTable
            // SourceDebugExtension
            // LineNumberTable
            // LocalVariableTypeTable

            // standard v52 attributes

            new AttributeReader(names.MethodParameters, V52, MEMBER_ATTRIBUTE) {
                protected void read(Symbol sym, int attrlen) {
                    int newbp = bp + attrlen;
                    if (saveParameterNames) {
                        sawMethodParameters = true;
                        int numEntries = nextByte();
                        parameterNameIndices = new int[numEntries];
                        haveParameterNameIndices = true;
                        for (int i = 0; i < numEntries; i++) {
                            int nameIndex = nextChar();
                            int flags = nextChar();
                            parameterNameIndices[i] = nameIndex;
                        }
                    }
                    bp = newbp;
                }
            },

            // standard v53 attributes

            new AttributeReader(names.Module, V53, CLASS_ATTRIBUTE) {
                @Override
                protected boolean accepts(AttributeKind kind) {
                    return super.accepts(kind) && allowModules;
                }
                protected void read(Symbol sym, int attrLen) {
                    if (sym.kind == TYP && sym.owner.kind == MDL) {
                        ModuleSymbol msym = (ModuleSymbol) sym.owner;
                        ListBuffer<Directive> directives = new ListBuffer<>();

                        Name moduleName = readModuleName(nextChar());
                        if (currentModule.name != moduleName) {
                            throw badClassFile("module.name.mismatch", moduleName, currentModule.name);
                        }

                        msym.flags.addAll(readModuleFlags(nextChar()));
                        msym.version = readName(nextChar());

                        ListBuffer<RequiresDirective> requires = new ListBuffer<>();
                        int nrequires = nextChar();
                        for (int i = 0; i < nrequires; i++) {
                            ModuleSymbol rsym = syms.enterModule(readModuleName(nextChar()));
                            Set<RequiresFlag> flags = readRequiresFlags(nextChar());
                            nextChar(); // skip compiled version
                            requires.add(new RequiresDirective(rsym, flags));
                        }
                        msym.requires = requires.toList();
                        directives.addAll(msym.requires);

                        ListBuffer<ExportsDirective> exports = new ListBuffer<>();
                        int nexports = nextChar();
                        for (int i = 0; i < nexports; i++) {
                            Name n = readName(nextChar());
                            PackageSymbol p = syms.enterPackage(currentModule, names.fromUtf(internalize(n)));
                            Set<ExportsFlag> flags = readExportsFlags(nextChar());
                            int nto = nextChar();
                            List<ModuleSymbol> to;
                            if (nto == 0) {
                                to = null;
                            } else {
                                ListBuffer<ModuleSymbol> lb = new ListBuffer<>();
                                for (int t = 0; t < nto; t++)
                                    lb.append(syms.enterModule(readModuleName(nextChar())));
                                to = lb.toList();
                            }
                            exports.add(new ExportsDirective(p, to, flags));
                        }
                        msym.exports = exports.toList();
                        directives.addAll(msym.exports);
                        ListBuffer<OpensDirective> opens = new ListBuffer<>();
                        int nopens = nextChar();
                        if (nopens != 0 && msym.flags.contains(ModuleFlags.OPEN)) {
                            throw badClassFile("module.non.zero.opens", currentModule.name);
                        }
                        for (int i = 0; i < nopens; i++) {
                            Name n = readName(nextChar());
                            PackageSymbol p = syms.enterPackage(currentModule, names.fromUtf(internalize(n)));
                            Set<OpensFlag> flags = readOpensFlags(nextChar());
                            int nto = nextChar();
                            List<ModuleSymbol> to;
                            if (nto == 0) {
                                to = null;
                            } else {
                                ListBuffer<ModuleSymbol> lb = new ListBuffer<>();
                                for (int t = 0; t < nto; t++)
                                    lb.append(syms.enterModule(readModuleName(nextChar())));
                                to = lb.toList();
                            }
                            opens.add(new OpensDirective(p, to, flags));
                        }
                        msym.opens = opens.toList();
                        directives.addAll(msym.opens);

                        msym.directives = directives.toList();

                        ListBuffer<InterimUsesDirective> uses = new ListBuffer<>();
                        int nuses = nextChar();
                        for (int i = 0; i < nuses; i++) {
                            Name srvc = readClassName(nextChar());
                            uses.add(new InterimUsesDirective(srvc));
                        }
                        interimUses = uses.toList();

                        ListBuffer<InterimProvidesDirective> provides = new ListBuffer<>();
                        int nprovides = nextChar();
                        for (int p = 0; p < nprovides; p++) {
                            Name srvc = readClassName(nextChar());
                            int nimpls = nextChar();
                            ListBuffer<Name> impls = new ListBuffer<>();
                            for (int i = 0; i < nimpls; i++) {
                                impls.append(readClassName(nextChar()));
                            provides.add(new InterimProvidesDirective(srvc, impls.toList()));
                            }
                        }
                        interimProvides = provides.toList();
                    }
                }
            },

            new AttributeReader(names.ModuleResolution, V53, CLASS_ATTRIBUTE) {
                @Override
                protected boolean accepts(AttributeKind kind) {
                    return super.accepts(kind) && allowModules;
                }
                protected void read(Symbol sym, int attrLen) {
                    if (sym.kind == TYP && sym.owner.kind == MDL) {
                        ModuleSymbol msym = (ModuleSymbol) sym.owner;
                        msym.resolutionFlags.addAll(readModuleResolutionFlags(nextChar()));
                    }
                }
            },
        };

        for (AttributeReader r: readers)
            attributeReaders.put(r.name, r);
    }

    protected void readEnclosingMethodAttr(Symbol sym) {
        // sym is a nested class with an "Enclosing Method" attribute
        // remove sym from it's current owners scope and place it in
        // the scope specified by the attribute
        sym.owner.members().remove(sym);
        ClassSymbol self = (ClassSymbol)sym;
        ClassSymbol c = readClassSymbol(nextChar());
        NameAndType nt = readNameAndType(nextChar());

        if (c.members_field == null || c.kind != TYP)
            throw badClassFile("bad.enclosing.class", self, c);

        MethodSymbol m = findMethod(nt, c.members_field, self.flags());
        if (nt != null && m == null)
            throw badEnclosingMethod(self);

        self.name = simpleBinaryName(self.flatname, c.flatname) ;
        self.owner = m != null ? m : c;
        if (self.name.isEmpty())
            self.fullname = names.empty;
        else
            self.fullname = ClassSymbol.formFullName(self.name, self.owner);

        if (m != null) {
            ((ClassType)sym.type).setEnclosingType(m.type);
        } else if ((self.flags_field & STATIC) == 0) {
            ((ClassType)sym.type).setEnclosingType(c.type);
        } else {
            ((ClassType)sym.type).setEnclosingType(Type.noType);
        }
        enterTypevars(self, self.type);
        if (!missingTypeVariables.isEmpty()) {
            ListBuffer<Type> typeVars =  new ListBuffer<>();
            for (Type typevar : missingTypeVariables) {
                typeVars.append(findTypeVar(typevar.tsym.name));
            }
            foundTypeVariables = typeVars.toList();
        } else {
            foundTypeVariables = List.nil();
        }
    }

    // See java.lang.Class
    private Name simpleBinaryName(Name self, Name enclosing) {
        String simpleBinaryName = self.toString().substring(enclosing.toString().length());
        if (simpleBinaryName.length() < 1 || simpleBinaryName.charAt(0) != '$')
            throw badClassFile("bad.enclosing.method", self);
        int index = 1;
        while (index < simpleBinaryName.length() &&
               isAsciiDigit(simpleBinaryName.charAt(index)))
            index++;
        return names.fromString(simpleBinaryName.substring(index));
    }

    private MethodSymbol findMethod(NameAndType nt, Scope scope, long flags) {
        if (nt == null)
            return null;

        MethodType type = nt.uniqueType.type.asMethodType();

        for (Symbol sym : scope.getSymbolsByName(nt.name)) {
            if (sym.kind == MTH && isSameBinaryType(sym.type.asMethodType(), type))
                return (MethodSymbol)sym;
        }

        if (nt.name != names.init)
            // not a constructor
            return null;
        if ((flags & INTERFACE) != 0)
            // no enclosing instance
            return null;
        if (nt.uniqueType.type.getParameterTypes().isEmpty())
            // no parameters
            return null;

        // A constructor of an inner class.
        // Remove the first argument (the enclosing instance)
        nt.setType(new MethodType(nt.uniqueType.type.getParameterTypes().tail,
                                 nt.uniqueType.type.getReturnType(),
                                 nt.uniqueType.type.getThrownTypes(),
                                 syms.methodClass));
        // Try searching again
        return findMethod(nt, scope, flags);
    }

    /** Similar to Types.isSameType but avoids completion */
    private boolean isSameBinaryType(MethodType mt1, MethodType mt2) {
        List<Type> types1 = types.erasure(mt1.getParameterTypes())
            .prepend(types.erasure(mt1.getReturnType()));
        List<Type> types2 = mt2.getParameterTypes().prepend(mt2.getReturnType());
        while (!types1.isEmpty() && !types2.isEmpty()) {
            if (types1.head.tsym != types2.head.tsym)
                return false;
            types1 = types1.tail;
            types2 = types2.tail;
        }
        return types1.isEmpty() && types2.isEmpty();
    }

    /**
     * Character.isDigit answers <tt>true</tt> to some non-ascii
     * digits.  This one does not.  <b>copied from java.lang.Class</b>
     */
    private static boolean isAsciiDigit(char c) {
        return '0' <= c && c <= '9';
    }

    /** Read member attributes.
     */
    void readMemberAttrs(Symbol sym) {
        readAttrs(sym, AttributeKind.MEMBER);
    }

    void readAttrs(Symbol sym, AttributeKind kind) {
        char ac = nextChar();
        for (int i = 0; i < ac; i++) {
            Name attrName = readName(nextChar());
            int attrLen = nextInt();
            AttributeReader r = attributeReaders.get(attrName);
            if (r != null && r.accepts(kind))
                r.read(sym, attrLen);
            else  {
                bp = bp + attrLen;
            }
        }
    }

    private boolean readingClassAttr = false;
    private List<Type> missingTypeVariables = List.nil();
    private List<Type> foundTypeVariables = List.nil();

    /** Read class attributes.
     */
    void readClassAttrs(ClassSymbol c) {
        readAttrs(c, AttributeKind.CLASS);
    }

    /** Read code block.
     */
    Code readCode(Symbol owner) {
        nextChar(); // max_stack
        nextChar(); // max_locals
        final int  code_length = nextInt();
        bp += code_length;
        final char exception_table_length = nextChar();
        bp += exception_table_length * 8;
        readMemberAttrs(owner);
        return null;
    }

/************************************************************************
 * Reading Java-language annotations
 ***********************************************************************/

    /** Attach annotations.
     */
    void attachAnnotations(final Symbol sym) {
        int numAttributes = nextChar();
        if (numAttributes != 0) {
            ListBuffer<CompoundAnnotationProxy> proxies = new ListBuffer<>();
            for (int i = 0; i<numAttributes; i++) {
                CompoundAnnotationProxy proxy = readCompoundAnnotation();
                if (proxy.type.tsym == syms.proprietaryType.tsym)
                    sym.flags_field |= PROPRIETARY;
                else if (proxy.type.tsym == syms.profileType.tsym) {
                    if (profile != Profile.DEFAULT) {
                        for (Pair<Name,Attribute> v: proxy.values) {
                            if (v.fst == names.value && v.snd instanceof Attribute.Constant) {
                                Attribute.Constant c = (Attribute.Constant) v.snd;
                                if (c.type == syms.intType && ((Integer) c.value) > profile.value) {
                                    sym.flags_field |= NOT_IN_PROFILE;
                                }
                            }
                        }
                    }
                } else {
                    if (proxy.type.tsym == syms.annotationTargetType.tsym) {
                        target = proxy;
                    } else if (proxy.type.tsym == syms.repeatableType.tsym) {
                        repeatable = proxy;
                    } else if (proxy.type.tsym == syms.deprecatedType.tsym) {
                        sym.flags_field |= (DEPRECATED | DEPRECATED_ANNOTATION);
                        for (Pair<Name, Attribute> v : proxy.values) {
                            if (v.fst == names.forRemoval && v.snd instanceof Attribute.Constant) {
                                Attribute.Constant c = (Attribute.Constant) v.snd;
                                if (c.type == syms.booleanType && ((Integer) c.value) != 0) {
                                    sym.flags_field |= DEPRECATED_REMOVAL;
                                }
                            }
                        }
                    }

                    proxies.append(proxy);
                }
            }
            annotate.normal(new AnnotationCompleter(sym, proxies.toList()));
        }
    }

    /** Attach parameter annotations.
     */
    void attachParameterAnnotations(final Symbol method) {
        final MethodSymbol meth = (MethodSymbol)method;
        int numParameters = buf[bp++] & 0xFF;
        List<VarSymbol> parameters = meth.params();
        int pnum = 0;
        while (parameters.tail != null) {
            attachAnnotations(parameters.head);
            parameters = parameters.tail;
            pnum++;
        }
        if (pnum != numParameters) {
            throw badClassFile("bad.runtime.invisible.param.annotations", meth);
        }
    }

    void attachTypeAnnotations(final Symbol sym) {
        int numAttributes = nextChar();
        if (numAttributes != 0) {
            ListBuffer<TypeAnnotationProxy> proxies = new ListBuffer<>();
            for (int i = 0; i < numAttributes; i++)
                proxies.append(readTypeAnnotation());
            annotate.normal(new TypeAnnotationCompleter(sym, proxies.toList()));
        }
    }

    /** Attach the default value for an annotation element.
     */
    void attachAnnotationDefault(final Symbol sym) {
        final MethodSymbol meth = (MethodSymbol)sym; // only on methods
        final Attribute value = readAttributeValue();

        // The default value is set later during annotation. It might
        // be the case that the Symbol sym is annotated _after_ the
        // repeating instances that depend on this default value,
        // because of this we set an interim value that tells us this
        // element (most likely) has a default.
        //
        // Set interim value for now, reset just before we do this
        // properly at annotate time.
        meth.defaultValue = value;
        annotate.normal(new AnnotationDefaultCompleter(meth, value));
    }

    Type readTypeOrClassSymbol(int i) {
        // support preliminary jsr175-format class files
        if (buf[poolIdx[i]] == CONSTANT_Class)
            return readClassSymbol(i).type;
        return readTypeToProxy(i);
    }
    Type readEnumType(int i) {
        // support preliminary jsr175-format class files
        int index = poolIdx[i];
        int length = getChar(index + 1);
        if (buf[index + length + 2] != ';')
            return enterClass(readName(i)).type;
        return readTypeToProxy(i);
    }
    Type readTypeToProxy(int i) {
        if (currentModule.module_info == currentOwner) {
            int index = poolIdx[i];
            return new ProxyType(Arrays.copyOfRange(buf, index + 3, index + 3 + getChar(index + 1)));
        } else {
            return readType(i);
        }
    }

    CompoundAnnotationProxy readCompoundAnnotation() {
        Type t;
        if (currentModule.module_info == currentOwner) {
            int index = poolIdx[nextChar()];
            t = new ProxyType(Arrays.copyOfRange(buf, index + 3, index + 3 + getChar(index + 1)));
        } else {
            t = readTypeOrClassSymbol(nextChar());
        }
        int numFields = nextChar();
        ListBuffer<Pair<Name,Attribute>> pairs = new ListBuffer<>();
        for (int i=0; i<numFields; i++) {
            Name name = readName(nextChar());
            Attribute value = readAttributeValue();
            pairs.append(new Pair<>(name, value));
        }
        return new CompoundAnnotationProxy(t, pairs.toList());
    }

    TypeAnnotationProxy readTypeAnnotation() {
        TypeAnnotationPosition position = readPosition();
        CompoundAnnotationProxy proxy = readCompoundAnnotation();

        return new TypeAnnotationProxy(proxy, position);
    }

    TypeAnnotationPosition readPosition() {
        int tag = nextByte(); // TargetType tag is a byte

        if (!TargetType.isValidTargetTypeValue(tag))
            throw badClassFile("bad.type.annotation.value", String.format("0x%02X", tag));

        TargetType type = TargetType.fromTargetTypeValue(tag);

        switch (type) {
        // instanceof
        case INSTANCEOF: {
            final int offset = nextChar();
            final TypeAnnotationPosition position =
                TypeAnnotationPosition.instanceOf(readTypePath());
            position.offset = offset;
            return position;
        }
        // new expression
        case NEW: {
            final int offset = nextChar();
            final TypeAnnotationPosition position =
                TypeAnnotationPosition.newObj(readTypePath());
            position.offset = offset;
            return position;
        }
        // constructor/method reference receiver
        case CONSTRUCTOR_REFERENCE: {
            final int offset = nextChar();
            final TypeAnnotationPosition position =
                TypeAnnotationPosition.constructorRef(readTypePath());
            position.offset = offset;
            return position;
        }
        case METHOD_REFERENCE: {
            final int offset = nextChar();
            final TypeAnnotationPosition position =
                TypeAnnotationPosition.methodRef(readTypePath());
            position.offset = offset;
            return position;
        }
        // local variable
        case LOCAL_VARIABLE: {
            final int table_length = nextChar();
            final int[] newLvarOffset = new int[table_length];
            final int[] newLvarLength = new int[table_length];
            final int[] newLvarIndex = new int[table_length];

            for (int i = 0; i < table_length; ++i) {
                newLvarOffset[i] = nextChar();
                newLvarLength[i] = nextChar();
                newLvarIndex[i] = nextChar();
            }

            final TypeAnnotationPosition position =
                    TypeAnnotationPosition.localVariable(readTypePath());
            position.lvarOffset = newLvarOffset;
            position.lvarLength = newLvarLength;
            position.lvarIndex = newLvarIndex;
            return position;
        }
        // resource variable
        case RESOURCE_VARIABLE: {
            final int table_length = nextChar();
            final int[] newLvarOffset = new int[table_length];
            final int[] newLvarLength = new int[table_length];
            final int[] newLvarIndex = new int[table_length];

            for (int i = 0; i < table_length; ++i) {
                newLvarOffset[i] = nextChar();
                newLvarLength[i] = nextChar();
                newLvarIndex[i] = nextChar();
            }

            final TypeAnnotationPosition position =
                    TypeAnnotationPosition.resourceVariable(readTypePath());
            position.lvarOffset = newLvarOffset;
            position.lvarLength = newLvarLength;
            position.lvarIndex = newLvarIndex;
            return position;
        }
        // exception parameter
        case EXCEPTION_PARAMETER: {
            final int exception_index = nextChar();
            final TypeAnnotationPosition position =
                TypeAnnotationPosition.exceptionParameter(readTypePath());
            position.setExceptionIndex(exception_index);
            return position;
        }
        // method receiver
        case METHOD_RECEIVER:
            return TypeAnnotationPosition.methodReceiver(readTypePath());
        // type parameter
        case CLASS_TYPE_PARAMETER: {
            final int parameter_index = nextByte();
            return TypeAnnotationPosition
                .typeParameter(readTypePath(), parameter_index);
        }
        case METHOD_TYPE_PARAMETER: {
            final int parameter_index = nextByte();
            return TypeAnnotationPosition
                .methodTypeParameter(readTypePath(), parameter_index);
        }
        // type parameter bound
        case CLASS_TYPE_PARAMETER_BOUND: {
            final int parameter_index = nextByte();
            final int bound_index = nextByte();
            return TypeAnnotationPosition
                .typeParameterBound(readTypePath(), parameter_index,
                                    bound_index);
        }
        case METHOD_TYPE_PARAMETER_BOUND: {
            final int parameter_index = nextByte();
            final int bound_index = nextByte();
            return TypeAnnotationPosition
                .methodTypeParameterBound(readTypePath(), parameter_index,
                                          bound_index);
        }
        // class extends or implements clause
        case CLASS_EXTENDS: {
            final int type_index = nextChar();
            return TypeAnnotationPosition.classExtends(readTypePath(),
                                                       type_index);
        }
        // throws
        case THROWS: {
            final int type_index = nextChar();
            return TypeAnnotationPosition.methodThrows(readTypePath(),
                                                       type_index);
        }
        // method parameter
        case METHOD_FORMAL_PARAMETER: {
            final int parameter_index = nextByte();
            return TypeAnnotationPosition.methodParameter(readTypePath(),
                                                          parameter_index);
        }
        // type cast
        case CAST: {
            final int offset = nextChar();
            final int type_index = nextByte();
            final TypeAnnotationPosition position =
                TypeAnnotationPosition.typeCast(readTypePath(), type_index);
            position.offset = offset;
            return position;
        }
        // method/constructor/reference type argument
        case CONSTRUCTOR_INVOCATION_TYPE_ARGUMENT: {
            final int offset = nextChar();
            final int type_index = nextByte();
            final TypeAnnotationPosition position = TypeAnnotationPosition
                .constructorInvocationTypeArg(readTypePath(), type_index);
            position.offset = offset;
            return position;
        }
        case METHOD_INVOCATION_TYPE_ARGUMENT: {
            final int offset = nextChar();
            final int type_index = nextByte();
            final TypeAnnotationPosition position = TypeAnnotationPosition
                .methodInvocationTypeArg(readTypePath(), type_index);
            position.offset = offset;
            return position;
        }
        case CONSTRUCTOR_REFERENCE_TYPE_ARGUMENT: {
            final int offset = nextChar();
            final int type_index = nextByte();
            final TypeAnnotationPosition position = TypeAnnotationPosition
                .constructorRefTypeArg(readTypePath(), type_index);
            position.offset = offset;
            return position;
        }
        case METHOD_REFERENCE_TYPE_ARGUMENT: {
            final int offset = nextChar();
            final int type_index = nextByte();
            final TypeAnnotationPosition position = TypeAnnotationPosition
                .methodRefTypeArg(readTypePath(), type_index);
            position.offset = offset;
            return position;
        }
        // We don't need to worry about these
        case METHOD_RETURN:
            return TypeAnnotationPosition.methodReturn(readTypePath());
        case FIELD:
            return TypeAnnotationPosition.field(readTypePath());
        case UNKNOWN:
            throw new AssertionError("jvm.ClassReader: UNKNOWN target type should never occur!");
        default:
            throw new AssertionError("jvm.ClassReader: Unknown target type for position: " + type);
        }
    }

    List<TypeAnnotationPosition.TypePathEntry> readTypePath() {
        int len = nextByte();
        ListBuffer<Integer> loc = new ListBuffer<>();
        for (int i = 0; i < len * TypeAnnotationPosition.TypePathEntry.bytesPerEntry; ++i)
            loc = loc.append(nextByte());

        return TypeAnnotationPosition.getTypePathFromBinary(loc.toList());

    }

    Attribute readAttributeValue() {
        char c = (char) buf[bp++];
        switch (c) {
        case 'B':
            return new Attribute.Constant(syms.byteType, readPool(nextChar()));
        case 'C':
            return new Attribute.Constant(syms.charType, readPool(nextChar()));
        case 'D':
            return new Attribute.Constant(syms.doubleType, readPool(nextChar()));
        case 'F':
            return new Attribute.Constant(syms.floatType, readPool(nextChar()));
        case 'I':
            return new Attribute.Constant(syms.intType, readPool(nextChar()));
        case 'J':
            return new Attribute.Constant(syms.longType, readPool(nextChar()));
        case 'S':
            return new Attribute.Constant(syms.shortType, readPool(nextChar()));
        case 'Z':
            return new Attribute.Constant(syms.booleanType, readPool(nextChar()));
        case 's':
            return new Attribute.Constant(syms.stringType, readPool(nextChar()).toString());
        case 'e':
            return new EnumAttributeProxy(readEnumType(nextChar()), readName(nextChar()));
        case 'c':
            return new ClassAttributeProxy(readTypeOrClassSymbol(nextChar()));
        case '[': {
            int n = nextChar();
            ListBuffer<Attribute> l = new ListBuffer<>();
            for (int i=0; i<n; i++)
                l.append(readAttributeValue());
            return new ArrayAttributeProxy(l.toList());
        }
        case '@':
            return readCompoundAnnotation();
        default:
            throw new AssertionError("unknown annotation tag '" + c + "'");
        }
    }

    interface ProxyVisitor extends Attribute.Visitor {
        void visitEnumAttributeProxy(EnumAttributeProxy proxy);
        void visitClassAttributeProxy(ClassAttributeProxy proxy);
        void visitArrayAttributeProxy(ArrayAttributeProxy proxy);
        void visitCompoundAnnotationProxy(CompoundAnnotationProxy proxy);
    }

    static class EnumAttributeProxy extends Attribute {
        Type enumType;
        Name enumerator;
        public EnumAttributeProxy(Type enumType, Name enumerator) {
            super(null);
            this.enumType = enumType;
            this.enumerator = enumerator;
        }
        public void accept(Visitor v) { ((ProxyVisitor)v).visitEnumAttributeProxy(this); }
        @Override @DefinedBy(Api.LANGUAGE_MODEL)
        public String toString() {
            return "/*proxy enum*/" + enumType + "." + enumerator;
        }
    }

    static class ClassAttributeProxy extends Attribute {
        Type classType;
        public ClassAttributeProxy(Type classType) {
            super(null);
            this.classType = classType;
        }
        public void accept(Visitor v) { ((ProxyVisitor)v).visitClassAttributeProxy(this); }
        @Override @DefinedBy(Api.LANGUAGE_MODEL)
        public String toString() {
            return "/*proxy class*/" + classType + ".class";
        }
    }

    static class ArrayAttributeProxy extends Attribute {
        List<Attribute> values;
        ArrayAttributeProxy(List<Attribute> values) {
            super(null);
            this.values = values;
        }
        public void accept(Visitor v) { ((ProxyVisitor)v).visitArrayAttributeProxy(this); }
        @Override @DefinedBy(Api.LANGUAGE_MODEL)
        public String toString() {
            return "{" + values + "}";
        }
    }

    /** A temporary proxy representing a compound attribute.
     */
    static class CompoundAnnotationProxy extends Attribute {
        final List<Pair<Name,Attribute>> values;
        public CompoundAnnotationProxy(Type type,
                                      List<Pair<Name,Attribute>> values) {
            super(type);
            this.values = values;
        }
        public void accept(Visitor v) { ((ProxyVisitor)v).visitCompoundAnnotationProxy(this); }
        @Override @DefinedBy(Api.LANGUAGE_MODEL)
        public String toString() {
            StringBuilder buf = new StringBuilder();
            buf.append("@");
            buf.append(type.tsym.getQualifiedName());
            buf.append("/*proxy*/{");
            boolean first = true;
            for (List<Pair<Name,Attribute>> v = values;
                 v.nonEmpty(); v = v.tail) {
                Pair<Name,Attribute> value = v.head;
                if (!first) buf.append(",");
                first = false;
                buf.append(value.fst);
                buf.append("=");
                buf.append(value.snd);
            }
            buf.append("}");
            return buf.toString();
        }
    }

    /** A temporary proxy representing a type annotation.
     */
    static class TypeAnnotationProxy {
        final CompoundAnnotationProxy compound;
        final TypeAnnotationPosition position;
        public TypeAnnotationProxy(CompoundAnnotationProxy compound,
                TypeAnnotationPosition position) {
            this.compound = compound;
            this.position = position;
        }
    }

    class AnnotationDeproxy implements ProxyVisitor {
        private ClassSymbol requestingOwner;

        AnnotationDeproxy(ClassSymbol owner) {
            this.requestingOwner = owner;
        }

        List<Attribute.Compound> deproxyCompoundList(List<CompoundAnnotationProxy> pl) {
            // also must fill in types!!!!
            ListBuffer<Attribute.Compound> buf = new ListBuffer<>();
            for (List<CompoundAnnotationProxy> l = pl; l.nonEmpty(); l=l.tail) {
                buf.append(deproxyCompound(l.head));
            }
            return buf.toList();
        }

        Attribute.Compound deproxyCompound(CompoundAnnotationProxy a) {
            Type annotationType = resolvePossibleProxyType(a.type);
            ListBuffer<Pair<Symbol.MethodSymbol,Attribute>> buf = new ListBuffer<>();
            for (List<Pair<Name,Attribute>> l = a.values;
                 l.nonEmpty();
                 l = l.tail) {
                MethodSymbol meth = findAccessMethod(annotationType, l.head.fst);
                buf.append(new Pair<>(meth, deproxy(meth.type.getReturnType(), l.head.snd)));
            }
            return new Attribute.Compound(annotationType, buf.toList());
        }

        MethodSymbol findAccessMethod(Type container, Name name) {
            CompletionFailure failure = null;
            try {
                for (Symbol sym : container.tsym.members().getSymbolsByName(name)) {
                    if (sym.kind == MTH && sym.type.getParameterTypes().length() == 0)
                        return (MethodSymbol) sym;
                }
            } catch (CompletionFailure ex) {
                failure = ex;
            }
            // The method wasn't found: emit a warning and recover
            JavaFileObject prevSource = log.useSource(requestingOwner.classfile);
            try {
                if (lintClassfile) {
                    if (failure == null) {
                        log.warning(Warnings.AnnotationMethodNotFound(container, name));
                    } else {
                        log.warning(Warnings.AnnotationMethodNotFoundReason(container,
                                                                            name,
                                                                            failure.getDetailValue()));//diagnostic, if present
                    }
                }
            } finally {
                log.useSource(prevSource);
            }
            // Construct a new method type and symbol.  Use bottom
            // type (typeof null) as return type because this type is
            // a subtype of all reference types and can be converted
            // to primitive types by unboxing.
            MethodType mt = new MethodType(List.nil(),
                                           syms.botType,
                                           List.nil(),
                                           syms.methodClass);
            return new MethodSymbol(PUBLIC | ABSTRACT, name, mt, container.tsym);
        }

        Attribute result;
        Type type;
        Attribute deproxy(Type t, Attribute a) {
            Type oldType = type;
            try {
                type = t;
                a.accept(this);
                return result;
            } finally {
                type = oldType;
            }
        }

        // implement Attribute.Visitor below

        public void visitConstant(Attribute.Constant value) {
            // assert value.type == type;
            result = value;
        }

        public void visitClass(Attribute.Class clazz) {
            result = clazz;
        }

        public void visitEnum(Attribute.Enum e) {
            throw new AssertionError(); // shouldn't happen
        }

        public void visitCompound(Attribute.Compound compound) {
            throw new AssertionError(); // shouldn't happen
        }

        public void visitArray(Attribute.Array array) {
            throw new AssertionError(); // shouldn't happen
        }

        public void visitError(Attribute.Error e) {
            throw new AssertionError(); // shouldn't happen
        }

        public void visitEnumAttributeProxy(EnumAttributeProxy proxy) {
            // type.tsym.flatName() should == proxy.enumFlatName
            Type enumType = resolvePossibleProxyType(proxy.enumType);
            TypeSymbol enumTypeSym = enumType.tsym;
            VarSymbol enumerator = null;
            CompletionFailure failure = null;
            try {
                for (Symbol sym : enumTypeSym.members().getSymbolsByName(proxy.enumerator)) {
                    if (sym.kind == VAR) {
                        enumerator = (VarSymbol)sym;
                        break;
                    }
                }
            }
            catch (CompletionFailure ex) {
                failure = ex;
            }
            if (enumerator == null) {
                if (failure != null) {
                    log.warning(Warnings.UnknownEnumConstantReason(currentClassFile,
                                                                   enumTypeSym,
                                                                   proxy.enumerator,
                                                                   failure.getDiagnostic()));
                } else {
                    log.warning(Warnings.UnknownEnumConstant(currentClassFile,
                                                             enumTypeSym,
                                                             proxy.enumerator));
                }
                result = new Attribute.Enum(enumTypeSym.type,
                        new VarSymbol(0, proxy.enumerator, syms.botType, enumTypeSym));
            } else {
                result = new Attribute.Enum(enumTypeSym.type, enumerator);
            }
        }

        @Override
        public void visitClassAttributeProxy(ClassAttributeProxy proxy) {
            Type classType = resolvePossibleProxyType(proxy.classType);
            result = new Attribute.Class(types, classType);
        }

        public void visitArrayAttributeProxy(ArrayAttributeProxy proxy) {
            int length = proxy.values.length();
            Attribute[] ats = new Attribute[length];
            Type elemtype = types.elemtype(type);
            int i = 0;
            for (List<Attribute> p = proxy.values; p.nonEmpty(); p = p.tail) {
                ats[i++] = deproxy(elemtype, p.head);
            }
            result = new Attribute.Array(type, ats);
        }

        public void visitCompoundAnnotationProxy(CompoundAnnotationProxy proxy) {
            result = deproxyCompound(proxy);
        }

        Type resolvePossibleProxyType(Type t) {
            if (t instanceof ProxyType) {
                Assert.check(requestingOwner.owner.kind == MDL);
                ModuleSymbol prevCurrentModule = currentModule;
                currentModule = (ModuleSymbol) requestingOwner.owner;
                try {
                    return ((ProxyType) t).resolve();
                } finally {
                    currentModule = prevCurrentModule;
                }
            } else {
                return t;
            }
        }
    }

    class AnnotationDefaultCompleter extends AnnotationDeproxy implements Runnable {
        final MethodSymbol sym;
        final Attribute value;
        final JavaFileObject classFile = currentClassFile;

        AnnotationDefaultCompleter(MethodSymbol sym, Attribute value) {
            super(currentOwner.kind == MTH
                    ? currentOwner.enclClass() : (ClassSymbol)currentOwner);
            this.sym = sym;
            this.value = value;
        }

        @Override
        public void run() {
            JavaFileObject previousClassFile = currentClassFile;
            try {
                // Reset the interim value set earlier in
                // attachAnnotationDefault().
                sym.defaultValue = null;
                currentClassFile = classFile;
                sym.defaultValue = deproxy(sym.type.getReturnType(), value);
            } finally {
                currentClassFile = previousClassFile;
            }
        }

        @Override
        public String toString() {
            return " ClassReader store default for " + sym.owner + "." + sym + " is " + value;
        }
    }

    class AnnotationCompleter extends AnnotationDeproxy implements Runnable {
        final Symbol sym;
        final List<CompoundAnnotationProxy> l;
        final JavaFileObject classFile;

        AnnotationCompleter(Symbol sym, List<CompoundAnnotationProxy> l) {
            super(currentOwner.kind == MTH
                    ? currentOwner.enclClass() : (ClassSymbol)currentOwner);
            if (sym.kind == TYP && sym.owner.kind == MDL) {
                this.sym = sym.owner;
            } else {
                this.sym = sym;
            }
            this.l = l;
            this.classFile = currentClassFile;
        }

        @Override
        public void run() {
            JavaFileObject previousClassFile = currentClassFile;
            try {
                currentClassFile = classFile;
                List<Attribute.Compound> newList = deproxyCompoundList(l);
                for (Attribute.Compound attr : newList) {
                    if (attr.type.tsym == syms.deprecatedType.tsym) {
                        sym.flags_field |= (DEPRECATED | DEPRECATED_ANNOTATION);
                        Attribute forRemoval = attr.member(names.forRemoval);
                        if (forRemoval instanceof Attribute.Constant) {
                            Attribute.Constant c = (Attribute.Constant) forRemoval;
                            if (c.type == syms.booleanType && ((Integer) c.value) != 0) {
                                sym.flags_field |= DEPRECATED_REMOVAL;
                            }
                        }
                    }
                }
                if (sym.annotationsPendingCompletion()) {
                    sym.setDeclarationAttributes(newList);
                } else {
                    sym.appendAttributes(newList);
                }
            } finally {
                currentClassFile = previousClassFile;
            }
        }

        @Override
        public String toString() {
            return " ClassReader annotate " + sym.owner + "." + sym + " with " + l;
        }
    }

    class TypeAnnotationCompleter extends AnnotationCompleter {

        List<TypeAnnotationProxy> proxies;

        TypeAnnotationCompleter(Symbol sym,
                List<TypeAnnotationProxy> proxies) {
            super(sym, List.nil());
            this.proxies = proxies;
        }

        List<Attribute.TypeCompound> deproxyTypeCompoundList(List<TypeAnnotationProxy> proxies) {
            ListBuffer<Attribute.TypeCompound> buf = new ListBuffer<>();
            for (TypeAnnotationProxy proxy: proxies) {
                Attribute.Compound compound = deproxyCompound(proxy.compound);
                Attribute.TypeCompound typeCompound = new Attribute.TypeCompound(compound, proxy.position);
                buf.add(typeCompound);
            }
            return buf.toList();
        }

        @Override
        public void run() {
            JavaFileObject previousClassFile = currentClassFile;
            try {
                currentClassFile = classFile;
                List<Attribute.TypeCompound> newList = deproxyTypeCompoundList(proxies);
                sym.setTypeAttributes(newList.prependList(sym.getRawTypeAttributes()));
            } finally {
                currentClassFile = previousClassFile;
            }
        }
    }


/************************************************************************
 * Reading Symbols
 ***********************************************************************/

    /** Read a field.
     */
    VarSymbol readField() {
        long flags = adjustFieldFlags(nextChar());
        Name name = readName(nextChar());
        Type type = readType(nextChar());
        VarSymbol v = new VarSymbol(flags, name, type, currentOwner);
        readMemberAttrs(v);
        return v;
    }

    /** Read a method.
     */
    MethodSymbol readMethod() {
        long flags = adjustMethodFlags(nextChar());
        Name name = readName(nextChar());
        Type type = readType(nextChar());
        if (currentOwner.isInterface() &&
                (flags & ABSTRACT) == 0 && !name.equals(names.clinit)) {
            if (majorVersion > Version.V52.major ||
                    (majorVersion == Version.V52.major && minorVersion >= Version.V52.minor)) {
                if ((flags & (STATIC | PRIVATE)) == 0) {
                    currentOwner.flags_field |= DEFAULT;
                    flags |= DEFAULT | ABSTRACT;
                }
            } else {
                //protect against ill-formed classfiles
                throw badClassFile((flags & STATIC) == 0 ? "invalid.default.interface" : "invalid.static.interface",
                                   Integer.toString(majorVersion),
                                   Integer.toString(minorVersion));
            }
        }
        if (name == names.init && currentOwner.hasOuterInstance()) {
            // Sometimes anonymous classes don't have an outer
            // instance, however, there is no reliable way to tell so
            // we never strip this$n
            // ditto for local classes. Local classes that have an enclosing method set
            // won't pass the "hasOuterInstance" check above, but those that don't have an
            // enclosing method (i.e. from initializers) will pass that check.
            boolean local = !currentOwner.owner.members().includes(currentOwner, LookupKind.NON_RECURSIVE);
            if (!currentOwner.name.isEmpty() && !local)
                type = new MethodType(adjustMethodParams(flags, type.getParameterTypes()),
                                      type.getReturnType(),
                                      type.getThrownTypes(),
                                      syms.methodClass);
        }
        MethodSymbol m = new MethodSymbol(flags, name, type, currentOwner);
        if (types.isSignaturePolymorphic(m)) {
            m.flags_field |= SIGNATURE_POLYMORPHIC;
        }
        if (saveParameterNames)
            initParameterNames(m);
        Symbol prevOwner = currentOwner;
        currentOwner = m;
        try {
            readMemberAttrs(m);
        } finally {
            currentOwner = prevOwner;
        }
        if (saveParameterNames)
            setParameterNames(m, type);

        if ((flags & VARARGS) != 0) {
            final Type last = type.getParameterTypes().last();
            if (last == null || !last.hasTag(ARRAY)) {
                m.flags_field &= ~VARARGS;
                throw badClassFile("malformed.vararg.method", m);
            }
        }

        return m;
    }

    private List<Type> adjustMethodParams(long flags, List<Type> args) {
        boolean isVarargs = (flags & VARARGS) != 0;
        if (isVarargs) {
            Type varargsElem = args.last();
            ListBuffer<Type> adjustedArgs = new ListBuffer<>();
            for (Type t : args) {
                adjustedArgs.append(t != varargsElem ?
                    t :
                    ((ArrayType)t).makeVarargs());
            }
            args = adjustedArgs.toList();
        }
        return args.tail;
    }

    /**
     * Init the parameter names array.
     * Parameter names are currently inferred from the names in the
     * LocalVariableTable attributes of a Code attribute.
     * (Note: this means parameter names are currently not available for
     * methods without a Code attribute.)
     * This method initializes an array in which to store the name indexes
     * of parameter names found in LocalVariableTable attributes. It is
     * slightly supersized to allow for additional slots with a start_pc of 0.
     */
    void initParameterNames(MethodSymbol sym) {
        // make allowance for synthetic parameters.
        final int excessSlots = 4;
        int expectedParameterSlots =
                Code.width(sym.type.getParameterTypes()) + excessSlots;
        if (parameterNameIndices == null
                || parameterNameIndices.length < expectedParameterSlots) {
            parameterNameIndices = new int[expectedParameterSlots];
        } else
            Arrays.fill(parameterNameIndices, 0);
        haveParameterNameIndices = false;
        sawMethodParameters = false;
    }

    /**
     * Set the parameter names for a symbol from the name index in the
     * parameterNameIndicies array. The type of the symbol may have changed
     * while reading the method attributes (see the Signature attribute).
     * This may be because of generic information or because anonymous
     * synthetic parameters were added.   The original type (as read from
     * the method descriptor) is used to help guess the existence of
     * anonymous synthetic parameters.
     * On completion, sym.savedParameter names will either be null (if
     * no parameter names were found in the class file) or will be set to a
     * list of names, one per entry in sym.type.getParameterTypes, with
     * any missing names represented by the empty name.
     */
    void setParameterNames(MethodSymbol sym, Type jvmType) {
        // if no names were found in the class file, there's nothing more to do
        if (!haveParameterNameIndices)
            return;
        // If we get parameter names from MethodParameters, then we
        // don't need to skip.
        int firstParam = 0;
        if (!sawMethodParameters) {
            firstParam = ((sym.flags() & STATIC) == 0) ? 1 : 0;
            // the code in readMethod may have skipped the first
            // parameter when setting up the MethodType. If so, we
            // make a corresponding allowance here for the position of
            // the first parameter.  Note that this assumes the
            // skipped parameter has a width of 1 -- i.e. it is not
        // a double width type (long or double.)
        if (sym.name == names.init && currentOwner.hasOuterInstance()) {
            // Sometimes anonymous classes don't have an outer
            // instance, however, there is no reliable way to tell so
            // we never strip this$n
            if (!currentOwner.name.isEmpty())
                firstParam += 1;
        }

        if (sym.type != jvmType) {
                // reading the method attributes has caused the
                // symbol's type to be changed. (i.e. the Signature
                // attribute.)  This may happen if there are hidden
                // (synthetic) parameters in the descriptor, but not
                // in the Signature.  The position of these hidden
                // parameters is unspecified; for now, assume they are
                // at the beginning, and so skip over them. The
                // primary case for this is two hidden parameters
                // passed into Enum constructors.
            int skip = Code.width(jvmType.getParameterTypes())
                    - Code.width(sym.type.getParameterTypes());
            firstParam += skip;
        }
        }
        List<Name> paramNames = List.nil();
        int index = firstParam;
        for (Type t: sym.type.getParameterTypes()) {
            int nameIdx = (index < parameterNameIndices.length
                    ? parameterNameIndices[index] : 0);
            Name name = nameIdx == 0 ? names.empty : readName(nameIdx);
            paramNames = paramNames.prepend(name);
            index += sawMethodParameters ? 1 : Code.width(t);
        }
        sym.savedParameterNames = paramNames.reverse();
    }

    /**
     * skip n bytes
     */
    void skipBytes(int n) {
        bp = bp + n;
    }

    /** Skip a field or method
     */
    void skipMember() {
        bp = bp + 6;
        char ac = nextChar();
        for (int i = 0; i < ac; i++) {
            bp = bp + 2;
            int attrLen = nextInt();
            bp = bp + attrLen;
        }
    }

    void skipInnerClasses() {
        int n = nextChar();
        for (int i = 0; i < n; i++) {
            nextChar();
            nextChar();
            nextChar();
            nextChar();
        }
    }

    /** Enter type variables of this classtype and all enclosing ones in
     *  `typevars'.
     */
    protected void enterTypevars(Symbol sym, Type t) {
        if (t.getEnclosingType() != null) {
            if (!t.getEnclosingType().hasTag(TypeTag.NONE)) {
                enterTypevars(sym.owner, t.getEnclosingType());
            }
        } else if (sym.kind == MTH && !sym.isStatic()) {
            enterTypevars(sym.owner, sym.owner.type);
        }
        for (List<Type> xs = t.getTypeArguments(); xs.nonEmpty(); xs = xs.tail) {
            typevars.enter(xs.head.tsym);
        }
    }

    protected ClassSymbol enterClass(Name name) {
        if (syms.proprietaryType.tsym.flatName() == name)
            return (ClassSymbol) syms.proprietaryType.tsym;
        return syms.enterClass(currentModule, name);
    }

    protected ClassSymbol enterClass(Name name, TypeSymbol owner) {
        return syms.enterClass(currentModule, name, owner);
    }

    /** Read contents of a given class symbol `c'. Both external and internal
     *  versions of an inner class are read.
     */
    void readClass(ClassSymbol c) {
        ClassType ct = (ClassType)c.type;

        // allocate scope for members
        c.members_field = WriteableScope.create(c);

        // prepare type variable table
        typevars = typevars.dup(currentOwner);
        if (ct.getEnclosingType().hasTag(CLASS))
            enterTypevars(c.owner, ct.getEnclosingType());

        // read flags, or skip if this is an inner class
        long f = nextChar();
        long flags = adjustClassFlags(f);
        if ((flags & MODULE) == 0) {
            if (c.owner.kind == PCK) c.flags_field = flags;
            // read own class name and check that it matches
            currentModule = c.packge().modle;
            ClassSymbol self = readClassSymbol(nextChar());
            if (c != self) {
                throw badClassFile("class.file.wrong.class",
                                   self.flatname);
            }
        } else {
            if (majorVersion < Version.V53.major) {
                throw badClassFile("anachronistic.module.info",
                        Integer.toString(majorVersion),
                        Integer.toString(minorVersion));
            }
            c.flags_field = flags;
            currentModule = (ModuleSymbol) c.owner;
            int this_class = nextChar();
            // temp, no check on this_class
        }

        // class attributes must be read before class
        // skip ahead to read class attributes
        int startbp = bp;
        nextChar();
        char interfaceCount = nextChar();
        bp += interfaceCount * 2;
        char fieldCount = nextChar();
        for (int i = 0; i < fieldCount; i++) skipMember();
        char methodCount = nextChar();
        for (int i = 0; i < methodCount; i++) skipMember();
        readClassAttrs(c);

        if (readAllOfClassFile) {
            for (int i = 1; i < poolObj.length; i++) readPool(i);
            c.pool = new Pool(poolObj.length, poolObj, types);
        }

        // reset and read rest of classinfo
        bp = startbp;
        int n = nextChar();
        if ((flags & MODULE) != 0 && n > 0) {
            throw badClassFile("module.info.invalid.super.class");
        }
        if (ct.supertype_field == null)
            ct.supertype_field = (n == 0)
                ? Type.noType
                : readClassSymbol(n).erasure(types);
        n = nextChar();
        List<Type> is = List.nil();
        for (int i = 0; i < n; i++) {
            Type _inter = readClassSymbol(nextChar()).erasure(types);
            is = is.prepend(_inter);
        }
        if (ct.interfaces_field == null)
            ct.interfaces_field = is.reverse();

        Assert.check(fieldCount == nextChar());
        for (int i = 0; i < fieldCount; i++) enterMember(c, readField());
        Assert.check(methodCount == nextChar());
        for (int i = 0; i < methodCount; i++) enterMember(c, readMethod());

        typevars = typevars.leave();
    }

    /** Read inner class info. For each inner/outer pair allocate a
     *  member class.
     */
    void readInnerClasses(ClassSymbol c) {
        int n = nextChar();
        for (int i = 0; i < n; i++) {
            nextChar(); // skip inner class symbol
            ClassSymbol outer = readClassSymbol(nextChar());
            Name name = readName(nextChar());
            if (name == null) name = names.empty;
            long flags = adjustClassFlags(nextChar());
            if (outer != null) { // we have a member class
                if (name == names.empty)
                    name = names.one;
                ClassSymbol member = enterClass(name, outer);
                if ((flags & STATIC) == 0) {
                    ((ClassType)member.type).setEnclosingType(outer.type);
                    if (member.erasure_field != null)
                        ((ClassType)member.erasure_field).setEnclosingType(types.erasure(outer.type));
                }
                if (c == outer) {
                    member.flags_field = flags;
                    enterMember(c, member);
                }
            }
        }
    }

    /** Read a class definition from the bytes in buf.
     */
    private void readClassBuffer(ClassSymbol c) throws IOException {
        int magic = nextInt();
        if (magic != JAVA_MAGIC)
            throw badClassFile("illegal.start.of.class.file");

        minorVersion = nextChar();
        majorVersion = nextChar();
        int maxMajor = 53; // Version.MAX().major;  //******* TEMPORARY *******
        int maxMinor = Version.MAX().minor;
        if (majorVersion > maxMajor ||
            majorVersion * 1000 + minorVersion <
            Version.MIN().major * 1000 + Version.MIN().minor) {
            if (majorVersion == (maxMajor + 1))
                log.warning(Warnings.BigMajorVersion(currentClassFile,
                                                     majorVersion,
                                                     maxMajor));
            else
                throw badClassFile("wrong.version",
                                   Integer.toString(majorVersion),
                                   Integer.toString(minorVersion),
                                   Integer.toString(maxMajor),
                                   Integer.toString(maxMinor));
        }

        indexPool();
        if (signatureBuffer.length < bp) {
            int ns = Integer.highestOneBit(bp) << 1;
            signatureBuffer = new byte[ns];
        }
        readClass(c);
    }

    public void readClassFile(ClassSymbol c) {
        currentOwner = c;
        currentClassFile = c.classfile;
        warnedAttrs.clear();
        filling = true;
        target = null;
        repeatable = null;
        try {
            bp = 0;
            buf = readInputStream(buf, c.classfile.openInputStream());
            readClassBuffer(c);
            if (!missingTypeVariables.isEmpty() && !foundTypeVariables.isEmpty()) {
                List<Type> missing = missingTypeVariables;
                List<Type> found = foundTypeVariables;
                missingTypeVariables = List.nil();
                foundTypeVariables = List.nil();
                interimUses = List.nil();
                interimProvides = List.nil();
                filling = false;
                ClassType ct = (ClassType)currentOwner.type;
                ct.supertype_field =
                    types.subst(ct.supertype_field, missing, found);
                ct.interfaces_field =
                    types.subst(ct.interfaces_field, missing, found);
                ct.typarams_field =
                    types.substBounds(ct.typarams_field, missing, found);
                for (List<Type> types = ct.typarams_field; types.nonEmpty(); types = types.tail) {
                    types.head.tsym.type = types.head;
                }
            } else if (missingTypeVariables.isEmpty() !=
                       foundTypeVariables.isEmpty()) {
                Name name = missingTypeVariables.head.tsym.name;
                throw badClassFile("undecl.type.var", name);
            }

            if ((c.flags_field & Flags.ANNOTATION) != 0) {
                c.setAnnotationTypeMetadata(new AnnotationTypeMetadata(c, new CompleterDeproxy(c, target, repeatable)));
            } else {
                c.setAnnotationTypeMetadata(AnnotationTypeMetadata.notAnAnnotationType());
            }

            if (c == currentModule.module_info) {
                if (interimUses.nonEmpty() || interimProvides.nonEmpty()) {
                    Assert.check(currentModule.isCompleted());
                    currentModule.usesProvidesCompleter =
                            new UsesProvidesCompleter(currentModule, interimUses, interimProvides);
                } else {
                    currentModule.uses = List.nil();
                    currentModule.provides = List.nil();
                }
            }
        } catch (IOException | ClosedFileSystemException ex) {
            throw badClassFile("unable.to.access.file", ex.toString());
        } catch (ArrayIndexOutOfBoundsException ex) {
            throw badClassFile("bad.class.file", c.flatname);
        } finally {
            interimUses = List.nil();
            interimProvides = List.nil();
            missingTypeVariables = List.nil();
            foundTypeVariables = List.nil();
            filling = false;
        }
    }
    // where
        private static byte[] readInputStream(byte[] buf, InputStream s) throws IOException {
            try {
                buf = ensureCapacity(buf, s.available());
                int r = s.read(buf);
                int bp = 0;
                while (r != -1) {
                    bp += r;
                    buf = ensureCapacity(buf, bp);
                    r = s.read(buf, bp, buf.length - bp);
                }
                return buf;
            } finally {
                try {
                    s.close();
                } catch (IOException e) {
                    /* Ignore any errors, as this stream may have already
                     * thrown a related exception which is the one that
                     * should be reported.
                     */
                }
            }
        }
        /*
         * ensureCapacity will increase the buffer as needed, taking note that
         * the new buffer will always be greater than the needed and never
         * exactly equal to the needed size or bp. If equal then the read (above)
         * will infinitely loop as buf.length - bp == 0.
         */
        private static byte[] ensureCapacity(byte[] buf, int needed) {
            if (buf.length <= needed) {
                byte[] old = buf;
                buf = new byte[Integer.highestOneBit(needed) << 1];
                System.arraycopy(old, 0, buf, 0, old.length);
            }
            return buf;
        }

    /** We can only read a single class file at a time; this
     *  flag keeps track of when we are currently reading a class
     *  file.
     */
    public boolean filling = false;

/************************************************************************
 * Adjusting flags
 ***********************************************************************/

    long adjustFieldFlags(long flags) {
        return flags;
    }

    long adjustMethodFlags(long flags) {
        if ((flags & ACC_BRIDGE) != 0) {
            flags &= ~ACC_BRIDGE;
            flags |= BRIDGE;
        }
        if ((flags & ACC_VARARGS) != 0) {
            flags &= ~ACC_VARARGS;
            flags |= VARARGS;
        }
        return flags;
    }

    long adjustClassFlags(long flags) {
        if ((flags & ACC_MODULE) != 0) {
            flags &= ~ACC_MODULE;
            flags |= MODULE;
        }
        return flags & ~ACC_SUPER; // SUPER and SYNCHRONIZED bits overloaded
    }

    /**
     * A subclass of JavaFileObject for the sourcefile attribute found in a classfile.
     * The attribute is only the last component of the original filename, so is unlikely
     * to be valid as is, so operations other than those to access the name throw
     * UnsupportedOperationException
     */
    private static class SourceFileObject implements JavaFileObject {

        /** The file's name.
         */
        private final Name name;
        private final Name flatname;

        public SourceFileObject(Name name, Name flatname) {
            this.name = name;
            this.flatname = flatname;
        }

        @Override @DefinedBy(Api.COMPILER)
        public URI toUri() {
            try {
                return new URI(null, name.toString(), null);
            } catch (URISyntaxException e) {
                throw new PathFileObject.CannotCreateUriError(name.toString(), e);
            }
        }

        @Override @DefinedBy(Api.COMPILER)
        public String getName() {
            return name.toString();
        }

        @Override @DefinedBy(Api.COMPILER)
        public JavaFileObject.Kind getKind() {
            return BaseFileManager.getKind(getName());
        }

        @Override @DefinedBy(Api.COMPILER)
        public InputStream openInputStream() {
            throw new UnsupportedOperationException();
        }

        @Override @DefinedBy(Api.COMPILER)
        public OutputStream openOutputStream() {
            throw new UnsupportedOperationException();
        }

        @Override @DefinedBy(Api.COMPILER)
        public CharBuffer getCharContent(boolean ignoreEncodingErrors) {
            throw new UnsupportedOperationException();
        }

        @Override @DefinedBy(Api.COMPILER)
        public Reader openReader(boolean ignoreEncodingErrors) {
            throw new UnsupportedOperationException();
        }

        @Override @DefinedBy(Api.COMPILER)
        public Writer openWriter() {
            throw new UnsupportedOperationException();
        }

        @Override @DefinedBy(Api.COMPILER)
        public long getLastModified() {
            throw new UnsupportedOperationException();
        }

        @Override @DefinedBy(Api.COMPILER)
        public boolean delete() {
            throw new UnsupportedOperationException();
        }

        @Override @DefinedBy(Api.COMPILER)
        public boolean isNameCompatible(String simpleName, JavaFileObject.Kind kind) {
            return true; // fail-safe mode
        }

        @Override @DefinedBy(Api.COMPILER)
        public NestingKind getNestingKind() {
            return null;
        }

        @Override @DefinedBy(Api.COMPILER)
        public Modifier getAccessLevel() {
            return null;
        }

        /**
         * Check if two file objects are equal.
         * SourceFileObjects are just placeholder objects for the value of a
         * SourceFile attribute, and do not directly represent specific files.
         * Two SourceFileObjects are equal if their names are equal.
         */
        @Override
        public boolean equals(Object other) {
            if (this == other)
                return true;

            if (!(other instanceof SourceFileObject))
                return false;

            SourceFileObject o = (SourceFileObject) other;
            return name.equals(o.name);
        }

        @Override
        public int hashCode() {
            return name.hashCode();
        }
    }

    private class CompleterDeproxy implements AnnotationTypeCompleter {
        ClassSymbol proxyOn;
        CompoundAnnotationProxy target;
        CompoundAnnotationProxy repeatable;

        public CompleterDeproxy(ClassSymbol c, CompoundAnnotationProxy target,
                CompoundAnnotationProxy repeatable)
        {
            this.proxyOn = c;
            this.target = target;
            this.repeatable = repeatable;
        }

        @Override
        public void complete(ClassSymbol sym) {
            Assert.check(proxyOn == sym);
            Attribute.Compound theTarget = null, theRepeatable = null;
            AnnotationDeproxy deproxy;

            try {
                if (target != null) {
                    deproxy = new AnnotationDeproxy(proxyOn);
                    theTarget = deproxy.deproxyCompound(target);
                }

                if (repeatable != null) {
                    deproxy = new AnnotationDeproxy(proxyOn);
                    theRepeatable = deproxy.deproxyCompound(repeatable);
                }
            } catch (Exception e) {
                throw new CompletionFailure(sym, ClassReader.this.diagFactory.fragment(Fragments.ExceptionMessage(e.getMessage())));
            }

            sym.getAnnotationTypeMetadata().setTarget(theTarget);
            sym.getAnnotationTypeMetadata().setRepeatable(theRepeatable);
        }
    }

    private class ProxyType extends Type {

        private final byte[] content;

        public ProxyType(byte[] content) {
            super(syms.noSymbol, TypeMetadata.EMPTY);
            this.content = content;
        }

        @Override
        public TypeTag getTag() {
            return TypeTag.NONE;
        }

        @Override
        public Type cloneWithMetadata(TypeMetadata metadata) {
            throw new UnsupportedOperationException();
        }

        public Type resolve() {
            return sigToType(content, 0, content.length);
        }

        @Override @DefinedBy(Api.LANGUAGE_MODEL)
        public String toString() {
            return "<ProxyType>";
        }

    }

    private static final class InterimUsesDirective {
        public final Name service;

        public InterimUsesDirective(Name service) {
            this.service = service;
        }

    }

    private static final class InterimProvidesDirective {
        public final Name service;
        public final List<Name> impls;

        public InterimProvidesDirective(Name service, List<Name> impls) {
            this.service = service;
            this.impls = impls;
        }

    }

    private final class UsesProvidesCompleter implements Completer {
        private final ModuleSymbol currentModule;
        private final List<InterimUsesDirective> interimUsesCopy;
        private final List<InterimProvidesDirective> interimProvidesCopy;

        public UsesProvidesCompleter(ModuleSymbol currentModule, List<InterimUsesDirective> interimUsesCopy, List<InterimProvidesDirective> interimProvidesCopy) {
            this.currentModule = currentModule;
            this.interimUsesCopy = interimUsesCopy;
            this.interimProvidesCopy = interimProvidesCopy;
        }

        @Override
        public void complete(Symbol sym) throws CompletionFailure {
            ListBuffer<Directive> directives = new ListBuffer<>();
            directives.addAll(currentModule.directives);
            ListBuffer<UsesDirective> uses = new ListBuffer<>();
            for (InterimUsesDirective interim : interimUsesCopy) {
                UsesDirective d = new UsesDirective(syms.enterClass(currentModule, interim.service));
                uses.add(d);
                directives.add(d);
            }
            currentModule.uses = uses.toList();
            ListBuffer<ProvidesDirective> provides = new ListBuffer<>();
            for (InterimProvidesDirective interim : interimProvidesCopy) {
                ListBuffer<ClassSymbol> impls = new ListBuffer<>();
                for (Name impl : interim.impls) {
                    impls.append(syms.enterClass(currentModule, impl));
                }
                ProvidesDirective d = new ProvidesDirective(syms.enterClass(currentModule, interim.service),
                                                            impls.toList());
                provides.add(d);
                directives.add(d);
            }
            currentModule.provides = provides.toList();
            currentModule.directives = directives.toList();
        }
    }
}
