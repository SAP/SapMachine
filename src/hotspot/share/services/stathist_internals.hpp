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

#include "memory/allocation.hpp"
#include "services/stathist.hpp"
#include "utilities/globalDefinitions.hpp"

namespace StatisticsHistory {

  typedef uint64_t value_t;
#define INVALID_VALUE   ((value_t)UINT64_MAX)

  struct record_t {
    time_t timestamp;
    value_t values[1]; // var sized
  };

  class ColumnList;

  class Column: public CHeapObj<mtInternal> {
    friend class ColumnList;

    const char* const _category;
    const char* const _header; // optional. May be NULL.
    const char* const _name;
    const char* const _description;

    // The following members are fixed by ColumnList when the Column is added to it.
    Column* _next;  // next column in table
    int _idx;       // position in table
    int _idx_cat;   // position in category
    int _idx_hdr;   // position under its header (if any, 0 otherwise)

  protected:

    Column(const char* category, const char* header, const char* name, const char* description);

    // Child classes implement this.
    // output stream can be NULL; in that case, method shall return number of characters it would have printed.
    virtual int do_print(outputStream* os, value_t value,
        value_t last_value, int last_value_age, const print_info_t* pi) const = 0;

  public:

    const char* category() const      { return _category; }
    const char* header() const        { return _header; }
    const char* name() const          { return _name; }
    const char* description() const   { return _description; }

    void print_value(outputStream* os, value_t value, value_t last_value,
        int last_value_age, int min_width, const print_info_t* pi) const;

    // Returns the number of characters this value needs to be printed.
    int calc_print_size(value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;

    // Returns the index (the position in the table) of this column.
    int index() const                 { return _idx; }
    int index_within_category_section() const { return _idx_cat; }
    int index_within_header_section() const   { return _idx_hdr; }

    const Column* next () const       { return _next; }

    virtual bool is_memory_size() const { return false; }
    virtual bool is_delta() const { return false; }


  };

  // Some standard column types

  class PlainValueColumn: public Column {
    int do_print(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    PlainValueColumn(const char* category, const char* header, const char* name, const char* description)
      : Column(category, header, name, description)
    {}
  };

  class DeltaValueColumn: public Column {
    const bool _show_only_positive;
    int do_print(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    // only_positive: only positive deltas are shown, negative deltas are supressed
    DeltaValueColumn(const char* category, const char* header, const char* name, const char* description,
        bool show_only_positive = true)
      : Column(category, header, name, description)
      , _show_only_positive(show_only_positive)
    {}
    bool is_delta() const { return true; }
  };

  class MemorySizeColumn: public Column {
    int do_print(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    MemorySizeColumn(const char* category, const char* header, const char* name, const char* description)
      : Column(category, header, name, description)
    {}
    bool is_memory_size() const { return true; }
  };

  class DeltaMemorySizeColumn: public Column {
    int do_print(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    DeltaMemorySizeColumn(const char* category, const char* header, const char* name, const char* description)
      : Column(category, header, name, description)
    {}
    bool is_memory_size() const { return true; }
    bool is_delta() const { return true; }
  };

  class ColumnList: public CHeapObj<mtInternal> {

    Column* _first, *_last;
    int _num_columns;

    static ColumnList* _the_list;

  public:

    ColumnList()
      : _first(NULL), _last(NULL), _num_columns(0)
    {}

    const Column* first() const { return _first; }
    int num_columns() const     { return _num_columns; }

    void add_column(Column* column);

    static ColumnList* the_list () { return _the_list; }

    static bool initialize();

#ifdef ASSERT
    bool is_valid_column_index(int idx) {
      return idx >= 0 && idx < _num_columns;
    }
#endif

  };

  // Implemented by platform specific
  bool platform_columns_initialize();

  void sample_platform_values(record_t* record);
  void sample_jvm_values(record_t* record, bool avoid_locking);

}; // namespace StatisticsHistory

#endif /* HOTSPOT_SHARE_SERVICES_STATHIST_INTERNALS_HPP */
