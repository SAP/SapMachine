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

#ifndef HOTSPOT_SHARE_SERVICES_STATHIST_HPP
#define HOTSPOT_SHARE_SERVICES_STATHIST_HPP

#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace StatisticsHistory {

  bool initialize();
  void cleanup();

  struct print_info_t {
    bool raw;
    bool csv;
    // Omit printing a legend.
    bool no_legend;
    // Normally, when we print a report, we sample the current values too and print it atop of the table.
    // We may want to avoid that, e.g. during error handling.
    bool avoid_sampling;
    // Reverse printing order (default: youngest-to-oldest; reversed: oldest-to-youngest)
    bool reverse_ordering;

    size_t scale;
  };

  // If no print info is given (print_info == NULL), we print with default settings
  void print_report(outputStream* st, const print_info_t* print_info = NULL);

  // These are counters for the statistics history. Ideally, they would live
  // inside their thematical homes, e.g. thread.cpp or classLoaderDataGraph.cpp,
  // however since this is unlikely ever to be brought upstream we keep this separate
  // to easy maintenance.

  namespace counters {
    void inc_classes_loaded(size_t count);
    void inc_classes_unloaded(size_t count);
    void inc_threads_created(size_t count);
  };

};

#endif /* HOTSPOT_SHARE_SERVICES_STATHIST_HPP */
