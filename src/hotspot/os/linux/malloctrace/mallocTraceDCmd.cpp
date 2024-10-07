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

#include "precompiled.hpp"
#include "malloctrace/mallocTrace.hpp"
#include "malloctrace/mallocTraceDCmd.hpp"
#include "utilities/globalDefinitions.hpp"

#include <errno.h>

namespace sap {

#ifdef HAVE_GLIBC_MALLOC_HOOKS

// By default, let's use nmt-like capturing. I see (very rarely) crashes with backtrace(3) on x86.
// backtrace(3) gives us better callstack but runs a (small) risk of crashing, especially on x86.
const bool use_backtrace_default = false;

void MallocTraceDCmd::execute(DCmdSource source, TRAPS) {
  const char* const subopt = _suboption.value();
  if (::strcmp(_option.value(), "on") == 0) {
    bool use_backtrace = use_backtrace_default;
    if (subopt != NULL) {
      if (::strcmp(subopt, "bt") == 0) {
        use_backtrace = true;
      } else if (::strcmp(subopt, "nmt") == 0) {
        use_backtrace = false;
      } else {
        _output->print_cr("Invalid sub option");
        return;
      }
    }
    if (MallocTracer::enable(use_backtrace)) {
      _output->print_raw("Tracing active");
    } else {
      _output->print("Failed to activate");
    }
  } else if (::strcmp(_option.value(), "off") == 0) {
    MallocTracer::disable();
    _output->print_raw("Tracing inactive");
  } else if (::strcmp(_option.value(), "print") == 0) {
    bool all = false;
    if (subopt != NULL) {
      if (::strcmp(subopt, "all") == 0) {
        all = true;
      } else {
        _output->print_cr("Invalid sub option");
        return;
      }
    }
    MallocTracer::print(_output, all);
  } else if (::strcmp(_option.value(), "reset") == 0) {
    MallocTracer::reset();
    _output->print_raw("Tracing table reset");
  } else {
    _output->print_cr("unknown sub command %s", _option.value());
  }
  _output->cr();
}
#else
void MallocTraceDCmd::execute(DCmdSource source, TRAPS) {
#ifdef __GLIBC__
  _output->print_cr("Glibc too new. Needs glibc version <= 2.31.");
#else
  _output->print_cr("Not a glibc system.");
#endif
}
#endif // HAVE_GLIBC_MALLOC_HOOKS

static const char* const usage_for_option =
  "Valid Values:\n"
  " - on [bt|nmt]\n"
  "    Switches trace on. Optional second parameter overrides the stack walk method.\n"
  "                      - nmt (default): uses internal stackwalking.\n"
  "                      - bt: uses glibc stackwalking (may give better results, but can be unstable).\n"
  " - off\n"
  "    Switches trace off.\n"
  " - print [all]\n"
  "    Print the capture table. By default only hot sites are printed; specifying \"all\" will print the full table.\n"
  " - reset\n"
  "    Resets the capture table.\n";

MallocTraceDCmd::MallocTraceDCmd(outputStream* output, bool heap) :
  DCmdWithParser(output, heap),
  _option("option", usage_for_option, "STRING", true),
  _suboption("suboption", "see option", "STRING", false)
{
  _dcmdparser.add_dcmd_argument(&_option);
  _dcmdparser.add_dcmd_argument(&_suboption);
}

} // namespace sap
