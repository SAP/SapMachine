/*
 * Copyright (c) 2003, 2017, Oracle and/or its affiliates. All rights reserved.
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

package jdk.javadoc.internal.doclets.formats.html;


import javax.lang.model.element.Element;
import javax.lang.model.element.ModuleElement;
import javax.lang.model.element.PackageElement;
import javax.lang.model.element.TypeElement;
import javax.lang.model.type.TypeMirror;

import jdk.javadoc.internal.doclets.toolkit.AnnotationTypeFieldWriter;
import jdk.javadoc.internal.doclets.toolkit.AnnotationTypeOptionalMemberWriter;
import jdk.javadoc.internal.doclets.toolkit.AnnotationTypeRequiredMemberWriter;
import jdk.javadoc.internal.doclets.toolkit.AnnotationTypeWriter;
import jdk.javadoc.internal.doclets.toolkit.ClassWriter;
import jdk.javadoc.internal.doclets.toolkit.ConstantsSummaryWriter;
import jdk.javadoc.internal.doclets.toolkit.DocFilesHandler;
import jdk.javadoc.internal.doclets.toolkit.MemberSummaryWriter;
import jdk.javadoc.internal.doclets.toolkit.ModuleSummaryWriter;
import jdk.javadoc.internal.doclets.toolkit.PackageSummaryWriter;
import jdk.javadoc.internal.doclets.toolkit.SerializedFormWriter;
import jdk.javadoc.internal.doclets.toolkit.WriterFactory;
import jdk.javadoc.internal.doclets.toolkit.util.ClassTree;
import jdk.javadoc.internal.doclets.toolkit.util.VisibleMemberMap;

/**
 * The factory that returns HTML writers.
 *
 *  <p><b>This is NOT part of any supported API.
 *  If you write code that depends on this, you do so at your own risk.
 *  This code and its internal interfaces are subject to change or
 *  deletion without notice.</b>
 *
 * @author Jamie Ho
 */
public class WriterFactoryImpl implements WriterFactory {

    private final HtmlConfiguration configuration;
    public WriterFactoryImpl(HtmlConfiguration configuration) {
        this.configuration = configuration;
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public ConstantsSummaryWriter getConstantsSummaryWriter() {
        return new ConstantsSummaryWriterImpl(configuration);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public PackageSummaryWriter getPackageSummaryWriter(PackageElement packageElement,
            PackageElement prevPkg, PackageElement nextPkg) {
        return new PackageWriterImpl(configuration, packageElement, prevPkg, nextPkg);
    }

    /**
     * {@inheritDoc}
     */
    public ModuleSummaryWriter getModuleSummaryWriter(ModuleElement mdle,
        ModuleElement prevModule, ModuleElement nextModule) {
        return new ModuleWriterImpl(configuration, mdle,
            prevModule, nextModule);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public ClassWriter getClassWriter(TypeElement typeElement, TypeElement prevClass,
            TypeElement nextClass, ClassTree classTree) {
        return new ClassWriterImpl(configuration, typeElement, prevClass, nextClass, classTree);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public AnnotationTypeWriter getAnnotationTypeWriter(TypeElement annotationType,
            TypeMirror prevType, TypeMirror nextType) {
        return new AnnotationTypeWriterImpl(configuration, annotationType, prevType, nextType);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public AnnotationTypeFieldWriter getAnnotationTypeFieldWriter(
            AnnotationTypeWriter annotationTypeWriter) {
        TypeElement te = annotationTypeWriter.getAnnotationTypeElement();
        return new AnnotationTypeFieldWriterImpl(
            (SubWriterHolderWriter) annotationTypeWriter, te);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public AnnotationTypeOptionalMemberWriter getAnnotationTypeOptionalMemberWriter(
        AnnotationTypeWriter annotationTypeWriter) {
        TypeElement te = annotationTypeWriter.getAnnotationTypeElement();
        return new AnnotationTypeOptionalMemberWriterImpl(
            (SubWriterHolderWriter) annotationTypeWriter, te);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public AnnotationTypeRequiredMemberWriter getAnnotationTypeRequiredMemberWriter(
            AnnotationTypeWriter annotationTypeWriter) {
        TypeElement te = annotationTypeWriter.getAnnotationTypeElement();
        return new AnnotationTypeRequiredMemberWriterImpl(
            (SubWriterHolderWriter) annotationTypeWriter, te);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public EnumConstantWriterImpl getEnumConstantWriter(ClassWriter classWriter) {
        return new EnumConstantWriterImpl((SubWriterHolderWriter) classWriter,
                classWriter.getTypeElement());
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public FieldWriterImpl getFieldWriter(ClassWriter classWriter) {
        return new FieldWriterImpl((SubWriterHolderWriter) classWriter, classWriter.getTypeElement());
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public PropertyWriterImpl getPropertyWriter(ClassWriter classWriter) {
        return new PropertyWriterImpl((SubWriterHolderWriter) classWriter,
                classWriter.getTypeElement());
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public MethodWriterImpl getMethodWriter(ClassWriter classWriter) {
        return new MethodWriterImpl((SubWriterHolderWriter) classWriter, classWriter.getTypeElement());
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public ConstructorWriterImpl getConstructorWriter(ClassWriter classWriter) {
        return new ConstructorWriterImpl((SubWriterHolderWriter) classWriter,
                classWriter.getTypeElement());
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public MemberSummaryWriter getMemberSummaryWriter(ClassWriter classWriter,
            VisibleMemberMap.Kind memberType) {
        switch (memberType) {
            case CONSTRUCTORS:
                return getConstructorWriter(classWriter);
            case ENUM_CONSTANTS:
                return getEnumConstantWriter(classWriter);
            case FIELDS:
                return getFieldWriter(classWriter);
            case PROPERTIES:
                return getPropertyWriter(classWriter);
            case INNER_CLASSES:
                return new NestedClassWriterImpl((SubWriterHolderWriter)
                    classWriter, classWriter.getTypeElement());
            case METHODS:
                return getMethodWriter(classWriter);
            default:
                return null;
        }
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public MemberSummaryWriter getMemberSummaryWriter(AnnotationTypeWriter annotationTypeWriter,
            VisibleMemberMap.Kind memberType) {
        switch (memberType) {
            case ANNOTATION_TYPE_FIELDS:
                return (AnnotationTypeFieldWriterImpl)
                    getAnnotationTypeFieldWriter(annotationTypeWriter);
            case ANNOTATION_TYPE_MEMBER_OPTIONAL:
                return (AnnotationTypeOptionalMemberWriterImpl)
                    getAnnotationTypeOptionalMemberWriter(annotationTypeWriter);
            case ANNOTATION_TYPE_MEMBER_REQUIRED:
                return (AnnotationTypeRequiredMemberWriterImpl)
                    getAnnotationTypeRequiredMemberWriter(annotationTypeWriter);
            default:
                return null;
        }
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public SerializedFormWriter getSerializedFormWriter() {
        return new SerializedFormWriterImpl(configuration);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public DocFilesHandler getDocFilesHandler(Element element) {
        return new DocFilesHandlerImpl(configuration, element);
    }
}
