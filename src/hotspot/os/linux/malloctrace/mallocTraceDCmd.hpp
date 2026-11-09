/*
 * Copyright (c) 2021, 2023 SAP SE. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef OS_LINUX_MALLOCTRACE_MALLOCTRACEDCMD_HPP
#define OS_LINUX_MALLOCTRACE_MALLOCTRACEDCMD_HPP

#include "services/diagnosticCommand.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace sap {

class MallocTraceDCmd : public DCmdWithParser {
  DCmdArgument<char*> _option;
  DCmdArgument<char*>  _suboption;
public:
  static int num_arguments() { return 2; }
  MallocTraceDCmd(outputStream* output, bool heap);
  static const char* name() {
    return "System.malloctrace";
  }
  static const char* description() {
    return "Trace malloc call sites\n"
           "Note: do *not* use in conjunction with MALLOC_CHECK_..!";
  }
  static const char* impact() {
    return "Low";
  }
  static const JavaPermission permission() {
    JavaPermission p = { "java.lang.management.ManagementPermission", "control", NULL };
    return p;
  }
  virtual void execute(DCmdSource source, TRAPS);
};

} // namespace sap

#endif // OS_LINUX_MALLOCTRACE_MALLOCTRACEDCMD_HPP
