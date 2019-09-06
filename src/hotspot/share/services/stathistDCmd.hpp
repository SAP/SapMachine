/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2019 SAP SE. All rights reserved.
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

#ifndef HOTSPOT_SHARE_SERVICES_STATHIST_INTERNALS_HPP
#define HOTSPOT_SHARE_SERVICES_STATHIST_INTERNALS_HPP

#include "services/diagnosticCommand.hpp"

namespace StatisticsHistory {

class StatHistDCmd : public DCmdWithParser {
protected:
  DCmdArgument<char*> _scale;
  DCmdArgument<bool> _cvs;
  DCmdArgument<bool> _no_legend;
#ifdef ASSERT
  DCmdArgument<bool> _raw;
#endif
public:
  StatHistDCmd(outputStream* output, bool heap);
  static const char* name() {
    return "VM.vitals";
  }
  static const char* description() {
    return "Print Vitals.";
  }
  static const char* impact() {
    return "Low.";
  }
  static const JavaPermission permission() {
    JavaPermission p = {"java.lang.management.ManagementPermission",
                        "monitor", NULL};
    return p;
  }
  static int num_arguments();
  virtual void execute(DCmdSource source, TRAPS);
};

}; // namespace StatisticsHistory

#endif /* HOTSPOT_SHARE_SERVICES_STATHIST_INTERNALS_HPP */
