/*
 * Copyright (c) 2024 SAP SE. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

static int attach_file_fd = -1;
static char attach_path[256];

static void cleanup() {
  if (attach_file_fd >= 0) {
    unlink(attach_path);
  }
}

static int file_exists(char const* path) {
  struct stat stat_buf;

  return stat(path, &stat_buf) == 0;
}

static void fail(char const* msg) {
  if (msg) {
    fprintf(stderr, "%s", msg);
  }

  exit(1);
}

static void fail_with_errno(char const* msg) {
  perror(msg);
  fail(NULL);
}

static void write_full(int fd, char const* buf, int size) {
  ssize_t written;

  while ((written = write(fd, buf, size)) != size) {
    if (written <= 0) {
      fail_with_errno("Could not write");
    }

    buf += written;
    size -= written;
  }
}

static void write_tag(int fd, char const* str) {
  write_full(fd, str, strlen(str) + 1);
}

static void write_string(int fd, char const* str) {
  write_full(fd, str, strlen(str));
}

static void run_jcmd(pid_t pid, int argc, char *argv[]) {
  char pid_file[256];
  sprintf(pid_file, "/tmp/.java_pid%d", (int) pid);

  if (!file_exists(pid_file)) {
    return;
  }

  int fd = socket(PF_UNIX, SOCK_STREAM, 0);

  if (fd < 0) {
    fail_with_errno("Could not create file socket");
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, pid_file, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
    fail_with_errno("Could not connect to file socket");
  }

  write_tag(fd, "1");
  write_tag(fd, "jcmd");

  if (argc == 0) {
    write_tag(fd, "help");
  } else {
    for (int i = 0; i < argc; ++i) {
      if (i > 0) {
        write_string(fd, " ");
      }

      write_string(fd, argv[i]);
    }

    write_tag(fd, "");
  }

  write_tag(fd, "");
  write_tag(fd, "");

  char buf[256];
  int c = read(fd, buf, sizeof(buf));

  if (c <= 0) {
    fail("Missing result code\n");
  }

  int result_code;

  if (sscanf(buf, "%d", &result_code) != 1) {
    fail("Invalid result code\n");
  }

  printf("%d:\n", pid);
  fflush(stdout);

  for (int i = 0; i < c; ++i) {
    if (buf[i] == '\n') {
      write_full(1, buf + i + 1, c - i - 1);
      break;
    }
  }

  while ((c = read(fd, buf, sizeof(buf))) > 0) {
    write_full(1, buf, c);
  }

  exit(result_code == 0 ? 0 : 1);
}

static void trigger_attach(int pid) {
  snprintf(attach_path, sizeof(attach_path), "/tmp/.attach_pid%d", pid);

  // Create .attach_pid file if not already present.
  if (!file_exists(attach_path)) {
    attach_file_fd = creat(attach_path, S_IRWXU);

    if (attach_file_fd < 0) {
      fail_with_errno("Could not create .attach_pid file");
    }

    atexit(cleanup);
  }

  if (kill(pid, SIGQUIT) != 0) {
    fail_with_errno("Could not signal pid");
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fail("Missing pid!\n");
  }

  char* endptr;
  long raw_pid = strtol(argv[1], &endptr, 10);

  if ((argv[1][0] == '\0') || (endptr[0] != '\0')) {
    fail("Could not parse pid.\n");
  }

  pid_t pid = (pid_t) raw_pid;

  if ((raw_pid <= 0) || (raw_pid == LONG_MAX) || (raw_pid != pid)) {
    fail("Pid is not in valid range.\n");
  }

  run_jcmd(pid, argc - 2, argv + 2);

  // If attaching failed, trigger it and try again.
  trigger_attach(pid);

  for (int i = 0; i < 10; ++i) {
    usleep(10000 * (1 << i));
    run_jcmd(pid, argc - 2, argv + 2);
  }

  fail("Connection to VM timed out.\n");

  return 0;
}

