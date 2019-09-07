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

#include "precompiled.hpp"


#include "gc/shared/collectedHeap.hpp"
#include "classfile/classLoaderDataGraph.inline.hpp"
#include "code/codeCache.hpp"
#include "memory/allocation.hpp"
#include "memory/universe.hpp"
#include "runtime/os.hpp"
#include "runtime/thread.hpp"
#include "services/memTracker.hpp"
#include "services/stathist.hpp"
#include "services/stathist_internals.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"


#include <locale.h>
#include <time.h>


// Define this switch to limit sampling to those values which can be obtained without locking.
#undef NEVER_LOCK_WHEN_SAMPLING

namespace StatisticsHistory {

namespace counters {

// These are counters for the statistics history. Ideally, they would live
// inside their thematical homes, e.g. thread.cpp or classLoaderDataGraph.cpp,
// however since this is unlikely ever to be brought upstream we keep them separate
// from central coding to ease maintenance.
static volatile size_t g_classes_loaded = 0;
static volatile size_t g_classes_unloaded = 0;
static volatile size_t g_threads_created = 0;

void inc_classes_loaded(size_t count) {
  Atomic::add(count, &g_classes_loaded);
}

void inc_classes_unloaded(size_t count) {
  Atomic::add(count, &g_classes_unloaded);
}

void inc_threads_created(size_t count) {
  Atomic::add(count, &g_threads_created);
}

} // namespace counters

// helper function for the missing outputStream::put(int c, int repeat)
static void ostream_put_n(outputStream* st, int c, int repeat) {
  for (int i = 0; i < repeat; i ++) {
    st->put(c);
  }
}

static size_t record_size_in_bytes() {
  const int num_columns = ColumnList::the_list()->num_columns();
  return sizeof(record_t) + sizeof(value_t) * (num_columns - 1);
}

static void print_text_with_dashes(outputStream* st, const char* text, int width) {
  assert(width > 0, "Sanity");
  // Print the name centered within the width like this
  // ----- system ------
  int extra_space = width - (int)strlen(text);
  if (extra_space > 0) {
    int left_space = extra_space / 2;
    int right_space = extra_space - left_space;
    ostream_put_n(st, '-', left_space);
    st->print_raw(text);
    ostream_put_n(st, '-', right_space);
  } else {
    ostream_put_n(st, '-', width);
  }
}

// Helper function for printing:
// Print to ostream, but only if ostream is given. In any case return number ofcat_process = 10,
// characters printed (or which would have been printed).
static
ATTRIBUTE_PRINTF(2, 3)
int printf_helper(outputStream* st, const char *fmt, ...) {
  // We only print numbers, so a small buffer is fine.
  char buf[64];
  va_list args;
  int len = 0;
  va_start(args, fmt);
  len = jio_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  assert((size_t)len < sizeof(buf), "Truncation. Increase bufsize.");
  if (st != NULL) {
    st->print_raw(buf);
  }
  return len;
}

// length of time stamp
#define TIMESTAMP_LEN 19
// number of spaces after time stamp
#define TIMESTAMP_DIVIDER_LEN 3
static void print_timestamp(outputStream* st, time_t t) {
  struct tm _tm;
  if (os::localtime_pd(&t, &_tm) == &_tm) {
    char buf[32];
    ::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &_tm);
    st->print("%*s", TIMESTAMP_LEN, buf);
  }
}

////// class ColumnList methods ////

ColumnList* ColumnList::_the_list = NULL;

bool ColumnList::initialize() {
  _the_list = new ColumnList();
  return _the_list != NULL;
}

void ColumnList::add_column(Column* c) {
  assert(c->index() == -1, "Do not add twice.");
  Column* c_last = _last;
  if (_last != NULL) {
    _last->_next = c;
    _last = c;
  } else {
    _first = _last = c;
  }
  // fix indices (describe position of column within table/category/header
  c->_idx = c->_idx_cat = c->_idx_hdr = 0;
  if (c_last != NULL) {
    c->_idx = c_last->_idx + 1;
    if (::strcmp(c->category(), c_last->category()) == 0) { // same category as last column?
      c->_idx_cat = c_last->_idx_cat + 1;
    }
    if (c->header() != NULL && c_last->header() != NULL &&
        ::strcmp(c_last->header(), c->header()) == 0) { // have header and same as last column?
      c->_idx_hdr = c_last->_idx_hdr + 1;
    }
  }
  _num_columns ++;
}

////////////////////

// At various places we need a scratch buffer of ints with one element per column; since
// after initialization the number of columns is fixed, we pre-create this.
static int* g_widths = NULL;

bool initialize_widths_buffer() {
  assert(ColumnList::the_list() != NULL && g_widths == NULL, "Initialization order problem.");
  g_widths = (int*)os::malloc(sizeof(int) * ColumnList::the_list()->num_columns(), mtInternal);
  return g_widths != NULL;
}

// pre-allocated space for a single record used to take current values when printing via dcmd.
static record_t* g_record_now = NULL;

bool initialize_space_for_now_record() {
  assert(ColumnList::the_list() != NULL && g_record_now == NULL, "Initialization order problem.");
  g_record_now = (record_t*) os::malloc(record_size_in_bytes(), mtInternal);
  return g_record_now != NULL;
}

////////////////////

static void print_category_line(outputStream* st, int widths[], const print_info_t* pi) {

  assert(pi->csv == false, "Not in csv mode");
  ostream_put_n(st, ' ', TIMESTAMP_LEN + TIMESTAMP_DIVIDER_LEN);

  const Column* c = ColumnList::the_list()->first();
  assert(c != NULL, "no columns?");
  const char* last_category_text = NULL;
  int width = 0;

  while(c != NULL) {
    if (c->index_within_category_section() == 0) {
      if (width > 0) {
        // Print category label centered over the last n columns, surrounded by dashes.
        print_text_with_dashes(st, last_category_text, width - 1);
        st->put(' ');
      }
      width = 0;
    }
    width += widths[c->index()];
    width += 1; // divider between columns
    last_category_text = c->category();
    c = c->next();
  }
  print_text_with_dashes(st, last_category_text, width - 1);
  st->cr();
}

static void print_header_line(outputStream* st, int widths[], const print_info_t* pi) {

  assert(pi->csv == false, "Not in csv mode");
  ostream_put_n(st, ' ', TIMESTAMP_LEN + TIMESTAMP_DIVIDER_LEN);

  const Column* c = ColumnList::the_list()->first();
  assert(c != NULL, "no columns?");
  const char* last_header_text = NULL;
  int width = 0;

  while(c != NULL) {
    if (c->index_within_header_section() == 0) { // First in header section
      if (width > 0) {
        if (last_header_text != NULL) {
          // Print header label centered over the last n columns, surrounded by dashes.
          print_text_with_dashes(st, last_header_text, width - 1);
          st->put(' '); // divider
        } else {
          // the last n columns had no header. Just fill with blanks.
          ostream_put_n(st, ' ', width);
        }
      }
      width = 0;
    }
    width += widths[c->index()];
    width += 1; // divider between columns
    last_header_text = c->header();
    c = c->next();
  }
  if (width > 0 && last_header_text != NULL) {
    print_text_with_dashes(st, last_header_text, width - 1);
  }
  st->cr();
}

static void print_column_names(outputStream* st, int widths[], const print_info_t* pi) {

  // Leave space for timestamp column
  if (pi->csv == false) {
    ostream_put_n(st, ' ', TIMESTAMP_LEN + TIMESTAMP_DIVIDER_LEN);
  } else {
    st->put(',');
  }

  const Column* c = ColumnList::the_list()->first();
  const Column* previous = NULL;
  while (c != NULL) {
    if (pi->csv == false) {
      st->print("%-*s ", widths[c->index()], c->name());
    } else { // csv mode
      // csv: use comma as delimiter, don't pad, and precede name with header if there is one.
      if (c->header() != NULL) {
        st->print("%s-", c->header());
      }
      st->print("%s,", c->name());
    }
    previous = c;
    c = c->next();
  }
  st->cr();
}

static void print_legend(outputStream* st, const print_info_t* pi) {
  const Column* c = ColumnList::the_list()->first();
  const Column* c_prev = NULL;
  while (c != NULL) {
    // Print category label.
    if (c->index_within_category_section() == 0) {
      print_text_with_dashes(st, c->category(), 30);
      st->cr();
    }
    // print column name and description
    const int min_width_column_label = 16;
    char buf[32];
    if (c->header() != NULL) {
      jio_snprintf(buf, sizeof(buf), "%s-%s", c->header(), c->name());
    } else {
      jio_snprintf(buf, sizeof(buf), "%s", c->name());
    }
    st->print("%*s: %s", min_width_column_label, buf, c->description());

    // If memory units are not dynamic (otion scale), print out the unit as well.
    if (c->is_memory_size() && pi->scale != 0) {
      st->print_raw(" [mem]");
    }

    // If column is a delta value, indicate so
    if (c->is_delta()) {
      st->print_raw(" [delta]");
    }

    st->cr();

    c_prev = c;
    c = c->next();
  }
  st->cr();
  st->print_cr("[delta] values refer to the previous measurement.");
  if (pi->scale != 0) {
    const char* display_unit = NULL;
    switch (pi->scale) {
      case 1: display_unit = "  "; break;
      case K: display_unit = "KB"; break;
      case M: display_unit = "MB"; break;
      case G: display_unit = "GB"; break;
      default: ShouldNotReachHere();
    }
    st->print_cr("[mem] values are in %s.", display_unit);
  }
}

// Print a human readable size.
// byte_size: size, in bytes, to be printed.
// scale: K,M,G or 0 (dynamic)
// width: printing width.
static int print_memory_size(outputStream* st, size_t byte_size, size_t scale)  {

  // If we forced a unit via scale=.. argument, we suppress display of the unit
  // since we already know which unit is used. That saves horizontal space and
  // makes automatic processing of the data easier.
  bool print_unit = false;

  if (scale == 0) {
    print_unit = true;
    // Dynamic mode. Choose scale for this value.
    if (byte_size == 0) {
      scale = K;
    } else {
      if (byte_size >= G) {
        scale = G;
      } else if (byte_size >= M) {
        scale = M;
      } else {
        scale = K;
      }
    }
  }

  const char* display_unit = "";
  if (print_unit) {
    switch(scale) {
      case K: display_unit = "k"; break;
      case M: display_unit = "m"; break;
      case G: display_unit = "g"; break;
      default:
        ShouldNotReachHere();
    }
  }

  int l = 0;
  float display_value = (float) byte_size / scale;
  // Values smaller than 1M are shown are rounded up to whole numbers to de-clutter
  // the display. Who cares for half kbytes.
  int precision = scale < G ? 0 : 1;

  if (scale > 1 && byte_size > 0 && byte_size < K) {
    // Prevent values smaller than one K but not 0 showing up as .
    l = printf_helper(st, "<1%s", display_unit);
  } else {
    l = printf_helper(st, "%.*f%s", precision, display_value, display_unit);
  }
  return l;

}

///////// class Column and childs ///////////

Column::Column(const char* category, const char* header, const char* name, const char* description)
  : _category(category),
    _header(header), // may be NULL
    _name(name),
    _description(description),
    _next(NULL), _idx(-1),
    _idx_cat(-1), _idx_hdr(-1)
{
  ColumnList::the_list()->add_column(this);
}

void Column::print_value(outputStream* st, value_t value, value_t last_value,
    int last_value_age, int min_width, const print_info_t* pi) const {

  if (pi->raw) {
    printf_helper(st, UINT64_FORMAT, value);
    return;
  }

  // We print all values right aligned.
  int needed = calc_print_size(value, last_value, last_value_age, pi);
  if (pi->csv == false && min_width > needed) {
    // In ascii (non csv) mode, pad to minimum width
    ostream_put_n(st, ' ', min_width - needed);
  }
  // csv values shall be enclosed in quotes.
  if (pi->csv) {
    st->put('"');
  }
  do_print(st, value, last_value, last_value_age, pi);
  if (pi->csv) {
    st->put('"');
  }
}

// Returns the number of characters this value needs to be printed.
int Column::calc_print_size(value_t value, value_t last_value,
    int last_value_age, const print_info_t* pi) const {
  return do_print(NULL, value, last_value, last_value_age, pi);
}

int PlainValueColumn::do_print(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const
{
  int l = 0;
  if (value != INVALID_VALUE) {
    l = printf_helper(st, UINT64_FORMAT, value);
  }
  return l;
}

int DeltaValueColumn::do_print(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const {
  if (_show_only_positive && last_value > value) {
    // we assume the underlying value to be monotonically raising, and that
    // any negative delta would be just a fluke (e.g. counter overflows)
    // we do not want to show
    return 0;
  }
  int l = 0;
  if (value != INVALID_VALUE && last_value != INVALID_VALUE) {
    l = printf_helper(st, INT64_FORMAT, (int64_t)(value - last_value));
  }
  return l;
}

int MemorySizeColumn::do_print(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const {
  int l = 0;
  if (value != INVALID_VALUE) {
    l = print_memory_size(st, value, pi->scale);
  }
  return l;
}

int DeltaMemorySizeColumn::do_print(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const {
  int l = 0;
  if (value != INVALID_VALUE && last_value != INVALID_VALUE) {
    l = print_memory_size(st, value - last_value, pi->scale);
  }
  return l;
}

////////////// Record printing ///////////////////////////

// Print one record.
static void print_one_record(outputStream* st, const record_t* record,
    const record_t* last_record, const int widths[], const print_info_t* pi) {

  // Print timestamp and divider
  if (record->timestamp == 0) {
    st->print("%*s", TIMESTAMP_LEN, "Now");
  } else {
    print_timestamp(st, record->timestamp);
  }

  if (pi->csv == false) {
    ostream_put_n(st, ' ', TIMESTAMP_DIVIDER_LEN);
  } else {
    st->put(',');
  }

  const Column* c = ColumnList::the_list()->first();
  while (c != NULL) {
    const int idx = c->index();
    const value_t v = record->values[idx];
    value_t v2 = INVALID_VALUE;
    int age = -1;
    if (last_record != NULL) {
      v2 = last_record->values[idx];
      age = record->timestamp - last_record->timestamp;
    }
    const int min_width = widths[idx];
    c->print_value(st, v, v2, age, min_width, pi);
    st->put(pi->csv ? ',' : ' ');
    c = c->next();
  }
  st->cr();
}

// For each value in record, update the width in the widths array if it is smaller than
// the value printing width
static void update_widths_from_one_record(const record_t* record, const record_t* last_record, int widths[],
    const print_info_t* pi) {
  const Column* c = ColumnList::the_list()->first();
  while (c != NULL) {
    const int idx = c->index();
    const value_t v = record->values[idx];
    value_t v2 = INVALID_VALUE;
    int age = -1;
    if (last_record != NULL) {
      v2 = last_record->values[idx];
      age = record->timestamp - last_record->timestamp;
    }
    int needed = c->calc_print_size(v, v2, age, pi);
    if (widths[idx] < needed) {
      widths[idx] = needed;
    }
    c = c->next();
  }
}

////////////// Class RecordTable /////////////////////////

class RecordTable : public CHeapObj<mtInternal> {

  const int _num_records;

  record_t* _records;

  // _pos: index of next slot to write to. If we did not yet wrap,
  //  this position is invalid, otherwise this is the position of the oldest slot.
  //
  // Valid ranges: not yet wrapped:    [0 .. _pos)
  //               wrapped:            [_pos ... (_num_records-1 -> 0) ... _pos)
  int _pos;
  bool _did_wrap;

  RecordTable* const _follower;
  const int _follower_ratio;
  int _follower_countdown;

  #ifdef ASSERT
    bool valid_pos(int pos) const           { return pos >= 0 && pos < _num_records; }
    bool valid_pos_or_null(int pos) const   { return pos == -1 || valid_pos(pos); }
  #endif

  // Returns the position of the last slot we wrote to.
  // -1 if this table is empty.
  int youngest_pos() const {
    int pos = _pos - 1;
    if (pos == -1) {
      if (_did_wrap) {
        pos = _num_records - 1;
      }
    }
    return pos;
  }

  // Returns the position of the oldest slot we wrote to.
  // -1 if this table is empty.
  int oldest_pos() const {
    if (_did_wrap) {
      return _pos;
    } else {
      return _pos > 0 ? 0 : -1;
    }
  }

  // Return the position following pos.
  //  Input position must be a valid position.
  //  Returns -1 if there is no following slot.
  int following_pos(int pos) const {
    assert(valid_pos(pos), "Sanity");
    int p2 = pos + 1;
    if (_did_wrap) {
      if (p2 == _num_records) {
        p2 = 0;
      }
    }
    if (p2 == _pos) {
      p2 = -1;
    }
    assert(valid_pos_or_null(p2), "Sanity");
    return p2;
  }

  int preceeding_pos(int pos) const {
    assert(valid_pos(pos), "Sanity");
    int p2 = pos - 1;
    if (p2 == -1) {
      if (_did_wrap) {
        p2 = _num_records - 1;
      }
    } else if (p2 == _pos) {
      assert(_did_wrap, "Sanity");
      p2 = -1;
    }
    assert(valid_pos_or_null(p2), "Sanity");
    return p2;
  }

  record_t* at(int pos) const {
    assert(valid_pos(pos), "Sanity");
    return (record_t*) ((address)_records + (record_size_in_bytes() * pos));
  }

  // May return NULL
  const record_t* preceeding(int pos) const {
    int p2 = preceeding_pos(pos);
    if (p2 != -1) {
      return at(p2);
    }
    return NULL;
  }

  class ConstIterator {
    const RecordTable* const _rt;
    const bool _reverse;
    int _p;

    void move_to_oldest() {
      _p = _rt->oldest_pos();
    }

    void move_to_youngest() {
      _p = _rt->youngest_pos();
    }

    void step_forward() {
      if (valid()) {
        _p = _rt->following_pos(_p);
      }
    }

    void step_back() {
      if (valid()) {
        _p = _rt->preceeding_pos(_p);
      }
    }

  public:

    ConstIterator(const RecordTable* rt, bool reverse = false)
      : _rt(rt), _reverse(reverse), _p(-1)
    {
      if (_reverse) {
        move_to_oldest();
      } else {
        move_to_youngest();
      }
    }

    bool valid() const { return _p != -1; }

    void step() {
      if (_reverse) {
        step_forward();
      } else {
        step_back();
      }
    }

    const record_t* get() const {
      assert(valid(), "Sanity");
      return _rt->at(_p);
    }

    // Returns the record directly preceding, age-wise, the current
    // record. This has nothing to do with iteratore
    // May return NULL
    const record_t* get_preceeding() const {
      return _rt->preceeding(_p);
    }

  }; // end ConstReverseIterator


  void add_record(const record_t* record) {
    ::memcpy(current_record(), record, record_size_in_bytes());
    finish_current_record();
  }

  // This "dry-prints" all records just to calculate the maximum print width, per column, needed to
  // display the values.
  void update_widths_from_all_records(int widths[], const print_info_t* pi) const {

    // reset widths - note: minimum width is the length of the name of the column.
    const Column* c = ColumnList::the_list()->first();
    while (c != NULL) {
      widths[c->index()] = (int)::strlen(c->name());
      c = c->next();
    }

    ConstIterator it(this);
    while(it.valid()) {
      const record_t* record = it.get();
      const record_t* previous_record = it.get_preceeding();
      update_widths_from_one_record(record, previous_record, widths, pi);
      it.step();
    }
  }

  // Print all records.
  void print_all_records(outputStream* st, const int widths[], const print_info_t* pi) const {
    ConstIterator it(this, pi->reverse_ordering);
    while(it.valid()) {
      const record_t* record = it.get();
      const record_t* previous_record = it.get_preceeding();
      print_one_record(st, record, previous_record, widths, pi);
      it.step();
    }
  }

  bool is_empty() const {
    return _pos == 0 && _did_wrap == false;
  }

public:

  RecordTable(int num_records, RecordTable* follower = NULL, int follower_ratio = -1)
    : _num_records(num_records),
      _records(NULL), _pos(0), _did_wrap(false),
      _follower(follower), _follower_ratio(follower_ratio),
      _follower_countdown(0)
  {}

  bool initialize() {
    _records = (record_t*) os::malloc(record_size_in_bytes() * _num_records, mtInternal);
    return _records != NULL;
  }

  // returns the pointer to the current, unfinished record.
  record_t* current_record() {
    return at(_pos);
  }

  // finish the current record: advances the write position in the FIFO buffer by one.
  // Should that cause a record to fall out of the FIFO end, it propagates the record to
  // the follower table if needed.
  void finish_current_record() {
    _pos ++;
    if (_pos == _num_records) {
      _pos = 0;
      _did_wrap = true;
    }
    if (_did_wrap) {
      // propagate old record if needed.
      if (_follower != NULL && _follower_countdown == 0) {
        _follower->add_record(current_record());
        _follower_countdown = _follower_ratio; // reset countdown.
      }
      _follower_countdown --; // count down.
    }
  }

  void print_table(outputStream* st, const print_info_t* pi, const record_t* values_now = NULL) const {

    if (is_empty() && values_now == NULL) {
      st->print_cr("(no records)");
      return;
    }

    const record_t* const youngest_in_table = preceeding(_pos);

    // Before actually printing, lets calculate the printing widths
    // (if now-values are given, they are part of the table too, so include them in the widths calculation)
    update_widths_from_all_records(g_widths, pi);
    if (values_now != NULL) {
      update_widths_from_one_record(values_now, youngest_in_table, g_widths, pi);
    }

    // Print headers (not in csv mode)
    if (pi->csv == false) {
      print_category_line(st, g_widths, pi);
      print_header_line(st, g_widths, pi);
    }
    print_column_names(st, g_widths, pi);
    st->cr();

    // Now print the actual values. We preceede the table values with the now value
    //  (if printing order is youngest-to-oldest) or write it last (if printing order is oldest-to-youngest).
    if (values_now != NULL && pi->reverse_ordering == false) {
      print_one_record(st, values_now, youngest_in_table, g_widths, pi);
    }
    print_all_records(st, g_widths, pi);
    if (values_now != NULL && pi->reverse_ordering == true) {
      print_one_record(st, values_now, youngest_in_table, g_widths, pi);
    }

  }
};

class RecordTables: public CHeapObj<mtInternal> {

  enum {
    // short term: 15 seconds per sample, 60 samples or 15 minutes total
    short_term_interval_default = 15,
    short_term_num_samples = 60,

    // mid term: 15 minutes per sample (aka 60 short term samples), 96 samples or 24 hours in total
    mid_term_interval_ratio = 60,
    mid_term_num_samples = 96,

    // long term history: 2 hour intervals (aka 8 mid term samples), 120 samples or 10 days in total
    long_term_interval_ratio = 8,
    long_term_num_samples = 120
  };

  int _short_term_interval;

  RecordTable* _short_term_table;
  RecordTable* _mid_term_table;
  RecordTable* _long_term_table;

  static RecordTables* _the_tables;

  // Call this after column list has been initialized.
  bool initialize(int short_term_interval) {

    // Calculate intervals
    _short_term_interval = short_term_interval;

    // Initialize tables, oldest first (since it has no follower)
    _long_term_table = new RecordTable(long_term_num_samples);
    if (_long_term_table == NULL || !_long_term_table->initialize()) {
      return false;
    }
    _mid_term_table = new RecordTable(mid_term_num_samples,
      _long_term_table, long_term_interval_ratio);
    if (_mid_term_table == NULL || !_mid_term_table->initialize()) {
      return false;
    }
    _short_term_table = new RecordTable(short_term_num_samples,
        _mid_term_table, mid_term_interval_ratio);
    if (_short_term_table == NULL || !_short_term_table->initialize()) {
      return false;
    }

    return true;

  }

public:

  static RecordTables* the_tables() { return _the_tables; }

  // Call this after column list has been initialized.
  static bool initialize() {

    _the_tables = new RecordTables();
    if (_the_tables == NULL) {
      return false;
    }

    const int short_term_interval = VitalsSampleInterval != 0 ?
        VitalsSampleInterval : short_term_interval_default;
    return _the_tables->initialize(short_term_interval);

  }

  void print_all(outputStream* st, const print_info_t* pi, const record_t* values_now = NULL) const {

    st->print_cr("Short Term Values:");
    // At the start of the short term table we print the current (now) values. The intent is to be able
    // to see very short term developments (e.g. a spike in heap usage in the last n seconds)
    _short_term_table->print_table(st, pi, values_now);
    st->cr();

    st->print_cr("Mid Term Values:");
    _mid_term_table->print_table(st, pi);
    st->cr();

    st->print_cr("Long Term Values:");
    _long_term_table->print_table(st, pi);
    st->cr();

  }

  RecordTable* first_table() const {
    return _short_term_table;
  }

  int short_term_interval() const {
    return _short_term_interval;
  }

};

RecordTables* RecordTables::_the_tables = NULL;

static void sample_values(record_t* record, bool avoid_locking) {

  // reset all values to be invalid.
  const ColumnList* clist = ColumnList::the_list();
  for (int colno = 0; colno < clist->num_columns(); colno ++) {
    record->values[colno] = INVALID_VALUE;
  }

  // sample...
  sample_jvm_values(record, avoid_locking);
  sample_platform_values(record);
}

class SamplerThread: public NamedThread {

  bool _stop;

  void take_sample() {

    const ColumnList* clist = ColumnList::the_list();
    RecordTable* record_table = RecordTables::the_tables()->first_table();
    record_t* record = record_table->current_record();

    ::time(&record->timestamp);

    sample_values(record, VitalsLockFreeSampling);

    // After sampling, finish record.
    record_table->finish_current_record();

  }

public:

  SamplerThread()
    : NamedThread()
    , _stop(false)
  {
    this->set_name("vitals sampler thread");
  }

  virtual void run() {
    record_stack_base_and_size();
    for (;;) {
      take_sample();
      os::sleep(this, RecordTables::the_tables()->short_term_interval() * 1000, false);
      if (_stop) {
        break;
      }
    }
  }

  void stop() {
    _stop = true;
  }

};

static SamplerThread* g_sampler_thread = NULL;

static bool initialize_sampler_thread() {
  g_sampler_thread = new SamplerThread();
  if (g_sampler_thread != NULL) {
    if (os::create_thread(g_sampler_thread, os::os_thread)) {
      os::start_thread(g_sampler_thread);
    }
    return true;
  }
  return false;
}


/////// JVM-specific columns //////////

static Column* g_col_heap_committed = NULL;
static Column* g_col_heap_used = NULL;

static Column* g_col_metaspace_committed = NULL;
static Column* g_col_metaspace_used = NULL;
static Column* g_col_classspace_committed = NULL;
static Column* g_col_classspace_used = NULL;
static Column* g_col_metaspace_cap_until_gc = NULL;

static Column* g_col_codecache_committed = NULL;

static Column* g_col_nmt_malloc = NULL;

static Column* g_col_number_of_java_threads = NULL;
static Column* g_col_number_of_java_threads_non_demon = NULL;
static Column* g_col_size_thread_stacks = NULL;
static Column* g_col_number_of_java_threads_created = NULL;

static Column* g_col_number_of_clds = NULL;
static Column* g_col_number_of_anon_clds = NULL;

static Column* g_col_number_of_classes = NULL;
static Column* g_col_number_of_class_loads = NULL;
static Column* g_col_number_of_class_unloads = NULL;

//...

static bool add_jvm_columns() {
  // Order matters!

  g_col_heap_committed = new MemorySizeColumn("jvm",
      "heap", "comm", "Java Heap Size, committed");
  g_col_heap_used = new MemorySizeColumn("jvm",
      "heap", "used", "Java Heap Size, used");

  g_col_metaspace_committed = new MemorySizeColumn("jvm",
      "meta", "comm", "Meta Space Size (class+nonclass), committed");

  g_col_metaspace_used = new MemorySizeColumn("jvm",
      "meta", "used", "Meta Space Size (class+nonclass), used");

  if (Metaspace::using_class_space()) {
    g_col_classspace_committed = new MemorySizeColumn("jvm",
        "meta", "csc", "Class Space Size, committed");
    g_col_classspace_used = new MemorySizeColumn("jvm",
        "meta", "csu", "Class Space Size, used");
  }

  g_col_metaspace_cap_until_gc = new MemorySizeColumn("jvm",
      "meta", "gctr", "GC threshold");

  g_col_codecache_committed = new MemorySizeColumn("jvm",
      NULL, "code", "Code cache, committed");

  g_col_nmt_malloc = new MemorySizeColumn("jvm",
      NULL, "mlc", "Memory malloced by hotspot (requires NMT)");

  g_col_number_of_java_threads = new PlainValueColumn("jvm",
      "jthr", "num", "Number of java threads");

  g_col_number_of_java_threads_non_demon = new PlainValueColumn("jvm",
      "jthr", "nd", "Number of non-demon java threads");

  g_col_number_of_java_threads_created = new DeltaValueColumn("jvm",
      "jthr", "cr", "Threads created");

  g_col_size_thread_stacks = new MemorySizeColumn("jvm",
      "jthr", "st", "Total reserved size of java thread stacks");

  g_col_number_of_clds = new PlainValueColumn("jvm",
      "cldg", "num", "Classloader Data");

  g_col_number_of_anon_clds = new PlainValueColumn("jvm",
      "cldg", "anon", "Anonymous CLD");

  g_col_number_of_classes = new PlainValueColumn("jvm",
      "cls", "num", "Classes (instance + array)");

  g_col_number_of_class_loads = new DeltaValueColumn("jvm",
      "cls", "ld", "Class loaded");

  g_col_number_of_class_unloads = new DeltaValueColumn("jvm",
      "cls", "uld", "Classes unloaded");

  return true;
}


////////// class ValueSampler and childs /////////////////

template <typename T>
static void set_value_in_record(const Column* col, record_t* r, T t) {
  if (col != NULL) {
    int idx = col->index();
    assert(ColumnList::the_list()->is_valid_column_index(idx), "Invalid column index");
    r->values[idx] = (value_t)t;
  }
}

class AddStackSizeThreadClosure: public ThreadClosure {
  size_t _l;
public:
  AddStackSizeThreadClosure() : ThreadClosure(), _l(0) {}
  void do_thread(Thread* thread) {
    _l += thread->stack_size();
  }
  size_t get() const { return _l; }
};

static size_t accumulate_thread_stack_size() {
#if defined(LINUX) || defined(__APPLE__)
  // Do not iterate thread list and query stack size until 8212173 is completely solved. It is solved
  // for BSD and Linux; on the other platforms, one runs a miniscule but real risk of triggering
  // the assert in Thread::stack_size().
  size_t l = 0;
  AddStackSizeThreadClosure tc;
  {
    MutexLocker ml(Threads_lock);
    Threads::threads_do(&tc);
  }
  return tc.get();
#else
  return INVALID_VALUE;
#endif
}

// Count CLDs
class CLDCounterClosure: public CLDClosure {
public:
  int _cnt;
  int _anon_cnt;
  CLDCounterClosure() : _cnt(0), _anon_cnt(0) {}
  void do_cld(ClassLoaderData* cld) {
    _cnt ++;
    if (cld->is_unsafe_anonymous()) {
      _anon_cnt ++;
    }
  }
};

void sample_jvm_values(record_t* record, bool avoid_locking) {

  // Heap
  if (!avoid_locking) {
    size_t heap_cap = 0;
    size_t heap_used = 0;
    const CollectedHeap* const heap = Universe::heap();
    if (heap != NULL) {
      MutexLocker hl(Heap_lock);
      heap_cap = Universe::heap()->capacity();
      heap_used = Universe::heap()->used();
    }
    set_value_in_record(g_col_heap_committed, record, heap_cap);
    set_value_in_record(g_col_heap_used, record, heap_used);
  }

  // Metaspace
  set_value_in_record(g_col_metaspace_committed, record, MetaspaceUtils::committed_bytes());
  set_value_in_record(g_col_metaspace_used, record, MetaspaceUtils::used_bytes());

  if (Metaspace::using_class_space()) {
    set_value_in_record(g_col_classspace_committed, record, MetaspaceUtils::committed_bytes(Metaspace::ClassType));
    set_value_in_record(g_col_classspace_used, record, MetaspaceUtils::used_bytes(Metaspace::ClassType));
  }

  set_value_in_record(g_col_metaspace_cap_until_gc, record, MetaspaceGC::capacity_until_GC());

  // Code cache
  const size_t codecache_committed = CodeCache::capacity();
  set_value_in_record(g_col_codecache_committed, record, codecache_committed);

  // NMT
  if (!avoid_locking) {
    size_t malloc_footprint = 0;
    if (MemTracker::tracking_level() != NMT_off) {
      MutexLocker locker(MemTracker::query_lock());
      malloc_footprint = MallocMemorySummary::as_snapshot()->total();
    }
    set_value_in_record(g_col_nmt_malloc, record, malloc_footprint);
  }

  // Java threads
  set_value_in_record(g_col_number_of_java_threads, record, Threads::number_of_threads());
  set_value_in_record(g_col_number_of_java_threads_non_demon, record, Threads::number_of_non_daemon_threads());
  set_value_in_record(g_col_number_of_java_threads_created, record, counters::g_threads_created);

  // Java thread stack size
  if (!avoid_locking) {
    set_value_in_record(g_col_size_thread_stacks, record, accumulate_thread_stack_size());
  }

  // CLDG
  if (!avoid_locking) {
    CLDCounterClosure cl;
    {
      MutexLocker lck(ClassLoaderDataGraph_lock);
      ClassLoaderDataGraph::cld_do(&cl);
    }
    set_value_in_record(g_col_number_of_clds, record, cl._cnt);
    set_value_in_record(g_col_number_of_anon_clds, record, cl._anon_cnt);
  }

  // Classes
  set_value_in_record(g_col_number_of_classes, record,
      ClassLoaderDataGraph::num_instance_classes() + ClassLoaderDataGraph::num_array_classes());
  set_value_in_record(g_col_number_of_class_loads, record, counters::g_classes_loaded);
  set_value_in_record(g_col_number_of_class_unloads, record, counters::g_classes_unloaded);
}

bool initialize() {

  if (!ColumnList::initialize()) {
    return false;
  }

  // Order matters. First platform columns, then jvm columns.
  if (!platform_columns_initialize()) {
    return false;
  }

  if (!add_jvm_columns()) {
    return false;
  }

  // -- Now the number of columns is known (and fixed). --

  if (!initialize_widths_buffer()) {
    return false;
  }

  if (!initialize_space_for_now_record()) {
    return false;
  }

  if (!RecordTables::initialize()) {
    return false;
  }

  if (!initialize_sampler_thread()) {
    return false;
  }

  return true;

}

void cleanup() {
  if (g_sampler_thread != NULL) {
    g_sampler_thread->stop();
  }
}

void print_report(outputStream* st, const print_info_t* pi) {

  st->print("Vitals:");

  if (ColumnList::the_list() == NULL) {
    st->print_cr(" (unavailable)");
    return;
  }

  st->cr();

  static const print_info_t default_settings = {
      false, // raw
      false, // csv
      false, // omit_legend
      true   // avoid_sampling
  };

  if (pi == NULL) {
    pi = &default_settings;
  }

  // Print legend at the top (omit if suppressed on command line, or in csv mode).
  if (pi->no_legend == false && pi->csv == false) {
    print_legend(st, pi);
    st->cr();
  }

  record_t* values_now = NULL;
  if (!pi->avoid_sampling) {
    // Sample the current values (not when reporting errors, since we do not want to risk secondary errors).
    values_now = g_record_now;
    values_now->timestamp = 0; // means "Now"
    sample_values(values_now, true);
  }

  RecordTables::the_tables()->print_all(st, pi, values_now);

}

// Dump both textual and csv style reports to two files, "vitals_<pid>.txt" and "vitals_<pid>.csv".
// If these files exist, they are overwritten.
void dump_reports() {

  char vitals_file_name[64];

  os::snprintf(vitals_file_name, sizeof(vitals_file_name), "vitals_%d.txt", os::current_process_id());
  ::printf("Dumping Vitals to %s.\n", vitals_file_name);
  print_info_t pi;
  memset(&pi, 0, sizeof(pi));
  pi.avoid_sampling = true; // this is called during exit, so lets be a bit careful.
  {
    fileStream fs(vitals_file_name);
    print_report(&fs, &pi);
  }

  os::snprintf(vitals_file_name, sizeof(vitals_file_name), "vitals_%d.csv", os::current_process_id());
  ::printf("Dumping Vitals csv to %s.\n", vitals_file_name);
  pi.csv = true;
  pi.scale = 1 * K;
  pi.reverse_ordering = true;
  {
    fileStream fs(vitals_file_name);
    print_report(&fs, &pi);
  }

}

} // namespace StatisticsHistory
