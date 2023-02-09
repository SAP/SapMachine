/*
 * Copyright (c) 2022 SAP SE. All rights reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef OS_LINUX_VITALS_LINUX_PROCFS_HPP
#define OS_LINUX_VITALS_LINUX_PROCFS_HPP

#include "utilities/globalDefinitions.hpp"
#include "vitals/vitals_internals.hpp"

namespace sapmachine_vitals {

class OSWrapper {

  static time_t _last_update;

#define ALL_VALUES_DO(f) \
		f(syst_phys) \
	  f(syst_avail) \
	  f(syst_comm) \
	  f(syst_crt) \
	  f(syst_swap) \
	  f(syst_si) \
	  f(syst_so) \
	  f(syst_p) \
	  f(syst_t) \
	  f(syst_tr) \
	  f(syst_tb) \
	  f(syst_cpu_us) \
	  f(syst_cpu_sy) \
	  f(syst_cpu_id) \
	  f(syst_cpu_st) \
	  f(syst_cpu_gu) \
	  f(syst_cgro_lim) \
	  f(syst_cgro_limsw) \
	  f(syst_cgro_slim) \
	  f(syst_cgro_usg) \
	  f(syst_cgro_usgsw) \
	  f(syst_cgro_kusg) \
	  f(proc_virt) \
	  f(proc_rss_all) \
	  f(proc_rss_anon) \
	  f(proc_rss_file) \
	  f(proc_rss_shm) \
	  f(proc_swdo) \
	  f(proc_chea_usd) \
	  f(proc_chea_free) \
	  f(proc_cpu_us) \
	  f(proc_cpu_sy) \
	  f(proc_io_of) \
	  f(proc_io_rd) \
	  f(proc_io_wr) \
	  f(proc_thr) \

#define DECLARE_VARIABLE(name) \
  static value_t _##name;

ALL_VALUES_DO(DECLARE_VARIABLE)

#undef DECLARE_VARIABLE

public:

#define DEFINE_GETTER(name) \
  static value_t name() { return _ ## name; }

ALL_VALUES_DO(DEFINE_GETTER)

#undef DEFINE_GETTER

  static void update_if_needed();

  static bool initialize();

};

} // namespace sapmachine_vitals

#endif // OS_LINUX_VITALS_LINUX_HELPERS_HPP
