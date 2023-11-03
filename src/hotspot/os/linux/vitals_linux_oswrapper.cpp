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

#include "precompiled.hpp"

#include "jvm.h"
#include "osContainer_linux.hpp"
#include "vitals_linux_oswrapper.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include "vitals/vitals_internals.hpp"

#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#define LOG_HERE_F(msg, ...)  { printf("[%d] ", (int)::getpid()); ::printf(msg, __VA_ARGS__); printf("\n"); fflush(stdout); }
#define LOG_HERE(msg)         { printf("[%d] ", (int)::getpid()); ::printf("%s", msg); printf("\n"); fflush(stdout); }

extern const char* sapmachine_get_memory_controller_path();

// os::file_exists() does not exist in all JDK versions, so we avoid that
static bool os_file_exists(const char* filename) {
  struct stat statbuf;
  if (filename == NULL || strlen(filename) == 0) {
    return false;
  }
  return os::stat(filename, &statbuf) == 0;
}

namespace sapmachine_vitals {

#define DEFINE_VARIABLE(name) \
  value_t OSWrapper::_##name = INVALID_VALUE;

ALL_VALUES_DO(DEFINE_VARIABLE)

#undef DEFINE_VARIABLE

time_t OSWrapper::_last_update = 0;

static const int num_seconds_until_update = 1;

///////////// procfs stuff //////////////////////////////////////////////////

class ProcFile {
  char* _buf;

  // To keep the code simple, I just use a fixed sized buffer.
  enum { bufsize = 64*K };

public:

  ProcFile() : _buf(NULL) {
    _buf = (char*)os::malloc(bufsize, mtInternal);
  }

  ~ProcFile () {
    os::free(_buf);
  }

  bool read(const char* filename) {

    FILE* f = ::fopen(filename, "r");
    if (f == NULL) {
      log_debug(vitals)("Failed to fopen %s (%d)", filename, errno);
      return false;
    }

    size_t bytes_read = ::fread(_buf, 1, bufsize, f);
    _buf[bufsize - 1] = '\0';

    ::fclose(f);

    return bytes_read > 0 && bytes_read < bufsize;
  }

  const char* text() const { return _buf; }

  // Utility function; parse a number string as value_t
  static value_t as_value(const char* text, size_t scale = 1) {
    value_t value;
    errno = 0;
    char* endptr = NULL;
    value = (value_t)::strtoll(text, &endptr, 10);
    if (endptr == text || errno != 0) {
      value = INVALID_VALUE;
    } else {
      value *= scale;
    }
    return value;
  }

  // Return the start of the file, as number. Useful for proc files which
  // contain a single number. Returns INVALID_VALUE if value did not parse
  value_t as_value(size_t scale = 1) const {
    return as_value(_buf, scale);
  }

  const char* get_prefixed_line(const char* prefix) const {
    return ::strstr(_buf, prefix);
  }

  value_t parsed_prefixed_value(const char* prefix, size_t scale = 1) const {
    value_t value = INVALID_VALUE;
    const char* const s = get_prefixed_line(prefix);
    if (s != NULL) {
      errno = 0;
      const char* p = s + ::strlen(prefix);
      return as_value(p, scale);
    }
    return value;
  }

};

struct cpu_values_t {
  value_t user;
  value_t nice;
  value_t system;
  value_t idle;
  value_t iowait;
  value_t steal;
  value_t guest;
  value_t guest_nice;
};

static void parse_proc_stat_cpu_line(const char* line, cpu_values_t* out) {
  // Note: existence of some of these values depends on kernel version
  out->user = out->nice = out->system = out->idle = out->iowait = out->steal = out->guest = out->guest_nice =
      INVALID_VALUE;
  uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  int num = ::sscanf(line,
      "cpu "
      UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " "
      UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " ",
      &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
  if (num >= 4) {
    out->user = user;
    out->nice = nice;
    out->system = system;
    out->idle = idle;
    if (num >= 5) { // iowait (5) (since Linux 2.5.41)
      out->iowait = iowait;
      if (num >= 8) { // steal (8) (since Linux 2.6.11)
        out->steal = steal;
        if (num >= 9) { // guest (9) (since Linux 2.6.24)
          out->guest = guest;
          if (num >= 10) { // guest (9) (since Linux 2.6.33)
            out->guest_nice = guest_nice;
          }
        }
      }
    }
  }
}


#ifdef __GLIBC__
// We use either mallinfo (which may be obsolete or removed in newer glibc versions) or mallinfo2
// (which does not exist prior to glibc 2.34).

#define MALLINFO_MEMBER_DO(f) \
		f(arena) \
		f(ordblks) \
		f(smblks) \
		f(hblks) \
		f(hblkhd) \
		f(usmblks) \
		f(fsmblks) \
		f(uordblks) \
		f(fordblks) \
		f(keepcost)

struct glibc_mallinfo {
#define DEF_MALLINFO_MEMBER(f) int f;
  MALLINFO_MEMBER_DO(DEF_MALLINFO_MEMBER)
#undef DEF_MALLINFO_MEMBER
};

struct glibc_mallinfo2 {
#define DEF_MALLINFO2_MEMBER(f) size_t f;
  MALLINFO_MEMBER_DO(DEF_MALLINFO2_MEMBER)
#undef DEF_MALLINFO2_MEMBER
};

typedef struct glibc_mallinfo   (*mallinfo_func_t)(void);
typedef struct glibc_mallinfo2  (*mallinfo2_func_t)(void);

static mallinfo_func_t  g_mallinfo = NULL;
static mallinfo2_func_t g_mallinfo2 = NULL;

static void mallinfo_init() {
  g_mallinfo = CAST_TO_FN_PTR(mallinfo_func_t, dlsym(RTLD_DEFAULT, "mallinfo"));
  g_mallinfo2 = CAST_TO_FN_PTR(mallinfo2_func_t, dlsym(RTLD_DEFAULT, "mallinfo2"));
}

#undef MALLINFO_MEMBER_DO

#endif // __GLIBC__

// Helper function, returns true if string is a numerical id
static bool is_numerical_id(const char* s) {
  const char* p = s;
  while(*p >= '0' && *p <= '9') {
    p ++;
  }
  return *p == '\0' ? true : false;
}

/////////////// cgroup stuff
// We use part of the hotspot cgroup wrapper, but not all of it.
// The reason:
// - wrapper uses UL heavily, which I don't want to happen in a sampler thread (I only log in initialization, which is ok)
// - wrapper does not expose all metrics I need (eg kmem)
// What the wrapper does very nicely is the parse stuff, which I don't want to re-invent, therefore
// I use the wrapper to get the controller path.

class CGroups : public AllStatic {

  static bool _containerized;
  static const char* _file_usg;
  static const char* _file_usgsw;
  static const char* _file_lim;
  static const char* _file_limsw;
  static const char* _file_slim;
  static const char* _file_kusg;

public:

  static bool initialize() {

    // For the heck of it, I go through with initialization even if we are not
    // containerized, since I like to know controller paths even for those cases.

    _containerized = OSContainer::is_containerized();
    log_debug(vitals)("Vitals cgroup initialization: containerized = %d", _containerized);

    const char* controller_path = sapmachine_get_memory_controller_path();
    if (controller_path == NULL) {
      log_debug(vitals)("Vitals cgroup initialization: controller path NULL");
      return false;
    }
    size_t pathlen = ::strlen(controller_path);
    if (pathlen == 0) {
      log_debug(vitals)("Vitals cgroup initialization: controller path empty?");
      return false;
    }
    stringStream path;
    if (controller_path[pathlen - 1] == '/') {
      path.print("%s", controller_path);
    } else {
      path.print("%s/", controller_path);
    }

    log_debug(vitals)("Vitals cgroup initialization: controller path: %s", path.base());

    // V1 or V2?
    stringStream ss;
    ss.print("%smemory.usage_in_bytes", path.base());
    struct stat s;
    const bool isv1 = os_file_exists(ss.base());
    if (isv1) {
      log_debug(vitals)("Vitals cgroup initialization: v1");
    } else  {
      ss.reset();
      ss.print("%smemory.current", path.base());
      if (os_file_exists(ss.base())) {
        // okay, its v2
        log_debug(vitals)("Vitals cgroup initialization: v2");
      } else {
        log_debug(vitals)("Vitals cgroup initialization: no clue. Giving up.");
        return false;
      }
    }

    _file_usg = os::strdup(ss.base()); // so, we have that.

#define STORE_PATH(variable, filename) \
  ss.reset(); ss.print("%s%s", path.base(), filename); variable = os::strdup(ss.base());

    if (isv1) {
      STORE_PATH(_file_usgsw, "memory.memsw.usage_in_bytes");
      STORE_PATH(_file_kusg, "memory.kmem.usage_in_bytes");
      STORE_PATH(_file_lim, "memory.limit_in_bytes");
      STORE_PATH(_file_limsw, "memory.memsw.limit_in_bytes");
      STORE_PATH(_file_slim, "memory.soft_limit_in_bytes");
    } else {
      STORE_PATH(_file_usgsw, "memory.swap.current");
      STORE_PATH(_file_kusg, "memory.kmem.usage_in_bytes");
      STORE_PATH(_file_lim, "memory.max");
      STORE_PATH(_file_limsw, "memory.swap.max");
      STORE_PATH(_file_slim, "memory.low");
    }
#undef STORE_PATH

#define LOG_PATH(variable) \
		log_debug(vitals)("Vitals: %s=%s", #variable, variable == NULL ? "<null>" : variable);
    LOG_PATH(_file_usg)
    LOG_PATH(_file_usgsw)
    LOG_PATH(_file_kusg)
    LOG_PATH(_file_lim)
    LOG_PATH(_file_limsw)
    LOG_PATH(_file_slim)
#undef LOG_PATH

    // Initialization went through. We show columns if we are containerized.
    return _containerized;
  }

  struct cgroup_values_t {
    value_t lim;
    value_t limsw;
    value_t slim;
    value_t usg;
    value_t usgsw;
    value_t kusg;
  };

  static bool get_stats(cgroup_values_t* v) {
    v->lim = v->limsw = v->slim = v->usg = v->usgsw = v->kusg = INVALID_VALUE;
    ProcFile pf;
#define GET_VALUE(var) \
  { \
    const char* what = _file_ ## var; \
    if (what != NULL && pf.read(what)) { \
      v-> var = pf.as_value(1); \
    } \
  }
  GET_VALUE(usg);
  GET_VALUE(usgsw);
  GET_VALUE(kusg);
  GET_VALUE(lim);
  GET_VALUE(limsw);
  GET_VALUE(slim);
#undef GET_VALUE
    // Cgroup limits defaults to PAGE_COUNTER_MAX in the kernel; so a very large number means "no limit"
    // Note that on 64-bit, the default is LONG_MAX aligned down to pagesize; but I am not sure this is
    // always true, so I just assume a very high value.
    const size_t practically_infinite = LP64_ONLY(128 * K * G) NOT_LP64(4 * G);
    if (v->lim > practically_infinite)    v->lim = INVALID_VALUE;
    if (v->slim > practically_infinite)   v->slim = INVALID_VALUE;
    if (v->limsw > practically_infinite)  v->limsw = INVALID_VALUE;
    return true;

  } // end: CGroups::get_stats()

}; // end: CGroups

bool CGroups::_containerized = false;
const char* CGroups::_file_usg = NULL;
const char* CGroups::_file_usgsw = NULL;
const char* CGroups::_file_lim = NULL;
const char* CGroups::_file_limsw = NULL;
const char* CGroups::_file_slim = NULL;
const char* CGroups::_file_kusg = NULL;

void OSWrapper::update_if_needed() {

  time_t t;
  time(&t);
  if (t != (time_t)-1 &&
      t < (_last_update + num_seconds_until_update)) {
    return; // still good
  }
  _last_update = t;

  static bool first_call = true;

  // Update Values from ProcFS (and elsewhere)
#define RESETVAL(name) _ ## name = INVALID_VALUE;
ALL_VALUES_DO(RESETVAL)
#undef RESETVAL

  ProcFile bf;
  if (bf.read("/proc/meminfo")) {

    if (first_call) {
      log_trace(vitals)("Read /proc/meminfo: \n%s", bf.text());
    }

    // All values in /proc/meminfo are in KB
    const size_t scale = K;

    _syst_phys = bf.parsed_prefixed_value("MemTotal:", scale);
    _syst_avail = bf.parsed_prefixed_value("MemAvailable:", scale);

    const value_t swap_total = bf.parsed_prefixed_value("SwapTotal:", scale);
    const value_t swap_free = bf.parsed_prefixed_value("SwapFree:", scale);
    if (swap_total != INVALID_VALUE && swap_free != INVALID_VALUE) {
      _syst_swap = swap_total - swap_free;
    }

    // Calc committed ratio. Values > 100% indicate overcommitment.
    const value_t commitlimit = bf.parsed_prefixed_value("CommitLimit:", scale);
    const value_t committed = bf.parsed_prefixed_value("Committed_AS:", scale);
    if (commitlimit != INVALID_VALUE && commitlimit != 0 && committed != INVALID_VALUE) {
      _syst_comm = committed;
      const value_t ratio = (committed * 100) / commitlimit;
      _syst_crt = ratio;
    }

  }

  if (bf.read("/proc/vmstat")) {
    _syst_si = bf.parsed_prefixed_value("pswpin");
    _syst_so = bf.parsed_prefixed_value("pswpout");
  }

  if (bf.read("/proc/stat")) {
    // Read and parse global cpu values
    cpu_values_t values;
    const char* line = bf.get_prefixed_line("cpu");
    parse_proc_stat_cpu_line(line, &values);

    _syst_cpu_us = values.user + values.nice;
    _syst_cpu_sy = values.system;
    _syst_cpu_id = values.idle;
    _syst_cpu_st = values.steal;
    _syst_cpu_gu = values.guest + values.guest_nice;

    // procs_running: this is actually number of threads running
    // procs_blocked: number of threads blocked on real disk IO
    // See https://utcc.utoronto.ca/~cks/space/blog/linux/ProcessStatesAndProcStat
    // and https://lore.kernel.org/lkml/12601530441257@xenotime.net/#t
    // and the canonical man page description at https://www.kernel.org/doc/Documentation/filesystems/proc.txt
    _syst_tr = bf.parsed_prefixed_value("procs_running");
    _syst_tb = bf.parsed_prefixed_value("procs_blocked");
  }

  // cgroups business
  CGroups::cgroup_values_t v;
  if (CGroups::get_stats(&v)) {
    _syst_cgro_usg = v.usg;
    _syst_cgro_usgsw = v.usgsw;
    _syst_cgro_kusg = v.kusg;
    _syst_cgro_lim = v.lim;
    _syst_cgro_limsw = v.limsw;
    _syst_cgro_slim = v.slim;
  }

  if (bf.read("/proc/self/status")) {

    _proc_virt = bf.parsed_prefixed_value("VmSize:", K);
    _proc_swdo = bf.parsed_prefixed_value("VmSwap:", K);
    _proc_rss_all = bf.parsed_prefixed_value("VmRSS:", K);
    _proc_rss_anon = bf.parsed_prefixed_value("RssAnon:", K);
    _proc_rss_file = bf.parsed_prefixed_value("RssFile:", K);
    _proc_rss_shm = bf.parsed_prefixed_value("RssShmem:", K);

    _proc_thr = bf.parsed_prefixed_value("Threads:");

  }

  // Number of open files: iterate over /proc/self/fd and count.
  {
    DIR* d = ::opendir("/proc/self/fd");
    if (d != NULL) {
      value_t v = 0;
      struct dirent* en = NULL;
      do {
        en = ::readdir(d);
        if (en != NULL) {
          v ++;
        }
      } while(en != NULL);
      ::closedir(d);
      assert(v >= 2, "should have read at least '.' and '..'");
      v -= 2; // We discount . and ..
      _proc_io_of = v;
    }
  }

  // Number of processes: iterate over /proc/<pid> and count.
  // Number of threads: read "num_threads" from /proc/<pid>/stat
  {
    DIR* d = ::opendir("/proc");
    if (d != NULL) {
      value_t v_p = 0;
      value_t v_t = 0;
      struct dirent* en = NULL;
      do {
        en = ::readdir(d);
        if (en != NULL) {
          if (is_numerical_id(en->d_name)) {
            v_p ++;
            char tmp[128];
            jio_snprintf(tmp, sizeof(tmp), "/proc/%s/stat", en->d_name);
            if (bf.read(tmp)) {
              const char* text = bf.text();
              // See man proc(5)
              // (20) num_threads  %ld
              long num_threads = 0;
              ::sscanf(text, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %ld", &num_threads);
              v_t += num_threads;
            }
          }
        }
      } while(en != NULL);
      ::closedir(d);
      _syst_p = v_p;
      _syst_t = v_t;
    }
  }

  if (bf.read("/proc/self/io")) {
    _proc_io_rd = bf.parsed_prefixed_value("rchar:");
    _proc_io_wr = bf.parsed_prefixed_value("wchar:");
  }

  if (bf.read("/proc/self/stat")) {
    const char* text = bf.text();
    // See man proc(5)
    // (14) utime  %lu
    // (15) stime  %lu
    long unsigned cpu_utime = 0;
    long unsigned cpu_stime = 0;
    ::sscanf(text, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &cpu_utime, &cpu_stime);
    _proc_cpu_us = cpu_utime;
    _proc_cpu_sy = cpu_stime;
  }

#ifdef __GLIBC__
  // Note "glibc heap used", from experiments and glibc source code reading, would be aprox. the sum
  //  of mmaped data area size (contains large allocations) and the small block sizes.
  if (g_mallinfo2 != NULL) {
    glibc_mallinfo2 mi = g_mallinfo2();
    _proc_chea_usd = mi.uordblks + mi.hblkhd;
    _proc_chea_free = mi.fordblks;
  } else if (g_mallinfo != NULL) {
    // disregard output from old style mallinfo if rss > 4g, since we cannot
    // know whether we wrapped. For rss < 4g, we know values in mallinfo cannot
    // have wrapped.
    if (LP64_ONLY(_proc_rss_all < (4 * G)) NOT_LP64(true)) {
      glibc_mallinfo mi = g_mallinfo();
      _proc_chea_usd = (value_t)(unsigned)mi.uordblks + (value_t)(unsigned)mi.hblkhd;
      _proc_chea_free = (value_t)(unsigned)mi.fordblks;
    }
  }
#endif // __GLIBC__

  first_call = false;

}

bool OSWrapper::initialize() {
#ifdef __GLIBC__
  mallinfo_init();
#endif
  return CGroups::initialize();
}

} // namespace sapmachine_vitals
