/*
 * Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/sharedPathsMiscInfo.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/arguments.hpp"
#include "utilities/ostream.hpp"

SharedPathsMiscInfo::SharedPathsMiscInfo() {
  _buf_size = INITIAL_BUF_SIZE;
  _cur_ptr = _buf_start = NEW_C_HEAP_ARRAY(char, _buf_size, mtClass);
  _allocated = true;
}

SharedPathsMiscInfo::~SharedPathsMiscInfo() {
  if (_allocated) {
    FREE_C_HEAP_ARRAY(char, _buf_start);
  }
}

void SharedPathsMiscInfo::add_path(const char* path, int type) {
  log_info(class, path)("type=%s ", type_name(type));
  ClassLoader::trace_class_path("add misc shared path ", path);
  write(path, strlen(path) + 1);
  write_jint(jint(type));
}

void SharedPathsMiscInfo::ensure_size(size_t needed_bytes) {
  assert(_allocated, "cannot modify buffer during validation.");
  int used = get_used_bytes();
  int target = used + int(needed_bytes);
  if (target > _buf_size) {
    _buf_size = _buf_size * 2 + (int)needed_bytes;
    _buf_start = REALLOC_C_HEAP_ARRAY(char, _buf_start, _buf_size, mtClass);
    _cur_ptr = _buf_start + used;
    _end_ptr = _buf_start + _buf_size;
  }
}

void SharedPathsMiscInfo::write(const void* ptr, size_t size) {
  ensure_size(size);
  memcpy(_cur_ptr, ptr, size);
  _cur_ptr += size;
}

bool SharedPathsMiscInfo::read(void* ptr, size_t size) {
  if (_cur_ptr + size <= _end_ptr) {
    memcpy(ptr, _cur_ptr, size);
    _cur_ptr += size;
    return true;
  }
  return false;
}

bool SharedPathsMiscInfo::fail(const char* msg, const char* name) {
  ClassLoader::trace_class_path(msg, name);
  MetaspaceShared::set_archive_loading_failed();
  return false;
}

void SharedPathsMiscInfo::print_path(outputStream* out, int type, const char* path) {
  switch (type) {
  case BOOT:
    out->print("Expecting BOOT path=%s", path);
    break;
  case NON_EXIST:
    out->print("Expecting that %s does not exist", path);
    break;
  default:
    ShouldNotReachHere();
  }
}

bool SharedPathsMiscInfo::check() {
  // The whole buffer must be 0 terminated so that we can use strlen and strcmp
  // without fear.
  _end_ptr -= sizeof(jint);
  if (_cur_ptr >= _end_ptr) {
    return fail("Truncated archive file header");
  }
  if (*_end_ptr != 0) {
    return fail("Corrupted archive file header");
  }

  while (_cur_ptr < _end_ptr) {
    jint type;
    const char* path = _cur_ptr;
    _cur_ptr += strlen(path) + 1;
    if (!read_jint(&type)) {
      return fail("Corrupted archive file header");
    }
    LogTarget(Info, class, path) lt;
    if (lt.is_enabled()) {
      lt.print("type=%s ", type_name(type));
      LogStream ls(lt);
      print_path(&ls, type, path);
      ls.cr();
    }
    if (!check(type, path)) {
      if (!PrintSharedArchiveAndExit) {
        return false;
      }
    } else {
      ClassLoader::trace_class_path("ok");
    }
  }

  return true;
}

bool SharedPathsMiscInfo::check(jint type, const char* path) {
  switch (type) {
  case BOOT:
    // In the future we should perform the check based on the content of the mapped archive.
    if (UseAppCDS && os::file_name_strcmp(path, Arguments::get_sysclasspath()) != 0) {
      return fail("[BOOT classpath mismatch, actual =", Arguments::get_sysclasspath());
    }
    break;
  case NON_EXIST:
    {
      struct stat st;
      if (os::stat(path, &st) == 0) {
        // The file actually exists
        // But we want it to not exist -> fail
        return fail("File must not exist");
      }
    }
    break;
  default:
    return fail("Corrupted archive file header");
  }

  return true;
}
