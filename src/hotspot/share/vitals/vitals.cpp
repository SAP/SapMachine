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

#include "precompiled.hpp"

#include "jvm_io.h"

#include "gc/shared/collectedHeap.hpp"
#include "classfile/classLoaderDataGraph.inline.hpp"
#include "code/codeCache.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/universe.hpp"
#include "nmt/mallocTracker.hpp"
#include "nmt/memBaseline.hpp"
#include "nmt/memTracker.hpp"
#include "runtime/os.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/nonJavaThread.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"
#include "vitals/vitals.hpp"
#include "vitals/vitals_internals.hpp"
#include "vitals/vitalsLocker.hpp"

#include <locale.h>
#include <time.h>

// Newer JDKS: NMT is always on and this macro does not exist
// Older JDKs: NMT can be off at compile time; in that case INCLUDE_NMT
//  will be defined=0 via CFLAGS; or on, in that case it will be defined=1 in macros.hpp.
#ifndef INCLUDE_NMT
#define INCLUDE_NMT 1
#endif

namespace sapmachine_vitals {

static Lock g_vitals_lock("VitalsLock");

namespace counters {

static volatile size_t g_number_of_clds = 0;
static volatile size_t g_number_of_anon_clds = 0;
static volatile size_t g_classes_loaded = 0;
static volatile size_t g_classes_unloaded = 0;
static volatile size_t g_threads_created = 0;

void inc_cld_count(bool is_anon_cld) {
  Atomic::inc(&g_number_of_clds);
  if (is_anon_cld) {
    Atomic::inc(&g_number_of_anon_clds);
  }
}

void dec_cld_count(bool is_anon_cld) {
  Atomic::dec(&g_number_of_clds);
  if (is_anon_cld) {
    Atomic::dec(&g_number_of_anon_clds);
  }
}

void inc_classes_loaded(size_t count) {
  Atomic::add(&g_classes_loaded, count);
}

void inc_classes_unloaded(size_t count) {
  Atomic::add(&g_classes_unloaded, count);
}

void inc_threads_created(size_t count) {
  Atomic::add(&g_threads_created, count);
}

} // namespace counters

// helper function for the missing outputStream::put(int c, int repeat)
static void ostream_put_n(outputStream* st, int c, int repeat) {
  for (int i = 0; i < repeat; i ++) {
    st->put(c);
  }
}

/////// class Sample /////

int Sample::num_values() { return ColumnList::the_list()->num_columns(); }

size_t Sample::size_in_bytes() {
  assert(num_values() > 0, "not yet initialized");
  return sizeof(Sample) + sizeof(value_t) * (num_values() - 1); // -1 since Sample::values is size 1 to shut up compilers about zero length arrays
}

// Note: this is not to be used for regular samples, which live in a preallocated table.
Sample* Sample::allocate() {
  Sample* s = (Sample*) NEW_C_HEAP_ARRAY(char, size_in_bytes(), mtInternal);
  s->reset();
  return s;
}

void Sample::reset() {
  for (int i = 0; i < num_values(); i ++) {
    set_value(i, INVALID_VALUE);
  }
  DEBUG_ONLY(_num = -1;)
  _timestamp = 0;
}

void Sample::set_value(int index, value_t v) {
  assert(index >= 0 && index < num_values(), "invalid index");
  _values[index] = v;
}

void Sample::set_timestamp(time_t t) {
  _timestamp = t;
}

#ifdef ASSERT
void Sample::set_num(int n) {
  _num = n;
}
#endif

value_t Sample::value(int index) const {
  assert(index >= 0 && index < num_values(), "invalid index");
  return _values[index];
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
// Print to ostream, but only if ostream is given. In any case return number of
// characters printed (or which would have been printed).
static
ATTRIBUTE_PRINTF(2, 3)
int printf_helper(outputStream* st, const char *fmt, ...) {
  // We only print numbers, so a small buffer is fine.
  char buf[128];
  va_list args;
  int len = 0;
  va_start(args, fmt);
  len = jio_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  // jio_vsnprintf guarantees -1 on truncation, and always zero termination if buffersize > 0.
  assert(len >= 0, "Error, possible truncation. Increase bufsize?");
  if (len < 0) { // Handle in release too: just print a clear marker
    jio_snprintf(buf, sizeof(buf), "!ERR!");
    len = (int)::strlen(buf);
  }
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


////// ColumnWidths : a helper class for pre-calculating column widths to make a table align nicely.
// Keeps an array of ints, dynamically sized (since each platform has a different number of columns),
// and offers methods of auto-sizeing them to fit given samples (via dry-printing).
class ColumnWidths {
  int _widths[64]; // Don't allocate dynamically, since we might not have enough memory when we use it.

public:

  ColumnWidths() {
    // Assert including the non-active columns, so we spot possible problems earlier.
    assert(sizeof(_widths) / sizeof(_widths[0]) >= (size_t) Legend::the_legend()->nr_of_columns(), "array too small");

    // Allocate array; initialize with the minimum required column widths (which is the
    // size required to print the column header fully)
    const Column* c = ColumnList::the_list()->first();
    while (c != NULL) {
      _widths[c->index()] = (int)::strlen(c->name());
      c = c->next();
    }
  }

  // given a sample (and an optional preceding sample for delta values),
  //   update widths to accommodate sample values (uses dry-printing)
  void update_from_sample(const Sample* sample, const Sample* last_sample, const print_info_t* pi, int add_width = 0) {
    const Column* c = ColumnList::the_list()->first();
    while (c != NULL) {
      const int idx = c->index();
      const value_t v = sample->value(idx);
      value_t v2 = INVALID_VALUE;
      int age = -1;
      if (last_sample != NULL) {
        v2 = last_sample->value(idx);
        age = sample->timestamp() - last_sample->timestamp();
      }
      int needed = c->calc_print_size(v, v2, age, pi) + add_width;
      if (_widths[idx] < needed) {
        _widths[idx] = needed;
      }
      c = c->next();
    }
  }

  int at(int index) const {
    return _widths[index];
  }
};


////// Legend ///////////////////////////////////////////////

Legend* Legend::_the_legend = NULL;

bool Legend::initialize() {
  _the_legend = new Legend();
  return _the_legend != NULL;
}

stringStream _legend;
stringStream _footnote;
static Legend* _the_legend;

Legend::Legend() : _last_added_cat(NULL), _nr_of_columns(0) {}

void Legend::add_column_info(const char* const category, const char* const header,
                       const char* const name, const char* const description) {
  // Print category label if this column opens a new category
  if ((_last_added_cat == NULL) || (::strcmp(_last_added_cat, category) != 0)) {
    print_text_with_dashes(&_legend, category, 30);
    _legend.cr();
  }
  _last_added_cat = category;
  _nr_of_columns++;
  // print column name and description
  const int min_width_column_label = 16;
  char buf[32];
  if (header != NULL) {
    jio_snprintf(buf, sizeof(buf), "%s-%s", header, name);
  } else {
    jio_snprintf(buf, sizeof(buf), "%s", name);
  }
  _legend.print_cr("%*s: %s", min_width_column_label, buf, description);
}

void Legend::add_footnote(const char* text) {
  _footnote.print_cr("%s", text);
}

void Legend::print_on(outputStream* st) const {
  st->print_raw(_legend.base());
  st->cr();
  st->print_raw(_footnote.base());
  st->print_cr("(Vitals version %X, pid: %d)", vitals_version, os::current_process_id());
}

////// ColumnList: a singleton class holding all information about all columns

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

static void print_category_line(outputStream* st, const ColumnWidths* widths, const print_info_t* pi) {

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
    width += widths->at(c->index());
    width += 1; // divider between columns
    last_category_text = c->category();
    c = c->next();
  }
  print_text_with_dashes(st, last_category_text, width - 1);
  st->cr();
}

static void print_header_line(outputStream* st, const ColumnWidths* widths, const print_info_t* pi) {

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
    width += widths->at(c->index());
    width += 1; // divider between columns
    last_header_text = c->header();
    c = c->next();
  }
  if (width > 0 && last_header_text != NULL) {
    print_text_with_dashes(st, last_header_text, width - 1);
  }
  st->cr();
}

static void print_column_names(outputStream* st, const ColumnWidths* widths, const print_info_t* pi) {

  // Leave space for timestamp column
  if (pi->csv == false) {
    ostream_put_n(st, ' ', TIMESTAMP_LEN + TIMESTAMP_DIVIDER_LEN);
  } else {
    st->print_raw("time,");
  }

  const Column* c = ColumnList::the_list()->first();
  const Column* previous = NULL;
  while (c != NULL) {
    if (pi->csv == false) {
      st->print("%-*s ", widths->at(c->index()), c->name());
    } else { // csv mode
      // csv: use comma as delimiter, don't pad, and precede name with category/header
      //  (limited to 4 chars).
      if (c->category() != NULL) {
        st->print("%.4s-", c->category());
      }
      if (c->header() != NULL) {
        st->print("%.4s-", c->header());
      }
      st->print("%s,", c->name());
    }
    previous = c;
    c = c->next();
  }
  st->cr();
}

// Print a human readable size.
// byte_size: size, in bytes, to be printed.
// scale: K,M,G or 0 (dynamic)
// width: printing width.
static int print_memory_size(outputStream* st, size_t byte_size, size_t scale)  {

  // If we forced a unit via scale=.. argument, we suppress display of the unit
  // since we already know which unit is used. That saves horizontal space and
  // makes automatic processing of the data easier.
  bool dynamic_mode = false;

  if (scale == 0) {
    dynamic_mode = true;
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
  if (dynamic_mode) {
    switch(scale) {
      case K: display_unit = "k"; break;
      case M: display_unit = "m"; break;
      case G: display_unit = "g"; break;
      default:
        ShouldNotReachHere();
    }
  }

  // How we display stuff:
  // scale=1 (manually set)          - print exact byte values without unit
  // scale=0 (default, dynamic mode) - print values < 1024KB as "..k", <1024MB as "..m", "..g" above that
  //                                 - to distinguish between 0 and "almost 0" print very small values as "<1K"
  //                                 - print "k", "m" values with precision 0, "g" values with precision 1.
  // scale=k,m or g (manually set)   - print value divided by scale and without unit. No smart printing.
  //                                   Used mostly for automated processing, lets keep parsing simple.

  int l = 0;
  if (scale == 1) {
    // scale = 1 - print exact bytes
    l = printf_helper(st, SIZE_FORMAT, byte_size);

  } else {
    const float display_value = (float) byte_size / scale;
    if (dynamic_mode) {
      // dynamic scale
      const int precision = scale >= G ? 1 : 0;
      if (byte_size > 0 && byte_size < 1 * K) {
        // very small but not zero.
        assert(scale == K, "Sanity");
        l = printf_helper(st, "<1%s", display_unit);
      } else {
        l = printf_helper(st, "%.*f%s", precision, display_value, display_unit);
      }
    } else {
      // fixed scale K, M or G
      const int precision = 0;
      l = printf_helper(st, "%.*f%s", precision, display_value, display_unit);
    }
  }

  return l;

}

///////// class Column and childs ///////////

Column::Column(const char* category, const char* header, const char* name, const char* description, Extremum extremum)
  : _category(category),
    _header(header), // may be NULL
    _name(name),
    _description(description),
    _extremum(extremum),
    _next(NULL), _idx(-1),
    _idx_cat(-1), _idx_hdr(-1)
{}

void Column::print_value(outputStream* st, value_t value, value_t last_value,
    int last_value_age, int min_width, const print_info_t* pi, char const* marker) const {

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
  st->print_raw(marker);
  if (pi->csv) {
    st->put('"');
  }
}

// Returns the number of characters this value needs to be printed.
int Column::calc_print_size(value_t value, value_t last_value,
    int last_value_age, const print_info_t* pi) const {
  return do_print(NULL, value, last_value, last_value_age, pi);
}

int Column::do_print(outputStream* st, value_t value, value_t last_value,
                     int last_value_age, const print_info_t* pi) const {
  if (value == INVALID_VALUE) {
    if (pi->raw) {
      if (st != NULL) {
        return printf_helper(st, "%s", "?");
      }
      return 1;
    } else {
      return 0;
    }
  } else {
    if (pi->raw) {
      return printf_helper(st, UINT64_FORMAT, value);
    } else {
      return do_print0(st, value, last_value, last_value_age, pi);
    }
  }
}

int PlainValueColumn::do_print0(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const
{
  return printf_helper(st, UINT64_FORMAT, value);
}

int DeltaValueColumn::do_print0(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const {
  if (last_value > value) {
    // we assume the underlying value to be monotonically raising, and that
    // any negative delta would be just a fluke (e.g. counter overflows)
    // we do not want to show
    return 0;
  }
  if (last_value != INVALID_VALUE) {
    return printf_helper(st, INT64_FORMAT, (int64_t)(value - last_value));
  }
  return 0;
}

int MemorySizeColumn::do_print0(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const {
  return print_memory_size(st, value, pi->scale);
}

int DeltaMemorySizeColumn::do_print0(outputStream* st, value_t value,
    value_t last_value, int last_value_age, const print_info_t* pi) const {
  if (last_value != INVALID_VALUE) {
    return print_memory_size(st, value - last_value, pi->scale);
  }
  return 0;
}

////////////// sample printing ///////////////////////////

// Print one sample.
static void print_one_sample(outputStream* st, const Sample* sample,
    const Sample* last_sample, const ColumnWidths* widths, const print_info_t* pi, int marked_index = -1, char const* mark = NULL) {

  // Print timestamp and divider
  if (pi->csv) {
    st->print("\"");
  }
  print_timestamp(st, sample->timestamp());
  if (pi->csv) {
    st->print("\"");
  }

  if (pi->csv == false) {
    ostream_put_n(st, ' ', TIMESTAMP_DIVIDER_LEN);
  } else {
    st->put(',');
  }

  const Column* c = ColumnList::the_list()->first();
  while (c != NULL) {
    const int idx = c->index();
    const value_t v = sample->value(idx);
    value_t v2 = INVALID_VALUE;
    int age = -1;
    if (last_sample != NULL) {
      v2 = last_sample->value(idx);
      age = sample->timestamp() - last_sample->timestamp();
    }
    const int min_width = widths->at(idx) - (marked_index >= 0 ? 1 : 0);
    c->print_value(st, v, v2, age, min_width, pi,
                  marked_index == idx ? mark : (marked_index >= 0 && !pi->csv ? " " : ""));
    st->put(pi->csv ? ',' : ' ');
    c = c->next();
  }
  st->cr();
}

////////////// Class SampleTable /////////////////////////

// A fixed sized fifo buffer of n samples
class SampleTable : public CHeapObj<mtInternal> {

  const int _num_entries;
  int _head;      // Index of the last sample written; -1 if none have been written yet
  bool _did_wrap;
  Sample* _samples;

#ifdef ASSERT
  void verify() const {
    assert(_samples != NULL, "sanity");
    assert(_head >= 0 && _head < _num_entries, "sanity");
  }
#endif

  size_t sample_offset_in_bytes(int idx) const {
    assert(idx >= 0 && idx <= _num_entries, "invalid index: %d", idx);
    return Sample::size_in_bytes() * idx;
  }

public:

  SampleTable(int num_entries)
    : _num_entries(num_entries),
      _head(-1),
      _did_wrap(false),
      _samples(NULL)
  {
    _samples = (Sample*) NEW_C_HEAP_ARRAY(char, Sample::size_in_bytes() * _num_entries, mtInternal);
#ifdef ASSERT
    for (int i = 0; i < _num_entries; i ++) {
      sample_at(i)->reset();
    }
#endif
  }

  bool is_empty() const { return _head == -1; }

  const Sample* sample_at(int index) const { return (Sample*)((uint8_t*)_samples + sample_offset_in_bytes(index)); }
  Sample* sample_at(int index) { return (Sample*)((uint8_t*)_samples + sample_offset_in_bytes(index)); }

  void add_sample(const Sample* sample) {
    // Advance head
    _head ++;
    if (_head == _num_entries) {
      _did_wrap = true;
      _head = 0;
    }
    // Copy sample
    ::memcpy(sample_at(_head), sample, Sample::size_in_bytes());
    DEBUG_ONLY(verify());
  }

  // Given a valid sample index, return the previous index or -1 if this is the oldest sample.
  int get_previous_index(int idx) const {
    assert(idx >= 0 && idx <= _num_entries, "index oob: %d", idx);
    assert(_did_wrap == true || idx <= _head, "index invalid: %d", idx);
    int prev = idx - 1;
    if (prev == -1 && _did_wrap) {
      prev = _num_entries - 1;
    }
    if (prev == _head) {
      prev = -1;
    }
    return prev;
  }

  class Closure {
   public:
    virtual void do_sample(const Sample* sample, const Sample* previous_sample) = 0;
  };

  void call_closure_for_sample_at(Closure* closure, int idx) const {
    const Sample* sample = sample_at(idx);
    int idx2 = get_previous_index(idx);
    const Sample* previous_sample = idx2 == -1 ? NULL : sample_at(idx2);
    closure->do_sample(sample, previous_sample);
  }

  void walk_table_locked(Closure* closure, bool youngest_to_oldest = true) const {

    if (_head == -1) {
      return;
    }

    DEBUG_ONLY(verify();)

    if (youngest_to_oldest) { // youngest to oldest
      for (int pos = _head; pos >= 0; pos--) {
        call_closure_for_sample_at(closure, pos);
      }
      if (_did_wrap) {
        for (int pos = _num_entries - 1; pos > _head; pos--) {
          call_closure_for_sample_at(closure, pos);
        }
      }
    } else { // oldest to youngest
      if (_did_wrap) {
        for (int pos = _head + 1; pos < _num_entries; pos ++) {
          call_closure_for_sample_at(closure, pos);
        }
      }
      for (int pos = 0; pos <= _head; pos ++) {
        call_closure_for_sample_at(closure, pos);
      }
    }
  }

};

class MeasureColumnWidthsClosure : public SampleTable::Closure {
  const print_info_t* const _pi;
  ColumnWidths* const _widths;

public:
  MeasureColumnWidthsClosure(const print_info_t* pi, ColumnWidths* widths) :
    _pi(pi), _widths(widths) {}

  void do_sample(const Sample* sample, const Sample* previous_sample) {
    _widths->update_from_sample(sample, previous_sample, _pi);
  }
};

class PrintSamplesClosure : public SampleTable::Closure {
  outputStream* const _st;
  const print_info_t* const _pi;
  const ColumnWidths* const _widths;

public:

  PrintSamplesClosure(outputStream* st, const print_info_t* pi, const ColumnWidths* widths) :
    _st(st), _pi(pi), _widths(widths) {}

  void do_sample(const Sample* sample, const Sample* previous_sample) {
    print_one_sample(_st, sample, previous_sample, _widths, _pi);
  }
};

// sampleTables is a combination of two tables: a short term table and a long term table.
// It takes care to feed new samples into these tables at the appropriate intervals.
class SampleTables: public CHeapObj<mtInternal> {

  static int short_term_tablesize() { return (VitalsShortTermTableHours * 3600 / VitalsSampleInterval) + 1; }
  static int long_term_tablesize()  { return (VitalsLongTermTableDays * 24 * 60 / VitalsLongTermSampleIntervalMinutes) + 1; }

  SampleTable _short_term_table;
  SampleTable _long_term_table;
  SampleTable _extremum_samples;
  SampleTable _last_extremum_samples;

  int _count;
  int _large_table_count;

  static void print_table(const SampleTable* table, outputStream* st,
                          const ColumnWidths* widths, const print_info_t* pi) {
    if (table->is_empty()) {
      st->print_cr("(no samples)");
      return;
    }
    PrintSamplesClosure prclos(st, pi, widths);
    table->walk_table_locked(&prclos, !pi->reverse_ordering);
  }

  static void print_headers(outputStream* st, const ColumnWidths* widths, const print_info_t* pi) {
    if (pi->csv == false) {
      print_category_line(st, widths, pi);
      print_header_line(st, widths, pi);
    }
    print_column_names(st, widths, pi);
  }

  // Helper, print a time span given in seconds-
  static void print_time_span(outputStream* st, int secs) {
    const int mins = secs / 60;
    const int hrs = secs / (60 * 60);
    const int days = secs / (60 * 60 * 24);
    if (days > 1) {
      st->print_cr("Last %d days:", days);
    } else if (hrs > 1) {
      st->print_cr("Last %d hours:", hrs);
    } else if (mins > 1) {
      st->print_cr("Last %d minutes:", mins);
    } else {
      st->print_cr("Last %d seconds:", secs);
    }
  }

public:

  SampleTables()
    : _short_term_table(short_term_tablesize()),
      _long_term_table(long_term_tablesize()),
      _extremum_samples(Sample::num_values()),
      _last_extremum_samples(Sample::num_values()),
      _count(0),
      _large_table_count(MAX2(1, (int) (VitalsLongTermSampleIntervalMinutes * 60 / VitalsSampleInterval)))
  {}

  void add_sample(const Sample* sample) {
    AutoLock autolock(&g_vitals_lock);
    // Nothing we do in here blocks: the sample values are already taken,
    // we only modify existing data structures (no memory is allocated either).
    _short_term_table.add_sample(sample);
    // Increment before feeding longterm table, in order for it to show up in reports
    // only after an initial long term table interval has passed
    _count++;
    // Feed long term table
    if ((_count % _large_table_count) == 0) {
      _long_term_table.add_sample(sample);
    }

    // Update exetremum samples if needed.
    if (StoreVitalsExtremas) {
      static Sample* last_sample = NULL;

      if (last_sample == NULL) {
        // Nothing to do yet. We need at least two samples, since some types need the
        // previous sample to print. Just allocate the space for the last sample as a marker
        // for seeing the first sample.
        last_sample = (Sample*) NEW_C_HEAP_ARRAY(char, Sample::size_in_bytes(), mtInternal);
      } else if (_extremum_samples.is_empty()) {
        // We already have a last sample and this is the second sample we see.
        // We can initialize the two tables now to store the last sample and this sample
        // for all extremas.
        for (int i = 0; i < Sample::num_values(); ++i) {
          _last_extremum_samples.add_sample(last_sample);
          _extremum_samples.add_sample(sample);
        }
      } else {
        // Iterate columns and update if needed.
        for (Column const* column = ColumnList::the_list()->first(); column != NULL; column = column->next()) {
          if (column->extremum() != NONE) {
            int idx = column->index();
            Sample* extremum_sample = _extremum_samples.sample_at(idx);

            bool should_log = (column->extremum() == MAX) && (sample->value(idx) > extremum_sample->value(idx));
            should_log |= (column->extremum() == MIN) && (sample->value(idx) < extremum_sample->value(idx));

            if (should_log) {
              Sample* last_extremum_sample = _last_extremum_samples.sample_at(idx);
              ::memcpy(last_extremum_sample, last_sample, Sample::size_in_bytes());
              ::memcpy(extremum_sample, sample, Sample::size_in_bytes());
            }
          }
        }
      }

      // Remember the last sample.
      ::memcpy(last_sample, sample, Sample::size_in_bytes());
    }
  }

  void print_all(outputStream* st, const print_info_t* pi, const Sample* sample_now) {

    { // lock start
      AutoLock autolock(&g_vitals_lock);

      if (sample_now != NULL) {
        ColumnWidths widths;
        MeasureColumnWidthsClosure mcwclos(pi, &widths);
        widths.update_from_sample(sample_now, NULL, pi);
        st->print_cr("Now:");
        print_headers(st, &widths, pi);
        print_one_sample(st, sample_now, NULL, &widths, pi);
        st->cr();
      }

      if (!_short_term_table.is_empty()) {
        ColumnWidths widths;
        MeasureColumnWidthsClosure mcwclos(pi, &widths);
        _short_term_table.walk_table_locked(&mcwclos);

        if (pi->csv == false) {
          print_time_span(st, VitalsShortTermTableHours * 3600);
        }
        print_headers(st, &widths, pi);
        print_table(&_short_term_table, st, &widths, pi);
        st->cr();
      }

      if (!_long_term_table.is_empty()) {
        ColumnWidths widths;
        MeasureColumnWidthsClosure mcwclos(pi, &widths);
        _long_term_table.walk_table_locked(&mcwclos);
        print_time_span(st, VitalsLongTermTableDays * 24 * 3600);
        print_headers(st, &widths, pi);
        print_table(&_long_term_table, st, &widths, pi);
        st->cr();
      }

      if (StoreVitalsExtremas && !_extremum_samples.is_empty() && !_last_extremum_samples.is_empty()) {
        st->print_cr("Samples at extremes (+ marks a maximum, - marks a minimum)");

        ColumnWidths widths;
        MeasureColumnWidthsClosure mcwclos(pi, &widths);

        for (Column const* column = ColumnList::the_list()->first(); column != NULL; column = column->next()) {
          if (column->extremum() != NONE) {
            Sample* extremum_sample = _extremum_samples.sample_at(column->index());
            Sample* last_extremum_sample = _last_extremum_samples.sample_at(column->index());
            widths.update_from_sample(extremum_sample, last_extremum_sample, pi, 1);
          }
        }

        print_headers(st, &widths, pi); // Need more space for the mark to display.

        for (Column const* column = ColumnList::the_list()->first(); column != NULL; column = column->next()) {
          if (column->extremum() != NONE) {
            Sample* extremum_sample = _extremum_samples.sample_at(column->index());
            Sample* last_extremum_sample = _last_extremum_samples.sample_at(column->index());
            print_one_sample(st, extremum_sample, last_extremum_sample, &widths, pi, column->index(),
                             column->extremum() == MIN ? "-" : "+");
          }
        }
      }

      st->cr();

    } // lock end
  }
};

static SampleTables* g_all_tables = NULL;

/////////////// SAMPLING //////////////////////

// Samples all values, but leaves timestamp unchanged
static void sample_values(Sample* sample, bool avoid_locking) {
  time_t t;
  ::time(&t);
  sample->set_timestamp(t);
  DEBUG_ONLY(sample->set_num(-1);)
  sample_jvm_values(sample, avoid_locking);
  sample_platform_values(sample);
}

class SamplerThread: public NamedThread {

  Sample* _sample;
  bool _stop;
  int _samples_taken;
  int _jump_cooldown;

  static int get_sample_interval_ms() {
    return (int)VitalsSampleInterval * 1000;
  }

  void take_sample() {
    _sample->reset();
    DEBUG_ONLY(_sample->set_num(_samples_taken);)
    _samples_taken ++;
    sample_values(_sample, VitalsLockFreeSampling);
    g_all_tables->add_sample(_sample);
  }

public:

  SamplerThread()
    : NamedThread(),
      _sample(NULL),
      _stop(false),
      _samples_taken(0),
      _jump_cooldown(0)
  {
    _sample = Sample::allocate();
    this->set_name("vitals sampler thread");
  }

  virtual void run() {
    record_stack_base_and_size();
    for (;;) {
      take_sample();
      os::naked_sleep(get_sample_interval_ms());
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


///////////////////////////////////////
/////// JVM-specific columns //////////

static Column* g_col_heap_committed = NULL;
static Column* g_col_heap_used = NULL;

static Column* g_col_metaspace_committed = NULL;
static Column* g_col_metaspace_used = NULL;

static bool g_show_classspace_columns = false;
static Column* g_col_classspace_committed = NULL;
static Column* g_col_classspace_used = NULL;

static Column* g_col_metaspace_cap_until_gc = NULL;

static Column* g_col_codecache_committed = NULL;

static bool g_show_nmt_columns = false;
static Column* g_col_nmt_malloc = NULL;
static Column* g_col_nmt_mmap = NULL;
static Column* g_col_nmt_gc_overhead = NULL;
static Column* g_col_nmt_other = NULL;
static Column* g_col_nmt_overhead = NULL;

static Column* g_col_number_of_java_threads = NULL;
static Column* g_col_number_of_java_threads_non_demon = NULL;

static bool g_show_size_thread_stacks_col = false;
static Column* g_col_size_thread_stacks = NULL;

static Column* g_col_number_of_java_threads_created = NULL;

static Column* g_col_number_of_clds = NULL;
static Column* g_col_number_of_anon_clds = NULL;

static Column* g_col_number_of_classes = NULL;
static Column* g_col_number_of_class_loads = NULL;
static Column* g_col_number_of_class_unloads = NULL;

static bool is_nmt_enabled() {
#if INCLUDE_NMT
  // Note: JDK version dependency: Before JDK18, NMT had the ability to shut down operations
  // at any point in time, and therefore we also had NMT_minimal tracking level. Therefore,
  // to stay version independent, we never must compare against NMT_off or NMT_minimal directly,
  // only always against NMT_summary/detail. And to be very safe, don't assume a numerical
  // value either, since older JDKs had NMT_unknown as a very high numerical value.
  const NMT_TrackingLevel lvl = MemTracker::tracking_level();
  return (lvl == NMT_summary || lvl == NMT_detail);
#else
  return false;
#endif
}

static bool add_jvm_columns() {
  // Order matters!
  const char* const jvm_cat = "jvm";

  Legend::the_legend()->add_footnote("  [delta]: values refer to the previous measurement.");
  Legend::the_legend()->add_footnote("    [nmt]: only shown if NMT is available and activated");
  Legend::the_legend()->add_footnote("     [cs]: only shown on 64-bit if class space is active");
  Legend::the_legend()->add_footnote("  [linux]: only on Linux");

  g_col_heap_committed =
      define_column<MemorySizeColumn>(jvm_cat, "heap", "comm", "Java Heap Size, committed", true);
  g_col_heap_used =
      define_column<MemorySizeColumn>(jvm_cat, "heap", "used", "Java Heap Size, used", true);

  g_col_metaspace_committed =
      define_column<MemorySizeColumn>(jvm_cat, "meta", "comm", "Meta Space Size (class+nonclass), committed", true);
  g_col_metaspace_used =
      define_column<MemorySizeColumn>(jvm_cat, "meta", "used", "Meta Space Size (class+nonclass), used", true);

  // Class space columns only shown if class space is active
  g_show_classspace_columns = Metaspace::using_class_space();
  g_col_classspace_committed =
      define_column<MemorySizeColumn>(jvm_cat, "meta", "csc", "Class Space Size, committed [cs]", g_show_classspace_columns);
  g_col_classspace_used =
      define_column<MemorySizeColumn>(jvm_cat, "meta", "csu", "Class Space Size, used [cs]",  g_show_classspace_columns);

  g_col_metaspace_cap_until_gc =
      define_column<MemorySizeColumn>(jvm_cat, "meta", "gctr", "GC threshold", true);

  g_col_codecache_committed =
      define_column<MemorySizeColumn>(jvm_cat, NULL, "code", "Code cache, committed", true);

  // NMT columns only shown if NMT is at least at summary level
  g_show_nmt_columns = is_nmt_enabled();
  g_col_nmt_malloc =
     define_column<MemorySizeColumn>(jvm_cat, "nmt", "mlc", "Memory malloced by hotspot [nmt]", g_show_nmt_columns);
  g_col_nmt_mmap =
      define_column<MemorySizeColumn>(jvm_cat, "nmt", "map", "Memory mapped by hotspot [nmt]", g_show_nmt_columns);
  g_col_nmt_gc_overhead =
      define_column<MemorySizeColumn>(jvm_cat, "nmt", "gc", "NMT \"gc\" (GC-overhead, malloc and mmap) [nmt]", g_show_nmt_columns);
  g_col_nmt_other =
      define_column<MemorySizeColumn>(jvm_cat, "nmt", "oth", "NMT \"other\" (typically DBB or Unsafe.allocateMemory) [nmt]", g_show_nmt_columns);
  g_col_nmt_overhead =
      define_column<MemorySizeColumn>(jvm_cat, "nmt", "ovh", "NMT overhead [nmt]", g_show_nmt_columns);

  g_col_number_of_java_threads =
      define_column<PlainValueColumn>(jvm_cat, "jthr", "num", "Number of java threads", true);
  g_col_number_of_java_threads_non_demon =
      define_column<PlainValueColumn>(jvm_cat, "jthr", "nd", "Number of non-demon java threads", true);
  g_col_number_of_java_threads_created =
      define_column<DeltaValueColumn>(jvm_cat, "jthr", "cr", "Threads created [delta]", true);

  // Displaying thread stack size for now only implemented on Linux, and requires NMT
  g_show_size_thread_stacks_col = LINUX_ONLY(g_show_nmt_columns) NOT_LINUX(false);
  g_col_size_thread_stacks =
      define_column<MemorySizeColumn>(jvm_cat, "jthr", "st", "Total reserved size of java thread stacks [nmt] [linux]",
          g_show_size_thread_stacks_col);

  g_col_number_of_clds =
      define_column<PlainValueColumn>(jvm_cat,  "cldg", "num", "Classloader Data", true);
  g_col_number_of_anon_clds =
      define_column<PlainValueColumn>(jvm_cat,  "cldg", "anon", "Anonymous CLD", true);

  g_col_number_of_classes =
      define_column<PlainValueColumn>(jvm_cat, "cls", "num", "Classes (instance + array)", true);

  g_col_number_of_class_loads =
        define_column<DeltaValueColumn>(jvm_cat, "cls", "ld", "Class loaded [delta]", true);

  g_col_number_of_class_unloads =
        define_column<DeltaValueColumn>(jvm_cat, "cls", "uld", "Classes unloaded [delta]", true);

  return true;
}


////////// class ValueSampler and childs /////////////////

template <typename T>
static void set_value_in_sample(const Column* col, Sample* sample, T t) {
  if (col != NULL) {
    int idx = col->index();
    assert(ColumnList::the_list()->is_valid_column_index(idx), "Invalid column index");
    sample->set_value(idx, (value_t)t);
  }
}

struct nmt_values_t {
  // How much memory, in total, was committed via mmap
  value_t mapped_total;
  // How much memory, in total, was malloced
  value_t malloced_total;
  // How many allocations from malloc, in total
  value_t malloced_num;
  // thread stack size; depending on NMT version this would
  // be reserved (I believe up to and including jdk 8) or committed (9+)
  value_t thread_stacks_committed;
  // NMT "GC" category (both malloced and mapped)
  value_t gc_overhead;
  // NMT "other" category (both malloced and mapped). Usually dominated by DBB allocated with allocateDirect(),
  // and Unsafe.allocateMemory.
  value_t other_memory;
  // NMT overhead  (both malloced and mapped)
  value_t overhead;
};

static bool get_nmt_values(nmt_values_t* out) {
#if INCLUDE_NMT
  if (is_nmt_enabled()) {
    MutexLocker locker(MemTracker::query_lock());
    MemBaseline baseline;
    baseline.baseline(true);
    MallocMemorySnapshot* mlc_snapshot = baseline.malloc_memory_snapshot();
    VirtualMemorySnapshot vm_snapshot;
    VirtualMemorySummary::snapshot(&vm_snapshot);
    out->malloced_total = mlc_snapshot->total();
    out->mapped_total = vm_snapshot.total_committed();
    out->thread_stacks_committed =
        vm_snapshot.by_type(mtThreadStack)->committed();
    out->thread_stacks_committed =
        vm_snapshot.by_type(MEMFLAGS::mtThreadStack)->committed() +
        mlc_snapshot->by_type(MEMFLAGS::mtThreadStack)->malloc_size();
    out->gc_overhead =
        vm_snapshot.by_type(MEMFLAGS::mtGC)->committed() +
        mlc_snapshot->by_type(MEMFLAGS::mtGC)->malloc_size();
    out->other_memory =
        vm_snapshot.by_type(MEMFLAGS::mtOther)->committed() +
        mlc_snapshot->by_type(MEMFLAGS::mtOther)->malloc_size();
    out->overhead =
        vm_snapshot.by_type(MEMFLAGS::mtNMT)->committed() +
        mlc_snapshot->by_type(MEMFLAGS::mtNMT)->malloc_size() +
        mlc_snapshot->malloc_overhead();
    out->malloced_num =
        mlc_snapshot->total_count();
    return true;
  }
#endif // INCLUDE_NMT
  return false;
}

void sample_jvm_values(Sample* sample, bool avoid_locking) {

  // Note: if avoid_locking=true, skip values which need JVM-side locking.

  nmt_values_t nmt_vals;
  bool have_nmt_values = false;
  if (!avoid_locking) {
    have_nmt_values = get_nmt_values(&nmt_vals);
  }

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
    set_value_in_sample(g_col_heap_committed, sample, heap_cap);
    set_value_in_sample(g_col_heap_used, sample, heap_used);
  }

  // Metaspace
  set_value_in_sample(g_col_metaspace_committed, sample, MetaspaceUtils::committed_bytes());
  set_value_in_sample(g_col_metaspace_used, sample, MetaspaceUtils::used_bytes());

  if (Metaspace::using_class_space()) {
    set_value_in_sample(g_col_classspace_committed, sample, MetaspaceUtils::committed_bytes(Metaspace::ClassType));
    set_value_in_sample(g_col_classspace_used, sample, MetaspaceUtils::used_bytes(Metaspace::ClassType));
  }

  set_value_in_sample(g_col_metaspace_cap_until_gc, sample, MetaspaceGC::capacity_until_GC());

  // Code cache
  value_t codecache_committed = INVALID_VALUE;
  if (!avoid_locking) {
    MutexLocker lck(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    codecache_committed = CodeCache::capacity();
  }
  set_value_in_sample(g_col_codecache_committed, sample, codecache_committed);

  // NMT integration
  if (have_nmt_values) {
    set_value_in_sample(g_col_nmt_malloc, sample, nmt_vals.malloced_total);
    set_value_in_sample(g_col_nmt_mmap, sample, nmt_vals.mapped_total);
    set_value_in_sample(g_col_nmt_gc_overhead, sample, nmt_vals.gc_overhead);
    set_value_in_sample(g_col_nmt_other, sample, nmt_vals.other_memory);
    set_value_in_sample(g_col_nmt_overhead, sample, nmt_vals.overhead);
  }

  // Java threads
  set_value_in_sample(g_col_number_of_java_threads, sample, Threads::number_of_threads());
  set_value_in_sample(g_col_number_of_java_threads_non_demon, sample, Threads::number_of_non_daemon_threads());
  set_value_in_sample(g_col_number_of_java_threads_created, sample, counters::g_threads_created);

  // Java thread stack size
  if (have_nmt_values) {
    set_value_in_sample(g_col_size_thread_stacks, sample, nmt_vals.thread_stacks_committed);
  }

  // CLDG
  set_value_in_sample(g_col_number_of_clds, sample, counters::g_number_of_clds);
  set_value_in_sample(g_col_number_of_anon_clds, sample, counters::g_number_of_anon_clds);

  // Classes
  set_value_in_sample(g_col_number_of_classes, sample,
      ClassLoaderDataGraph::num_instance_classes() + ClassLoaderDataGraph::num_array_classes());
  set_value_in_sample(g_col_number_of_class_loads, sample, counters::g_classes_loaded);
  set_value_in_sample(g_col_number_of_class_unloads, sample, counters::g_classes_unloaded);
}

bool initialize() {

  static bool initialized = false;
  assert(initialized == false, "Vitals already initialized");
  initialized = true;

  log_info(vitals)("Vitals v%x", vitals_version);

  bool success = ColumnList::initialize();
  success = success && Legend::initialize();

  // Order matters. First platform columns, then jvm columns.
  success = success && platform_columns_initialize();
  success = success && add_jvm_columns();

  // -- Now the number of columns is known (and fixed). --

  g_all_tables = new SampleTables();
  success = success && (g_all_tables != NULL);

  success = success && initialize_sampler_thread();

  if (success) {
    log_info(vitals)("Vitals initialized.");
    log_debug(vitals)("Vitals sample interval: " UINTX_FORMAT " seconds.", VitalsSampleInterval);
  } else {
    log_warning(vitals)("Failed to initialize Vitals.");
  }

  return success;

}

void cleanup() {
  if (g_sampler_thread != NULL) {
    g_sampler_thread->stop();
  }
}

void default_settings(print_info_t* out) {
  out->raw = false;
  out->csv = false;
  out->no_legend = false;
  out->reverse_ordering = false;
  out->scale = 0;
  out->sample_now = false;
}

void print_report(outputStream* st, const print_info_t* pinfo) {

  if (ColumnList::the_list() == NULL) {
    st->print_cr(" (unavailable)");
    return;
  }

  print_info_t info;
  if (pinfo != NULL) {
    info = *pinfo;
  } else {
    default_settings(&info);
  }

  if (info.csv == false) {
    st->cr();
  }

  // Print legend at the top (omit if suppressed on command line, or in csv mode).
  if (info.no_legend == false && info.csv == false) {
    Legend::the_legend()->print_on(st);
    if (info.scale != 0) {
      const char* display_unit = NULL;
      switch (info.scale) {
        case 1: display_unit = "bytes"; break;
        case K: display_unit = "KB"; break;
        case M: display_unit = "MB"; break;
        case G: display_unit = "GB"; break;
        default: ShouldNotReachHere();
      }
      st->print_cr("[mem] values are in %s.", display_unit);
    }
    st->cr();
  }

  // If we are to sample the current values at print time, do that and print them too.
  // Note: we omit the "Now" sample for csv output.
  Sample* sample_now = NULL;
  if (info.sample_now && !info.csv) {
    sample_now = Sample::allocate();
    sample_values(sample_now, true /* never lock for now sample - be safe */ );
  }

  g_all_tables->print_all(st, &info, sample_now);

  os::free(sample_now);
}

// Dump both textual and csv style reports to two files, "sapmachine_vitals_<pid>.txt" and "sapmachine_vitals_<pid>.csv".
// If these files exist, they are overwritten.
void dump_reports() {

  static const char* file_prefix = "sapmachine_vitals_";
  char vitals_file_name[1024];

  if (VitalsFile != NULL) {
    os::snprintf(vitals_file_name, sizeof(vitals_file_name), "%s.txt", VitalsFile);
  } else {
    os::snprintf(vitals_file_name, sizeof(vitals_file_name), "%s%d.txt", file_prefix, os::current_process_id());
  }

  // Note: we print two reports, both in reverse order (oldest to youngest). One in text form, one as csv.

  ::printf("Dumping Vitals to %s\n", vitals_file_name);
  {
    fileStream fs(vitals_file_name);
    static const sapmachine_vitals::print_info_t settings = {
        false, // raw
        false, // csv
        false, // no_legend
        true,  // reverse_ordering
        0,     // scale
        true   // sample_now
    };
    print_report(&fs, &settings);
  }

  if (VitalsFile != NULL) {
    os::snprintf(vitals_file_name, sizeof(vitals_file_name), "%s.csv", VitalsFile);
  } else {
    os::snprintf(vitals_file_name, sizeof(vitals_file_name), "%s%d.csv", file_prefix, os::current_process_id());
  }
  ::printf("Dumping Vitals csv to %s\n", vitals_file_name);
  {
    fileStream fs(vitals_file_name);
    static const sapmachine_vitals::print_info_t settings = {
        false, // raw
        true,  // csv
        false, // no_legend
        true,  // reverse_ordering
        1 * K, // scale
        false  // sample_now
    };
    print_report(&fs, &settings);
  }
}

// For printing in thread lists only.
const Thread* samplerthread() { return g_sampler_thread; }

} // namespace sapmachine_vitals
