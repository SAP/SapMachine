/*
 * Copyright (c) 1997, 2019, Oracle and/or its affiliates. All rights reserved.
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

import java.util.Arrays;
import java.util.List;

import javax.lang.model.element.Element;
import javax.lang.model.element.ExecutableElement;
import javax.lang.model.element.TypeElement;

import jdk.javadoc.internal.doclets.formats.html.markup.ContentBuilder;
import jdk.javadoc.internal.doclets.formats.html.markup.Entity;
import jdk.javadoc.internal.doclets.formats.html.markup.HtmlStyle;
import jdk.javadoc.internal.doclets.formats.html.markup.HtmlTag;
import jdk.javadoc.internal.doclets.formats.html.markup.HtmlTree;
import jdk.javadoc.internal.doclets.formats.html.markup.StringContent;
import jdk.javadoc.internal.doclets.formats.html.markup.Table;
import jdk.javadoc.internal.doclets.formats.html.markup.TableHeader;
import jdk.javadoc.internal.doclets.toolkit.ConstructorWriter;
import jdk.javadoc.internal.doclets.toolkit.Content;
import jdk.javadoc.internal.doclets.toolkit.MemberSummaryWriter;
import jdk.javadoc.internal.doclets.toolkit.util.VisibleMemberTable;

import static jdk.javadoc.internal.doclets.toolkit.util.VisibleMemberTable.Kind.CONSTRUCTORS;


/**
 * Writes constructor documentation.
 *
 *  <p><b>This is NOT part of any supported API.
 *  If you write code that depends on this, you do so at your own risk.
 *  This code and its internal interfaces are subject to change or
 *  deletion without notice.</b>
 *
 * @author Robert Field
 * @author Atul M Dambalkar
 * @author Bhavesh Patel (Modified)
 */
public class ConstructorWriterImpl extends AbstractExecutableMemberWriter
    implements ConstructorWriter, MemberSummaryWriter {

    private boolean foundNonPubConstructor = false;

    /**
     * Construct a new ConstructorWriterImpl.
     *
     * @param writer The writer for the class that the constructors belong to.
     * @param typeElement the class being documented.
     */
    public ConstructorWriterImpl(SubWriterHolderWriter writer, TypeElement typeElement) {
        super(writer, typeElement);

        VisibleMemberTable vmt = configuration.getVisibleMemberTable(typeElement);
        List<? extends Element> constructors = vmt.getVisibleMembers(CONSTRUCTORS);

        for (Element constructor : constructors) {
            if (utils.isProtected(constructor) || utils.isPrivate(constructor)) {
                setFoundNonPubConstructor(true);
            }
        }
    }

    /**
     * Construct a new ConstructorWriterImpl.
     *
     * @param writer The writer for the class that the constructors belong to.
     */
    public ConstructorWriterImpl(SubWriterHolderWriter writer) {
        super(writer);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public Content getMemberSummaryHeader(TypeElement typeElement,
            Content memberSummaryTree) {
        memberSummaryTree.add(MarkerComments.START_OF_CONSTRUCTOR_SUMMARY);
        Content memberTree = new ContentBuilder();
        writer.addSummaryHeader(this, typeElement, memberTree);
        return memberTree;
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addMemberTree(Content memberSummaryTree, Content memberTree) {
        writer.addMemberTree(HtmlStyle.constructorSummary, memberSummaryTree, memberTree);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public Content getConstructorDetailsTreeHeader(TypeElement typeElement,
            Content memberDetailsTree) {
        memberDetailsTree.add(MarkerComments.START_OF_CONSTRUCTOR_DETAILS);
        Content constructorDetailsTree = new ContentBuilder();
        Content heading = HtmlTree.HEADING(Headings.TypeDeclaration.DETAILS_HEADING,
                contents.constructorDetailsLabel);
        constructorDetailsTree.add(links.createAnchor(
                SectionName.CONSTRUCTOR_DETAIL));
        constructorDetailsTree.add(heading);
        return constructorDetailsTree;
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public Content getConstructorDocTreeHeader(ExecutableElement constructor,
            Content constructorDetailsTree) {
        String erasureAnchor;
        Content constructorDocTree = new ContentBuilder();
        Content heading = new HtmlTree(Headings.TypeDeclaration.MEMBER_HEADING);
        if ((erasureAnchor = getErasureAnchor(constructor)) != null) {
            heading.add(links.createAnchor((erasureAnchor)));
        }
        heading.add(links.createAnchor(writer.getAnchor(constructor), new StringContent(name(constructor))));
        constructorDocTree.add(heading);
        return HtmlTree.SECTION(HtmlStyle.detail, constructorDocTree);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public Content getSignature(ExecutableElement constructor) {
        return new MemberSignature(constructor)
                .addParameters(getParameters(constructor, true))
                .addExceptions(getExceptions(constructor))
                .toContent();
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addDeprecated(ExecutableElement constructor, Content constructorDocTree) {
        addDeprecatedInfo(constructor, constructorDocTree);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addComments(ExecutableElement constructor, Content constructorDocTree) {
        addComment(constructor, constructorDocTree);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addTags(ExecutableElement constructor, Content constructorDocTree) {
        writer.addTagsInfo(constructor, constructorDocTree);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public Content getConstructorDetails(Content constructorDetailsTreeHeader, Content constructorDetailsTree) {
        Content constructorDetails = new ContentBuilder(constructorDetailsTreeHeader, constructorDetailsTree);
        return getMemberTree(HtmlTree.SECTION(HtmlStyle.constructorDetails, constructorDetails));
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public Content getConstructorDoc(Content constructorDocTree) {
        return getMemberTree(constructorDocTree);
    }

    /**
     * Let the writer know whether a non public constructor was found.
     *
     * @param foundNonPubConstructor true if we found a non public constructor.
     */
    @Override
    public void setFoundNonPubConstructor(boolean foundNonPubConstructor) {
        this.foundNonPubConstructor = foundNonPubConstructor;
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addSummaryLabel(Content memberTree) {
        Content label = HtmlTree.HEADING(Headings.TypeDeclaration.SUMMARY_HEADING,
                contents.constructorSummaryLabel);
        memberTree.add(label);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public TableHeader getSummaryTableHeader(Element member) {
        if (foundNonPubConstructor) {
            return new TableHeader(contents.modifierLabel, contents.constructorLabel,
                    contents.descriptionLabel);
        } else {
            return new TableHeader(contents.constructorLabel, contents.descriptionLabel);
        }
    }

    @Override
    protected Table createSummaryTable() {
        List<HtmlStyle> bodyRowStyles;
        int rowScopeColumn;

        if (foundNonPubConstructor) {
            bodyRowStyles = Arrays.asList(HtmlStyle.colFirst, HtmlStyle.colConstructorName,
                    HtmlStyle.colLast);
            rowScopeColumn = 1;
        } else {
            bodyRowStyles = Arrays.asList(HtmlStyle.colConstructorName, HtmlStyle.colLast);
            rowScopeColumn = 0;
        }

        return new Table(HtmlStyle.memberSummary)
                .setCaption(contents.constructors)
                .setHeader(getSummaryTableHeader(typeElement))
                .setRowScopeColumn(rowScopeColumn)
                .setColumnStyles(bodyRowStyles);
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addSummaryAnchor(TypeElement typeElement, Content memberTree) {
        memberTree.add(links.createAnchor(SectionName.CONSTRUCTOR_SUMMARY));
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addInheritedSummaryAnchor(TypeElement typeElement, Content inheritedTree) {
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void addInheritedSummaryLabel(TypeElement typeElement, Content inheritedTree) {
    }

    /**
     * {@inheritDoc}
     */
    @Override
    protected void addSummaryType(Element member, Content tdSummaryType) {
        if (foundNonPubConstructor) {
            Content code = new HtmlTree(HtmlTag.CODE);
            if (utils.isProtected(member)) {
                code.add("protected ");
            } else if (utils.isPrivate(member)) {
                code.add("private ");
            } else if (utils.isPublic(member)) {
                code.add(Entity.NO_BREAK_SPACE);
            } else {
                code.add(
                        resources.getText("doclet.Package_private"));
            }
            tdSummaryType.add(code);
        }
    }

    @Override
    public Content getMemberTreeHeader(){
        return writer.getMemberTreeHeader();
    }
}
