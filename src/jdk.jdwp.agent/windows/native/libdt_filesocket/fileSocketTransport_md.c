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


#include <windows.h>
#include <winsock2.h>
#include <afunix.h>

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

static SOCKET server_socket = INVALID_SOCKET;
static SOCKET connection_socket = INVALID_SOCKET;

static void closeSocket(SOCKET* socket) {
    if (*socket != INVALID_SOCKET) {
        closesocket(*socket);
        *socket = INVALID_SOCKET;
    }
}

static char file_to_delete[UNIX_PATH_MAX];
static volatile int file_to_delete_valid;

static jboolean deleteFile(char const* name) {
    if ((!DeleteFile(name)) && (GetLastError() != ERROR_FILE_NOT_FOUND)) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

static void registerFileToDelete(char const* name) {
    if (file_to_delete_valid == 0) {
        if ((name != NULL) && (strlen(name) + 1 <= sizeof(file_to_delete))) {
            strcpy(file_to_delete, name);
            MemoryBarrier();
            file_to_delete_valid = 1;
            MemoryBarrier();
        }
    }
    else {
        // Should never change.
        assert(strcmp(name, file_to_delete) == 0);
    }
}

static char* getErrorMsg(char* buf, size_t len) {
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, (DWORD)len, NULL);
    return buf;
}

static void cleanupSocketOnExit(void) {
    MemoryBarrier();

    if (file_to_delete_valid) {
        MemoryBarrier();
        deleteFile(file_to_delete);
    }
}

jboolean fileSocketTransport_HasValidHandle() {
    return connection_socket == INVALID_SOCKET ? JNI_FALSE : JNI_TRUE;
}

void fileSocketTransport_CloseImpl() {
    closeSocket(&server_socket);
    closeSocket(&connection_socket);
}

void logAndCleanupFailedAccept(char const* error_msg, char const* name) {
    char buf[256];
    fileSocketTransport_logError("%s: socket %s: %s", error_msg, name, getErrorMsg(buf, sizeof(buf)));
    fileSocketTransport_CloseImpl();
}

void fileSocketTransport_AcceptImpl(char const* name) {
    static int already_called = 0;

    if (!already_called) {
        registerFileToDelete(name);
        atexit(cleanupSocketOnExit);
        already_called = 1;
    }

    if (server_socket == INVALID_SOCKET) {
        socklen_t len = sizeof(struct sockaddr_un);
        struct sockaddr_un addr;
        int addr_size = sizeof(addr);

        memset((void*)&addr, 0, len);
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);

        server_socket = socket(PF_UNIX, SOCK_STREAM, 0);

        if (server_socket == INVALID_SOCKET) {
            logAndCleanupFailedAccept("Could not create doamin socket", name);
            return;
        }

        if (!deleteFile(name)) {
            logAndCleanupFailedAccept("Could not remove file to create new file socket", name);
            return;
        }

        if (bind(server_socket, (struct sockaddr*)&addr, addr_size) == -1) {
            logAndCleanupFailedAccept("Could not bind file socket", name);
            return;
        }

        if (listen(server_socket, 1) == -1) {
            logAndCleanupFailedAccept("Could not listen on file socket", name);
            return;
        }
    }

    do {
        connection_socket = accept(server_socket, NULL, NULL);
    } while ((connection_socket == INVALID_SOCKET) && (WSAGetLastError() == WSAEINTR));

    /* We can remove the file since we are connected (or it failed). */
    deleteFile(name);
    closeSocket(&server_socket);

    if (connection_socket == INVALID_SOCKET) {
        logAndCleanupFailedAccept("Could not accept on file socket", name);
        return;
    }

    ULONG peer_pid;
    DWORD size;

    if (WSAIoctl(connection_socket, SIO_AF_UNIX_GETPEERPID, NULL, 0, &peer_pid, sizeof(peer_pid), &size, NULL, NULL) != 0) {
        logAndCleanupFailedAccept("Could not determine connected processed", name);
        return;
    }

    HANDLE peer_proc = NULL;
    HANDLE self_token = NULL;
    HANDLE peer_token = NULL;
    PTOKEN_USER self_user = NULL;
    PTOKEN_USER peer_user = NULL;
    TOKEN_ELEVATION peer_elevation;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self_token)) {
        logAndCleanupFailedAccept("Could not get own token", name);
    } else if (GetTokenInformation(self_token, TokenUser, NULL, 0, &size)) {
        logAndCleanupFailedAccept("Could not get own token user size", name);
    } else if ((self_user = (PTOKEN_USER)malloc((size_t)size)) == NULL) {
        logAndCleanupFailedAccept("Could not alloc own token user size", name);
    } else if (!GetTokenInformation(self_token, TokenUser, self_user, size, &size)) {
        logAndCleanupFailedAccept("Could not get own token user", name);
    } else if ((peer_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer_pid)) == NULL) {
        logAndCleanupFailedAccept("Could not open peer process", name);
    } else if (!OpenProcessToken(peer_proc, TOKEN_QUERY | TOKEN_QUERY_SOURCE, &peer_token)) {
        logAndCleanupFailedAccept("Could not get peer token", name);
    } else if (GetTokenInformation(peer_token, TokenUser, NULL, 0, &size)) {
        logAndCleanupFailedAccept("Could not get peer token user size", name);
    } else if ((peer_user = (PTOKEN_USER)malloc((size_t)size)) == NULL) {
        logAndCleanupFailedAccept("Could not alloc peer token user size", name);
    } else if (!GetTokenInformation(peer_token, TokenUser, peer_user, size, &size)) {
        logAndCleanupFailedAccept("Could not get peer token information", name);
    } else if (!GetTokenInformation(peer_token, TokenElevation, &peer_elevation, sizeof(peer_elevation), &size)) {
        logAndCleanupFailedAccept("Could not get peer token information", name);
    } else if (!peer_elevation.TokenIsElevated && !EqualSid(self_user->User.Sid, peer_user->User.Sid)) {
        fileSocketTransport_logError("Connecting process is not the same user nor admin");
        fileSocketTransport_CloseImpl();
    }

    if (peer_token != NULL) {
        CloseHandle(peer_token);
    }

    if (self_token != NULL) {
        CloseHandle(self_token);
    }

    if (peer_proc != NULL) {
        CloseHandle(peer_proc);
    }

    free(self_user);
    free(peer_user);
}

int fileSocketTransport_ReadImpl(char* buffer, int size) {
    int result;

    do {
        result = recv(connection_socket, buffer, size, 0);
    } while ((result < 0) && (WSAGetLastError() == WSAEINTR));

    if (result < 0) {
        char buf[256];
        fileSocketTransport_logError("Read failed with result %d: %s", result, getErrorMsg(buf, sizeof(buf)));
    }

    return result;
}

int fileSocketTransport_WriteImpl(char* buffer, int size) {
    int result;

    do {
        result = send(connection_socket, buffer, size, 0);
    } while ((result < 0) && (WSAGetLastError() == WSAEINTR));

    if (result < 0) {
        char buf[256];
        fileSocketTransport_logError("Write failed with result %d: %s", result, getErrorMsg(buf, sizeof(buf)));
    }

    return result;
}

