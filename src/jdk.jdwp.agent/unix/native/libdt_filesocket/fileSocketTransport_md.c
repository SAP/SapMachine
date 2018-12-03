/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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

#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/times.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>

#ifdef __sun
#include <sys/filio.h>
#endif

// MacOS X does not declare the macros SIGRTMIN and SIGRTMAX
#ifdef __APPLE__
  #ifndef SIGRTMAX
    #define SIGRTMAX SIGUSR2
  #endif
#endif

#include "jni.h"
#include "fileSocketTransport.h"

#define INVALID_HANDLE_VALUE -1
static int server_handle = INVALID_HANDLE_VALUE;
static int handle = INVALID_HANDLE_VALUE;

static void closeServerHandle() {
    if (server_handle != INVALID_HANDLE_VALUE) {
        int rv = -1;

        do {
           rv = close(server_handle);
        } while (rv == -1 && errno == EINTR);


        server_handle = INVALID_HANDLE_VALUE;
    }
}

jboolean fileSocketTransport_HasValidHandle() {
    return handle == INVALID_HANDLE_VALUE ? JNI_FALSE : JNI_TRUE;
}

void fileSocketTransport_CloseImpl() {
    if (handle != INVALID_HANDLE_VALUE) {
        int rv = -1;

        shutdown(handle, SHUT_RDWR);

        do {
           rv = close(handle);
        } while (rv == -1 && errno == EINTR);


        handle = INVALID_HANDLE_VALUE;
    }
}

void fileSocketTransport_AcceptImpl(char const* name) {
    struct sockaddr_un addr;
    socklen_t len = sizeof(struct sockaddr_un);

    if (server_handle == INVALID_HANDLE_VALUE) {
        struct sockaddr_un addr;
#ifdef _AIX
        // on some as400 releases we need this
        const int addr_size = SUN_LEN(&addr);
#else
        const int addr_size = sizeof(addr);
#endif
        memset((void *) &addr, 0, len);
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);

        server_handle = socket(PF_UNIX, SOCK_STREAM, 0);

        if (server_handle == INVALID_HANDLE_VALUE) {
            return;
        }

        if ((access(name, F_OK )) != -1 && (unlink(name) != 0)) {
            log_error("Could not remove %s to create new file socket", name);
            closeServerHandle();
            return;
        }

        if (bind(server_handle, (struct sockaddr*) &addr, addr_size) == -1) {
            log_error("Could not bind file socket %s: %s", name, strerror(errno));
            closeServerHandle();
            return;
        }


        if (listen(server_handle, 1) == -1) {
            log_error("Could not listen on file socket %s: %s", name, strerror(errno));
            unlink(name);
            closeServerHandle();
            return;
        }

        if (chmod(name, (S_IREAD|S_IWRITE) &  ~(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) == -1) {
            log_error("Chmod on %s failed: %s", strerror(errno));
            unlink(name);
            closeServerHandle();
            return;
        }
  }

  memset((void *) &addr, 0, len);

  do {
    handle = accept(server_handle, (struct sockaddr *) &addr, &len);
  } while (server_handle == INVALID_HANDLE_VALUE && errno == EINTR);

  if (handle == INVALID_HANDLE_VALUE) {
    log_error("Could not accept on file socket %s: %s", strerror(errno));
  }
}

int fileSocketTransport_ReadImpl(char* buffer, int size) {
    int result = read(handle, buffer, size);

    if (result <= 0) {
        log_error("Read failed with result %d: %s", result, strerror(errno));
    }

    return result;
}

int fileSocketTransport_WriteImpl(char* buffer, int size) {
    int result = write(handle, buffer, size);

    if (result <= 0) {
        log_error("Read failed with result %d: %s", result, strerror(errno));
    }

    return result;
}
