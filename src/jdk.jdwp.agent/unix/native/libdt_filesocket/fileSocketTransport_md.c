/*
 * Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.
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
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef __sun
#include <ucred.h>
#endif

#include "jni.h"
#include "fileSocketTransport.h"

#define INVALID_HANDLE_VALUE -1
static int server_handle = INVALID_HANDLE_VALUE;
static int handle = INVALID_HANDLE_VALUE;

static void closeHandle(int* handle) {
    if (*handle != INVALID_HANDLE_VALUE) {
        int rv = -1;

        do {
           rv = close(*handle);
        } while (rv == -1 && errno == EINTR);


        *handle = INVALID_HANDLE_VALUE;
    }
}

jboolean fileSocketTransport_HasValidHandle() {
    return handle == INVALID_HANDLE_VALUE ? JNI_FALSE : JNI_TRUE;
}

void fileSocketTransport_CloseImpl() {
    closeHandle(&server_handle);
    closeHandle(&handle);
}

void logAndCleanupFailedAccept(char const* error_msg, char const* name) {
    fileSocketTransport_logError("%s: socket %s: %s", error_msg, name, strerror(errno));
    fileSocketTransport_CloseImpl();
}

void fileSocketTransport_AcceptImpl(char const* name) {
    if (server_handle == INVALID_HANDLE_VALUE) {
        socklen_t len = sizeof(struct sockaddr_un);
        int addr_size = sizeof(addr);
        struct sockaddr_un addr;

        memset((void *) &addr, 0, len);
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);

#ifdef _AIX
        addr.sun_len = strlen(addr.sun_path);
        addr_size = SUN_LEN(&addr);
#endif

        server_handle = socket(PF_UNIX, SOCK_STREAM, 0);

        if (server_handle == INVALID_HANDLE_VALUE) {
            logAndCleanupFailedAccept("Could not create doamin socket", name);
            return;
        }

        if ((access(name, F_OK )) != -1 && (unlink(name) != 0)) {
            logAndCleanupFailedAccept("Could not remove file to create new file socket", name);
            return;
        }

        if (bind(server_handle, (struct sockaddr*) &addr, addr_size) == -1) {
            logAndCleanupFailedAccept("Could not bind file socket", name);
            return;
        }

        if (chmod(name, (S_IREAD|S_IWRITE) &  ~(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) == -1) {
            logAndCleanupFailedAccept("Chmod on file socket failed", name);
            return;
        }

        if (chown(name, geteuid(), getegid()) == -1) {
            logAndCleanupFailedAccept("Chown on file socket failed", name);
            return;
        }

        if (listen(server_handle, 1) == -1) {
            logAndCleanupFailedAccept("Could not listen on file socket", name);
            return;
        }
    }

    do {
        handle = accept(server_handle, NULL, NULL);
    } while (handle == -1 && errno == EINTR);

    /* We can remove the file since we are connected (or it failed). */
    unlink(name);

    if (handle == INVALID_HANDLE_VALUE) {
        logAndCleanupFailedAccept("Could not accept on file socket", name);
    } else {
        uid_t other_user;
        gid_t other_group;

        /* Check if the connected user is the same as the user running the VM. */
#ifdef __linux__
        struct ucred cred_info;
        socklen_t optlen = sizeof(cred_info);

        if (getsockopt(handle, SOL_SOCKET, SO_PEERCRED, (void*)&cred_info, &optlen) == -1) {
            logAndCleanupFailedAccept("Failed to get socket option SO_PEERCRED of file socket", name);
            return;
        }

        other_user = cred_info.uid;
        other_group = cred_info.gid;
#elif __APPLE__
        if (getpeereid(handle, &other_user, &other_group) != 0) {
            logAndCleanupFailedAccept("Failed to get peer id of file socket", name);
            return;
        }
#elif _AIX
        struct peercred_struct cred_info;
        socklen_t optlen = sizeof(cred_info);

        if (getsockopt(handle, SOL_SOCKET, SO_PEERID, (void*) &cred_info, &optlen) == -1) {
            logAndCleanupFailedAccept("Failed to get socket option SO_PEERID of file socket", name);
            return;
        }

        other_user = cred_info.euid;
        other_group = cred_info.egid;
#elif __sun
        ucred_t* cred_info = NULL;

        if (getpeerucred(handle, &cred_info) == -1) {
            logAndCleanupFailedAccept("Failed to peer credientials of file socket", name);
            return;
        }

        other_user = ucred_geteuid(cred_info);
        other_group = ucred_getegid(cred_info);
        ucred_free(cred_info);
#else
#error "Unknown platform"
#endif

        if (other_user != geteuid()) {
            fileSocketTransport_logError("Cannot allow user %d to connect to file socket %s of user %d",
                                         (int) other_user, name, (int) geteuid());
            fileSocketTransport_CloseImpl();
        } else if (other_group != getegid()) {
            fileSocketTransport_logError("Cannot allow user %d (groupd %d) to connect to file socket "
                                         "%s of user %d (group %d)", (int) other_user, (int) other_group,
                                         name, (int) geteuid(), (int) getegid());
            fileSocketTransport_CloseImpl();
        }
    }
}

int fileSocketTransport_ReadImpl(char* buffer, int size) {
    int result;

    do {
        result = read(handle, buffer, size);
    } while (result == -1 && errno == EINTR);

    if (result < 0) {
        fileSocketTransport_logError("Read failed with result %d: %s", result, strerror(errno));
    }

    return result;
}

int fileSocketTransport_WriteImpl(char* buffer, int size) {
    int result;

    do {
        result = write(handle, buffer, size);
    } while (result == -1 && errno == EINTR);

    if (result < 0) {
        fileSocketTransport_logError("Write failed with result %d: %s", result, strerror(errno));
    }

    return result;
}

static char default_name[160] = { 0, };

char* fileSocketTransport_GetDefaultAddress() {
    if (default_name[0] == '\0') {
        snprintf(default_name, sizeof(default_name), "/tmp/dt_filesocket_%lld_%lld", (long long) getpid(), (long long) geteuid());
        default_name[sizeof(default_name) - 1] = '\0';
    }

    return default_name;
}
