/*
 * Copyright (c) 2018, 2024, Oracle and/or its affiliates. All rights reserved.
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


#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <afunix.h>

#define SOCKET_HANDLE SOCKET
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#define CLOSE_SOCKET_FUNC closesocket
#define CALL_INTERRUPTED (WSAGetLastError() == WSAEINTR)

#else

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
#define SOCKET_HANDLE int
#define INVALID_SOCKET_HANDLE -1
#define CLOSE_SOCKET_FUNC close
#define CALL_INTERRUPTED (errno == EINTR)
#define DELETE_FILE unlink
#endif


#if defined(_WIN32)
/* Make sure winsock is initialized on Windows */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    WSADATA wsadata;

    if ((reason == DLL_PROCESS_ATTACH) && (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)) {
        return JNI_FALSE;
    }
    else if (reason == DLL_PROCESS_DETACH) {
        WSACleanup();
    }

    return TRUE;
}
#endif

static SOCKET_HANDLE server_socket = INVALID_SOCKET_HANDLE;
static SOCKET_HANDLE connection_socket = INVALID_SOCKET_HANDLE;

static int readSocket(SOCKET_HANDLE socket, char* buf, int len) {
    int result;

    do {
#if defined(_WIN32)
        result = recv(socket, buf, len, 0);
#else
        result = read(socket, buf, len);
#endif
    } while ((result < 0) && CALL_INTERRUPTED);

    return result;
}

static int writeSocket(SOCKET_HANDLE socket, char* buf, int len) {
    int result;

    do {
#if defined(_WIN32)
        result = send(socket, buf, len, 0);
#else
        result = write(socket, buf, len);
#endif
    } while ((result < 0) && CALL_INTERRUPTED);

    return result;
}

static char* getErrorMsg(char* buf, size_t len) {
#if defined(_WIN32)
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, (DWORD) len, NULL);
    return buf;
#else
    return strerror(errno);
#endif
}
static void closeSocket(SOCKET_HANDLE* socket) {
    if (*socket != INVALID_SOCKET_HANDLE) {
        int rv = -1;

        do {
            rv = CLOSE_SOCKET_FUNC(*socket);
        } while ((rv != 0) && CALL_INTERRUPTED);


        *socket = INVALID_SOCKET_HANDLE;
    }
}

static char file_to_delete[UNIX_PATH_MAX];
static volatile int file_to_delete_valid;

static void memoryBarrier() {
#if defined(_WIN32)
    MemoryBarrier();
#elif defined(__linux__) || defined(__APPLE__)
    __sync_synchronize();
#elif define(_AIX)
    __sync();
#else
#error "Unknown platform"
#endif
}

static jboolean deleteFile(char const* name) {
#if defined(_WIN32)
    if ((!DeleteFile(name)) && (GetLastError() != ERROR_FILE_NOT_FOUND)) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
#else
    return (access(name, F_OK) == -1) || (unlink(name) == 0) ? JNI_TRUE : JNI_FALSE;
#endif
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
    return connection_socket == INVALID_SOCKET_HANDLE ? JNI_FALSE : JNI_TRUE;
}

void fileSocketTransport_CloseImpl() {
    closeSocket(&server_socket);
    closeSocket(&connection_socket);
}

void logAndCleanupFailedAccept(char const* error_msg, char const* name) {
    char buf[256];
    fileSocketTransport_logError("%s: socket %s: %s", error_msg, name, getErrorMsg(buf, sizeof(buf)));
    fileSocketTransport_CloseImpl();
    registerFileToDelete(NULL);
}

void fileSocketTransport_AcceptImpl(char const* name) {
    static int already_called = 0;

    if (!already_called) {
        atexit(cleanupSocketOnExit);
        already_called = 1;
    }

    if (server_socket == INVALID_SOCKET_HANDLE) {
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

        if (server_socket == INVALID_SOCKET_HANDLE) {
            logAndCleanupFailedAccept("Could not create doamin socket", name);
            return;
        }

        if (!deleteFile(name)) {
            logAndCleanupFailedAccept("Could not remove file to create new file socket", name);
            return;
        }

        registerFileToDelete(name);

        if (bind(server_socket, (struct sockaddr*) &addr, addr_size) == -1) {
            logAndCleanupFailedAccept("Could not bind file socket", name);
            return;
        }
#if !defined(_WIN32)
        if (chmod(name, (S_IREAD | S_IWRITE) & ~(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) == -1) {
            logAndCleanupFailedAccept("Chmod on file socket failed", name);
            return;
        }

        if (chown(name, geteuid(), getegid()) == -1) {
            logAndCleanupFailedAccept("Chown on file socket failed", name);
            return;
        }
#endif

        if (listen(server_socket, 1) == -1) {
            logAndCleanupFailedAccept("Could not listen on file socket", name);
            return;
        }
    }

    do {
        connection_socket = accept(server_socket, NULL, NULL);
    } while ((connection_socket == INVALID_SOCKET_HANDLE) && CALL_INTERRUPTED);

    /* We can remove the file since we are connected (or it failed). */
    deleteFile(name);
    closeSocket(&server_socket);

    if (connection_socket == INVALID_SOCKET_HANDLE) {
        logAndCleanupFailedAccept("Could not accept on file socket", name);
    }
    else {
#if !defined(_WIN32)
        uid_t other_user = (uid_t)-1;
        gid_t other_group = (gid_t)-1;

        /* Check if the connected user is the same as the user running the VM. */
#if defined(__linux__)
        struct ucred cred_info;
        socklen_t optlen = sizeof(cred_info);

        if (getsockopt(connection_socket, SOL_SOCKET, SO_PEERCRED, (void*)&cred_info, &optlen) == -1) {
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
#endif
    }
}

int fileSocketTransport_ReadImpl(char* buffer, int size) {
    int result = readSocket(connection_socket, buffer, size);

    if (result < 0) {
        char buf[256];
        fileSocketTransport_logError("Read failed with result %d: %s", result, getErrorMsg(buf, sizeof(buf)));
    }

    return result;
}

int fileSocketTransport_WriteImpl(char* buffer, int size) {
    int result = writeSocket(connection_socket, buffer, size);

    if (result < 0) {
        char buf[256];
        fileSocketTransport_logError("Write failed with result %d: %s", result, getErrorMsg(buf, sizeof(buf)));
    }

    return result;
}

char* fileSocketTransport_GetDefaultAddress() {
    return NULL;
}

