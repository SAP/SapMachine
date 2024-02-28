/*
 * Copyright (c) 2018, 2024 SAP SE. All rights reserved.
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
#include <assert.h>

#include "jni.h"
#include "jvm.h"
#include "fileSocketTransport.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>

#define UNIX_PATH_MAX sizeof(((struct sockaddr_un *) 0)->sun_path)

static int server_socket = -1;
static int connection_socket = -1;

static void closeSocket(int* socket) {
    if (*socket != -1) {
        int rv = -1;

#if defined(_AIX)
	do {
            rv = close(*socket);
        } while ((rv != 0) && (errno == EINTR));
#else
	rc = close(*socket);
#endif

        *socket = -1;
    }
}

static char file_to_delete[UNIX_PATH_MAX];
static volatile int file_to_delete_valid;

static void memoryBarrier() {
#if defined(__linux__) || defined(__APPLE__)
    __sync_synchronize();
#elif define(_AIX)
    __sync();
#else
#error "Unknown platform"
#endif
}

static jboolean deleteFile(char const* name) {
    return (access(name, F_OK) == -1) || (unlink(name) == 0) ? JNI_TRUE : JNI_FALSE;
}

static void registerFileToDelete(char const* name) {
    if (file_to_delete_valid == 0) {
        if ((name != NULL) && (strlen(name) + 1 <= sizeof(file_to_delete))) {
            strcpy(file_to_delete, name);
            memoryBarrier();
            file_to_delete_valid = 1;
            memoryBarrier();
        }
    } else {
        // Should never change.
        assert(strcmp(name, file_to_delete) == 0);
    }
}

static void cleanupSocketOnExit(void) {
    memoryBarrier();

    if (file_to_delete_valid) {
        memoryBarrier();
        deleteFile(file_to_delete);
    }
}

jboolean fileSocketTransport_HasValidHandle() {
    return connection_socket == -1 ? JNI_FALSE : JNI_TRUE;
}

void fileSocketTransport_CloseImpl() {
    closeSocket(&server_socket);
    closeSocket(&connection_socket);
}

void logAndCleanupFailedAccept(char const* error_msg, char const* name) {
    fileSocketTransport_logError("%s: socket %s: %s", error_msg, name, strerror(errno));
    fileSocketTransport_CloseImpl();
}

void fileSocketTransport_AcceptImpl(char const* name) {
    static int already_called = 0;

    if (!already_called) {
        registerFileToDelete(name);
        atexit(cleanupSocketOnExit);
        already_called = 1;
    }

    if (server_socket == -1) {
        socklen_t len = sizeof(struct sockaddr_un);
        struct sockaddr_un addr;
        int addr_size = sizeof(addr);

        memset((void*)&addr, 0, len);
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);

#ifdef _AIX
        addr.sun_len = strlen(addr.sun_path);
        addr_size = SUN_LEN(&addr);
#endif

        server_socket = socket(PF_UNIX, SOCK_STREAM, 0);

        if (server_socket == -1) {
            logAndCleanupFailedAccept("Could not create doamin socket", name);
            return;
        }

        if (!deleteFile(name)) {
            logAndCleanupFailedAccept("Could not remove file to create new file socket", name);
            return;
        }

        if (bind(server_socket, (struct sockaddr*) &addr, addr_size) == -1) {
            logAndCleanupFailedAccept("Could not bind file socket", name);
            return;
        }

        if (chmod(name, (S_IREAD | S_IWRITE) & ~(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) == -1) {
            logAndCleanupFailedAccept("Chmod on file socket failed", name);
            return;
        }

        if (chown(name, geteuid(), getegid()) == -1) {
            logAndCleanupFailedAccept("Chown on file socket failed", name);
            return;
        }

        if (listen(server_socket, 1) == -1) {
            logAndCleanupFailedAccept("Could not listen on file socket", name);
            return;
        }
    }

    do {
        connection_socket = accept(server_socket, NULL, NULL);
    } while ((connection_socket == -1) && (errno == EINTR));

    /* We can remove the file since we are connected (or it failed). */
    deleteFile(name);
    closeSocket(&server_socket);

    if (connection_socket == -1) {
        logAndCleanupFailedAccept("Could not accept on file socket", name);
        return;
    }

    uid_t other_user = (uid_t)-1;
    gid_t other_group = (gid_t)-1;

    /* Check if the connected user is the same as the user running the VM. */
#if defined(__linux__)
    struct ucred cred_info;
    socklen_t optlen = sizeof(cred_info);

    if (getsockopt(connection_socket, SOL_SOCKET, SO_PEERCRED, (void*) &cred_info, &optlen) == -1) {
        logAndCleanupFailedAccept("Failed to get socket option SO_PEERCRED of file socket", name);
        return;
    }

    other_user = cred_info.uid;
    other_group = cred_info.gid;
#elif defined(__APPLE__)
    if (getpeereid(connection_socket, &other_user, &other_group) != 0) {
        logAndCleanupFailedAccept("Failed to get peer id of file socket", name);
        return;
    }
#elif defined(_AIX)
    struct peercred_struct cred_info;
    socklen_t optlen = sizeof(cred_info);

    if (getsockopt(connection_socket, SOL_SOCKET, SO_PEERID, (void*)&cred_info, &optlen) == -1) {
        logAndCleanupFailedAccept("Failed to get socket option SO_PEERID of file socket", name);
        return;
    }

    other_user = cred_info.euid;
    other_group = cred_info.egid;
#else
#error "Unknown platform"
#endif

    if (other_user != geteuid()) {
        fileSocketTransport_logError("Cannot allow user %d to connect to file socket %s of user %d",
            (int)other_user, name, (int)geteuid());
        fileSocketTransport_CloseImpl();
    }
    else if (other_group != getegid()) {
        fileSocketTransport_logError("Cannot allow user %d (group %d) to connect to file socket "
            "%s of user %d (group %d)", (int)other_user, (int)other_group,
            name, (int)geteuid(), (int)getegid());
        fileSocketTransport_CloseImpl();
    }
}

int fileSocketTransport_ReadImpl(char* buffer, int size) {
    int result;

    do {
        result = read(connection_socket, buffer, size);
    } while ((result < 0) && (errno == EINTR));

    if (result < 0) {
        fileSocketTransport_logError("Read failed with result %d: %s", result, strerror(errno));
    }

    return result;
}

int fileSocketTransport_WriteImpl(char* buffer, int size) {
    int result;

    do {
        result = write(connection_socket, buffer, size);
    } while ((result < 0) && (errno == EINTR));

    if (result < 0) {
        fileSocketTransport_logError("Write failed with result %d: %s", result, strerror(errno));
    }

    return result;
}

