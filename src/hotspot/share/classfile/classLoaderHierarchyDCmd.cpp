/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2018 SAP SE. All rights reserved.
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
 *
 */

#include "precompiled.hpp"

#include "classfile/classLoaderData.inline.hpp"
#include "classfile/classLoaderHierarchyDCmd.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/safepoint.hpp"
#include "oops/reflectionAccessorImplKlassHelper.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"


ClassLoaderHierarchyDCmd::ClassLoaderHierarchyDCmd(outputStream* output, bool heap)
  : DCmdWithParser(output, heap)
  , _show_classes("show-classes", "Print loaded classes.", "BOOLEAN", false, "false")
  , _verbose("verbose", "Print detailed information.", "BOOLEAN", false, "false") {
  _dcmdparser.add_dcmd_option(&_show_classes);
  _dcmdparser.add_dcmd_option(&_verbose);
}


int ClassLoaderHierarchyDCmd::num_arguments() {
  ResourceMark rm;
  ClassLoaderHierarchyDCmd* dcmd = new ClassLoaderHierarchyDCmd(NULL, false);
  if (dcmd != NULL) {
    DCmdMark mark(dcmd);
    return dcmd->_dcmdparser.num_arguments();
  } else {
    return 0;
  }
}

// Helper class for drawing the branches to the left of a node.
class BranchTracker : public StackObj {
  //       "<x>"
  //       " |---<y>"
  //       " |    |
  //       " |   <z>"
  //       " |    |---<z1>
  //       " |    |---<z2>
  //       ^^^^^^^ ^^^
  //        A       B

  // Some terms for the graphics:
  // - branch: vertical connection between a node's ancestor to a later sibling.
  // - branchwork: (A) the string to print as a prefix at the start of each line, contains all branches.
  // - twig (B): Length of the dashed line connecting a node to its branch.
  // - branch spacing: how many spaces between branches are printed.

public:

  enum { max_depth = 64, twig_len = 2, branch_spacing = 5 };

private:

  char _branches[max_depth];
  int _pos;

public:
  BranchTracker()
    : _pos(0) {}

  void push(bool has_branch) {
    if (_pos < max_depth) {
      _branches[_pos] = has_branch ? '|' : ' ';
    }
    _pos ++; // beyond max depth, omit branch drawing but do count on.
  }

  void pop() {
    assert(_pos > 0, "must be");
    _pos --;
  }

  void print(outputStream* st) {
    for (int i = 0; i < _pos; i ++) {
      st->print("%c%.*s", _branches[i], branch_spacing, "          ");
    }
  }

  class Mark {
    BranchTracker& _tr;
  public:
    Mark(BranchTracker& tr, bool has_branch_here)
      : _tr(tr)  { _tr.push(has_branch_here); }
    ~Mark() { _tr.pop(); }
  };

}; // end: BranchTracker

struct LoadedClassInfo : public ResourceObj {
public:
  LoadedClassInfo* _next;
  Klass* const _klass;
  const ClassLoaderData* const _cld;

  LoadedClassInfo(Klass* klass, const ClassLoaderData* cld)
    : _klass(klass), _cld(cld) {}

};

class LoaderTreeNode : public ResourceObj {

  // We walk the CLDG and, for each CLD which is non-anonymous, add
  // a tree node. To add a node we need its parent node; if it itself
  // does not exist yet, we add a preliminary node for it. This preliminary
  // node just contains its loader oop; later, when encountering its CLD in
  // our CLDG walk, we complete the missing information in this node.

  const oop _loader_oop;
  const ClassLoaderData* _cld;

  LoaderTreeNode* _child;
  LoaderTreeNode* _next;

  LoadedClassInfo* _classes;
  int _num_classes;

  LoadedClassInfo* _anon_classes;
  int _num_anon_classes;

  void print_with_childs(outputStream* st, BranchTracker& branchtracker,
      bool print_classes, bool verbose) const {

    ResourceMark rm;

    if (_cld == NULL) {
      // Not sure how this could happen: we added a preliminary node for a parent but then never encountered
      // its CLD?
      return;
    }

    // Retrieve information.
    const Klass* const loader_klass = _cld->class_loader_klass();
    const Symbol* const loader_name = _cld->name();

    branchtracker.print(st);

    // e.g. "+--- jdk.internal.reflect.DelegatingClassLoader"
    st->print("+%.*s", BranchTracker::twig_len, "----------");
    if (_cld->is_the_null_class_loader_data()) {
      st->print(" <bootstrap>");
    } else {
      if (loader_name != NULL) {
        st->print(" \"%s\",", loader_name->as_C_string());
      }
      st->print(" %s", loader_klass != NULL ? loader_klass->external_name() : "??");
      st->print(" {" PTR_FORMAT "}", p2i(_loader_oop));
    }
    st->cr();

    // Output following this node (node details and child nodes) - up to the next sibling node
    // needs to be prefixed with "|" if there is a follow up sibling.
    const bool have_sibling = _next != NULL;
    BranchTracker::Mark trm(branchtracker, have_sibling);

    {
      // optional node details following this node needs to be prefixed with "|"
      // if there are follow up child nodes.
      const bool have_child = _child != NULL;
      BranchTracker::Mark trm(branchtracker, have_child);

      // Empty line
      branchtracker.print(st);
      st->cr();

      const int indentation = 18;

      if (verbose) {
        branchtracker.print(st);
        st->print_cr("%*s " PTR_FORMAT, indentation, "Loader Data:", p2i(_cld));
        branchtracker.print(st);
        st->print_cr("%*s " PTR_FORMAT, indentation, "Loader Klass:", p2i(loader_klass));

        // Empty line
        branchtracker.print(st);
        st->cr();
      }

      if (print_classes) {
        if (_classes != NULL) {
          for (LoadedClassInfo* lci = _classes; lci; lci = lci->_next) {
            // Non-anonymous classes should live in the primary CLD of its loader
            assert(lci->_cld == _cld, "must be");

            branchtracker.print(st);
            if (lci == _classes) { // first iteration
              st->print("%*s ", indentation, "Classes:");
            } else {
              st->print("%*s ", indentation, "");
            }
            st->print("%s", lci->_klass->external_name());

            // Special treatment for generated core reflection accessor classes: print invocation target.
            if (ReflectionAccessorImplKlassHelper::is_generated_accessor(lci->_klass)) {
              st->print(" (invokes: ");
              ReflectionAccessorImplKlassHelper::print_invocation_target(st, lci->_klass);
              st->print(")");
            }

            st->cr();
          }
          branchtracker.print(st);
          st->print("%*s ", indentation, "");
          st->print_cr("(%u class%s)", _num_classes, (_num_classes == 1) ? "" : "es");

          // Empty line
          branchtracker.print(st);
          st->cr();
        }

        if (_anon_classes != NULL) {
          for (LoadedClassInfo* lci = _anon_classes; lci; lci = lci->_next) {
            branchtracker.print(st);
            if (lci == _anon_classes) { // first iteration
              st->print("%*s ", indentation, "Anonymous Classes:");
            } else {
              st->print("%*s ", indentation, "");
            }
            st->print("%s", lci->_klass->external_name());
            // For anonymous classes, also print CLD if verbose. Should be a different one than the primary CLD.
            assert(lci->_cld != _cld, "must be");
            if (verbose) {
              st->print("  (CLD: " PTR_FORMAT ")", p2i(lci->_cld));
            }
            st->cr();
          }
          branchtracker.print(st);
          st->print("%*s ", indentation, "");
          st->print_cr("(%u anonymous class%s)", _num_anon_classes, (_num_anon_classes == 1) ? "" : "es");

          // Empty line
          branchtracker.print(st);
          st->cr();
        }

      } // end: print_classes

    } // Pop branchtracker mark

    // Print children, recursively
    LoaderTreeNode* c = _child;
    while (c != NULL) {
      c->print_with_childs(st, branchtracker, print_classes, verbose);
      c = c->_next;
    }

  }

public:

  LoaderTreeNode(const oop loader_oop)
    : _loader_oop(loader_oop), _cld(NULL)
    , _child(NULL), _next(NULL)
    , _classes(NULL), _anon_classes(NULL)
    , _num_classes(0), _num_anon_classes(0) {}

  void set_cld(const ClassLoaderData* cld) {
    _cld = cld;
  }

  void add_child(LoaderTreeNode* info) {
    info->_next = _child;
    _child = info;
  }

  void add_sibling(LoaderTreeNode* info) {
    assert(info->_next == NULL, "must be");
    info->_next = _next;
    _next = info;
  }

  void add_classes(LoadedClassInfo* first_class, int num_classes, bool anonymous) {
    LoadedClassInfo** p_list_to_add_to = anonymous ? &_anon_classes : &_classes;
    // Search tail.
    while ((*p_list_to_add_to) != NULL) {
      p_list_to_add_to = &(*p_list_to_add_to)->_next;
    }
    *p_list_to_add_to = first_class;
    if (anonymous) {
      _num_anon_classes += num_classes;
    } else {
      _num_classes += num_classes;
    }
  }

  const ClassLoaderData* cld() const {
    return _cld;
  }

  const oop loader_oop() const {
    return _loader_oop;
  }

  LoaderTreeNode* find(const oop loader_oop) {
    LoaderTreeNode* result = NULL;
    if (_loader_oop == loader_oop) {
      result = this;
    } else {
      LoaderTreeNode* c = _child;
      while (c != NULL && result == NULL) {
        result = c->find(loader_oop);
        c = c->_next;
      }
    }
    return result;
  }

  void print_with_childs(outputStream* st, bool print_classes, bool print_add_info) const {
    BranchTracker bwt;
    print_with_childs(st, bwt, print_classes, print_add_info);
  }

};

class LoadedClassCollectClosure : public KlassClosure {
public:
  LoadedClassInfo* _list;
  const ClassLoaderData* _cld;
  int _num_classes;
  LoadedClassCollectClosure(const ClassLoaderData* cld)
    : _list(NULL), _cld(cld), _num_classes(0) {}
  void do_klass(Klass* k) {
    LoadedClassInfo* lki = new LoadedClassInfo(k, _cld);
    lki->_next = _list;
    _list = lki;
    _num_classes ++;
  }
};

class LoaderInfoScanClosure : public CLDClosure {

  const bool _print_classes;
  const bool _verbose;
  LoaderTreeNode* _root;

  static void fill_in_classes(LoaderTreeNode* info, const ClassLoaderData* cld) {
    assert(info != NULL && cld != NULL, "must be");
    LoadedClassCollectClosure lccc(cld);
    const_cast<ClassLoaderData*>(cld)->classes_do(&lccc);
    if (lccc._num_classes > 0) {
      info->add_classes(lccc._list, lccc._num_classes, cld->is_anonymous());
    }
  }

  LoaderTreeNode* find_node_or_add_empty_node(oop loader_oop) {

    assert(_root != NULL, "root node must exist");

    if (loader_oop == NULL) {
      return _root;
    }

    // Check if a node for this oop already exists.
    LoaderTreeNode* info = _root->find(loader_oop);

    if (info == NULL) {
      // It does not. Create a node.
      info = new LoaderTreeNode(loader_oop);

      // Add it to tree.
      LoaderTreeNode* parent_info = NULL;

      // Recursively add parent nodes if needed.
      const oop parent_oop = java_lang_ClassLoader::parent(loader_oop);
      if (parent_oop == NULL) {
        parent_info = _root;
      } else {
        parent_info = find_node_or_add_empty_node(parent_oop);
      }
      assert(parent_info != NULL, "must be");

      parent_info->add_child(info);
    }
    return info;
  }


public:
  LoaderInfoScanClosure(bool print_classes, bool verbose)
    : _print_classes(print_classes), _verbose(verbose), _root(NULL) {
    _root = new LoaderTreeNode(NULL);
  }

  void print_results(outputStream* st) const {
    _root->print_with_childs(st, _print_classes, _verbose);
  }

  void do_cld (ClassLoaderData* cld) {

    // We do not display unloading loaders, for now.
    if (cld->is_unloading()) {
      return;
    }

    const oop loader_oop = cld->class_loader();

    LoaderTreeNode* info = find_node_or_add_empty_node(loader_oop);
    assert(info != NULL, "must be");

    // Update CLD in node, but only if this is the primary CLD for this loader.
    if (cld->is_anonymous() == false) {
      assert(info->cld() == NULL, "there should be only one primary CLD per loader");
      info->set_cld(cld);
    }

    // Add classes.
    fill_in_classes(info, cld);
  }

};


class ClassLoaderHierarchyVMOperation : public VM_Operation {
  outputStream* const _out;
  const bool _show_classes;
  const bool _verbose;
public:
  ClassLoaderHierarchyVMOperation(outputStream* out, bool show_classes, bool verbose) :
    _out(out), _show_classes(show_classes), _verbose(verbose)
  {}

  VMOp_Type type() const {
    return VMOp_ClassLoaderHierarchyOperation;
  }

  void doit() {
    assert(SafepointSynchronize::is_at_safepoint(), "must be a safepoint");
    ResourceMark rm;
    LoaderInfoScanClosure cl (_show_classes, _verbose);
    ClassLoaderDataGraph::cld_do(&cl);
    cl.print_results(_out);
  }
};

// This command needs to be executed at a safepoint.
void ClassLoaderHierarchyDCmd::execute(DCmdSource source, TRAPS) {
  ClassLoaderHierarchyVMOperation op(output(), _show_classes.value(), _verbose.value());
  VMThread::execute(&op);
}
