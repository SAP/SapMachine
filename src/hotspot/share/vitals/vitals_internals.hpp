/*
 * Copyright (c) 2019, 2023 SAP SE. All rights reserved.
 * Copyright (c) 2019, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef HOTSPOT_SHARE_VITALS_VITALS_INTERNALS_HPP
#define HOTSPOT_SHARE_VITALS_VITALS_INTERNALS_HPP

#include "memory/allocation.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "vitals/vitals.hpp"

namespace sapmachine_vitals {

  static const int vitals_version = 0x220600;

  typedef uint64_t value_t;
#define INVALID_VALUE   ((value_t)UINT64_MAX)

  class Sample {
    DEBUG_ONLY(int _num;)
    time_t _timestamp;
    value_t _values[1]; // var sized
  public:
    static int num_values();
    static size_t size_in_bytes();
    static Sample* allocate();

    void reset();
    void set_value(int index, value_t v);
    void set_timestamp(time_t t);
    DEBUG_ONLY(void set_num(int n);)

    value_t value(int index) const;
    time_t timestamp() const    { return _timestamp; }
    DEBUG_ONLY(int num() const  { return _num; })
  };

  class ColumnList;

  enum Extremum {
    NONE,
    MAX,
    MIN
  };

  class Column: public CHeapObj<mtInternal> {
    friend class ColumnList;

    const char* const _category;
    const char* const _header; // optional. May be NULL.
    const char* const _name;
    const char* const _description;
    const Extremum    _extremum;

    // The following members are fixed by ColumnList when the Column is added to it.
    Column* _next;  // next column in table
    int _idx;       // position in table
    int _idx_cat;   // position in category
    int _idx_hdr;   // position under its header (if any, 0 otherwise)

    int do_print(outputStream* os, value_t value, value_t last_value,
                 int last_value_age, const print_info_t* pi) const;

  protected:

    Column(const char* category, const char* header, const char* name, const char* description, Extremum extremum);

    // Child classes implement this.
    // output stream can be NULL; in that case, method shall return number of characters it would have printed.
    virtual int do_print0(outputStream* os, value_t value,
        value_t last_value, int last_value_age, const print_info_t* pi) const = 0;

  public:

    const char* category() const      { return _category; }
    const char* header() const        { return _header; }
    const char* name() const          { return _name; }
    const char* description() const   { return _description; }
    Extremum    extremum() const      { return _extremum; }

    void print_value(outputStream* os, value_t value, value_t last_value,
        int last_value_age, int min_width, const print_info_t* pi, char const* marker) const;

    // Returns the number of characters this value needs to be printed.
    int calc_print_size(value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;

    // Returns the index (the position in the table) of this column.
    int index() const                 { return _idx; }
    int index_within_category_section() const { return _idx_cat; }
    int index_within_header_section() const   { return _idx_hdr; }

    const Column* next () const       { return _next; }

    virtual bool is_memory_size() const   { return false; }

    static Extremum extremum_default()    { return NONE; }
  };

  // Some standard column types

  class PlainValueColumn: public Column {
    int do_print0(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    PlainValueColumn(const char* category, const char* header, const char* name, const char* description, Extremum extremum)
      : Column(category, header, name, description, extremum)
    {}
  };

  class DeltaValueColumn: public Column {
    int do_print0(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    // only_positive: only positive deltas are shown, negative deltas are supressed
    DeltaValueColumn(const char* category, const char* header, const char* name, const char* description, Extremum extremum)
      : Column(category, header, name, description, extremum)
    {}
  };

  class MemorySizeColumn: public Column {
    int do_print0(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    MemorySizeColumn(const char* category, const char* header, const char* name, const char* description, Extremum extremum)
      : Column(category, header, name, description, extremum)
    {}
    bool is_memory_size() const { return true; }
    static Extremum extremum_default() { return MAX; }
  };

  class DeltaMemorySizeColumn: public Column {
    int do_print0(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    DeltaMemorySizeColumn(const char* category, const char* header, const char* name, const char* description, Extremum extremum)
      : Column(category, header, name, description, extremum)
    {}
  };

  class TimeStampColumn: public Column {
    int do_print0(outputStream* os, value_t value, value_t last_value,
        int last_value_age, const print_info_t* pi) const;
  public:
    TimeStampColumn(const char* category, const char* header, const char* name, const char* description, Extremum extremum)
      : Column(category, header, name, description, extremum)
    {}
  };

  ////// Legend: handles the legend

  class Legend: public CHeapObj<mtInternal> {
    stringStream _legend;
    stringStream _footnote;
    static Legend* _the_legend;
    // needed during building the legend
    const char* _last_added_cat;
    int _nr_of_columns;
  public:
    Legend();
    void add_column_info(const char* const category, const char* const header,
                         const char* const name, const char* const description);
    void add_footnote(const char* text);
    void print_on(outputStream* st) const;
    int nr_of_columns() { return _nr_of_columns; }
    static Legend* the_legend () { return _the_legend; }
    static bool initialize();
  };

  ////// ColumnList: a singleton class holding all information about all columns

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

  // Convenient method to define and register a possibly deactivated column
  // (a deactivated column is not shown in the table, but still shown in the legend, to
  //  given the user a hint about it)
  template <class ColumnType>
  Column* define_column (
      const char* const category, const char* const header,
      const char* const name, const char* const description,
      bool is_active, Extremum extremum = ColumnType::extremum_default())
  {
    Column* c = NULL;
    if (is_active) {
      c = new ColumnType(category, header, name, description, extremum);
      ColumnList::the_list()->add_column(c);
    }
    Legend::the_legend()->add_column_info(category, header, name, description);
    return c;
  }

  // Ask platform to add platform specific columns
  bool platform_columns_initialize();

  void sample_platform_values(Sample* sample);
  void sample_jvm_values(Sample* sample, bool avoid_locking);

}; // namespace sapmachine_vitals

#endif /* HOTSPOT_SHARE_VITALS_VITALS_INTERNALS_HPP */
