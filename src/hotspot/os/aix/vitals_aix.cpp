/*
 * Copyright (c) 2019, 2025 SAP SE. All rights reserved.
 *
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

#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "vitals/vitals_internals.hpp"

namespace sapmachine_vitals {

static Column* g_col_system_load_average = nullptr;

bool platform_columns_initialize() {
  g_col_system_load_average =
      define_column<PlainValueColumn>("system", nullptr, "la", "Load average in the sample interval in percent", true);

  return true;
}

void sample_platform_values(Sample* sample, Sample* long_term_sample) {
  set_load_average(g_col_system_load_average, get_load_avg_from_os_interface(), sample, long_term_sample);
}

} // namespace sapmachine_vitals
