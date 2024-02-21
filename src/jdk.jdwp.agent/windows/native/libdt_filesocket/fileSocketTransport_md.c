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

#if 0
#include <stdlib.h>

#include "jni.h"
#include "fileSocketTransport.h"

#include <windows.h>
#include <process.h>
#include <sddl.h>

static volatile HANDLE handle = INVALID_HANDLE_VALUE;
static PTOKEN_USER our_user_sid = (PTOKEN_USER) &our_user_sid;
static OVERLAPPED read_event;
static OVERLAPPED write_event;
static OVERLAPPED cancel_event;
static OVERLAPPED accept_event;
static OVERLAPPED* events[] = { &read_event, &write_event, &cancel_event, &accept_event };

static PTOKEN_USER GetUserSid() {
    if (our_user_sid == (PTOKEN_USER) &our_user_sid) {
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, _getpid());
        HANDLE token = NULL;
        our_user_sid = NULL;

        if (process != NULL && OpenProcessToken(process, TOKEN_QUERY, &token)) {
            DWORD size = 0;
            PTOKEN_USER user = NULL;
            BOOL success;

            GetTokenInformation(token, TokenUser, NULL, 0, &size);
            user = (PTOKEN_USER) malloc(size);
            success = GetTokenInformation(token, TokenUser, user, size, &size);

            CloseHandle(token);
            CloseHandle(process);

            if (success &&  IsValidSid(user->User.Sid)) {
                our_user_sid = user;
            } else {
                free(user);
            }
        } else if (process != NULL) {
            CloseHandle(process);
        }
    }

    return our_user_sid;
}

static void destroyEvents(OVERLAPPED** events, int nr_of_events) {
    int i;

    for (i = 0; i < nr_of_events; ++i) {
        if (events[i]->hEvent != NULL) {
            CloseHandle(events[i]->hEvent);
        }

        ZeroMemory(events[i], sizeof(OVERLAPPED));
    }
}

static jboolean initEvents(OVERLAPPED** events, int nr_of_events) {
    int i;

    destroyEvents(events, nr_of_events);

    for (i = 0; i < nr_of_events; ++i) {
        ZeroMemory(events[i], sizeof(OVERLAPPED));
        events[i]->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        if (events[i]->hEvent == NULL) {
            destroyEvents(events, i - 1);
            return JNI_FALSE;
        }
    }

    return JNI_TRUE;
}

jboolean fileSocketTransport_HasValidHandle() {
    return handle == INVALID_HANDLE_VALUE ? JNI_FALSE : JNI_TRUE;
}

void fileSocketTransport_CloseImpl() {
    HANDLE tmp = handle;
    SetEvent(cancel_event.hEvent);

    if (tmp != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(tmp);
        CloseHandle(tmp);
        handle = INVALID_HANDLE_VALUE;
    }
}

void fileSocketTransport_AcceptImpl(char const* name) {
    // Allow access to admins and user, but not others.
    LPSECURITY_ATTRIBUTES sec = NULL;
    char sec_spec[1024];
    static char const* user_id = NULL;
    jboolean connected = JNI_FALSE;
    static jboolean event_initialized = JNI_FALSE;

    if (!event_initialized && !initEvents(events, sizeof(events) / sizeof(events[0]))) {
        fileSocketTransport_logError("Could not initialize events.");
        return;
    }

    event_initialized = JNI_TRUE;

    if (user_id == NULL) {
        PTOKEN_USER sid = GetUserSid();
        LPTSTR result;
        if (!sid) {
            fileSocketTransport_logError("Could not get the user sid");
            return;
        } else if (!ConvertSidToStringSid(sid->User.Sid, &result)) {
            fileSocketTransport_logError("Could not convert the user sid to a string");
            return;
        } else {
            user_id = (char const*) result;
        }
    }

    snprintf(sec_spec, sizeof(sec_spec),
        "D:"
        "(A;OICI;FA;;;%s)"
        "(A;OICI;FA;;;BA)", user_id);
    sec = (LPSECURITY_ATTRIBUTES) malloc(sizeof(SECURITY_ATTRIBUTES));
    sec->nLength = sizeof(SECURITY_ATTRIBUTES);
    sec->bInheritHandle = FALSE;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptor(sec_spec, SDDL_REVISION_1, &(sec->lpSecurityDescriptor), NULL)) {
        fileSocketTransport_logError("Could not convert security descriptor %s", sec_spec);
        free(sec);
        return;
    }

    handle = CreateNamedPipe(name, FILE_FLAG_OVERLAPPED | PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, NMPWAIT_USE_DEFAULT_WAIT, sec);
    free(sec);

    if (handle == INVALID_HANDLE_VALUE) {
        fileSocketTransport_logError("Could not create pipe, error code %lld", (long long) GetLastError());
        return;
    }

    ResetEvent(accept_event.hEvent);
    ResetEvent(cancel_event.hEvent);

    while (!connected) {
        DWORD lastError;

        connected = ConnectNamedPipe(handle, &accept_event);
        lastError = GetLastError();

        if (!connected && (lastError == ERROR_PIPE_CONNECTED)) {
            connected = TRUE;
        }

        if (!connected) {
            if (lastError == ERROR_IO_PENDING) {
                // The ConnectNamedPipe is still in progress. wait until signaled...
                // we wait for two different signals, not both signals must be 1, wait until signal is seen
                HANDLE hOverlapped[2] = { accept_event.hEvent, cancel_event.hEvent };
                DWORD waitResult = WaitForMultipleObjects(2, hOverlapped, FALSE, INFINITE);

                if (waitResult == WAIT_OBJECT_0) {
                    // client has connected
                    if (HasOverlappedIoCompleted(&accept_event)) {
                        connected = TRUE;
                    } else {
                        CancelIo(handle);
                    }
                } else {
                    fileSocketTransport_logError("Accept error (wait): %d", waitResult);
                    fileSocketTransport_CloseImpl();
                    return;
                }
            } else {
                // we definitely cannot get a new client connection (for example pipe was closed)
                fileSocketTransport_logError("Accept error (connect) : %d.", (int) lastError);
                fileSocketTransport_CloseImpl();
                return;
            }
        }
    }
}

int fileSocketTransport_logIOError(char const* type, DWORD errorCode) {
    if (errorCode == ERROR_BROKEN_PIPE) {
        fileSocketTransport_logError("%s: Connection closed", type);
        return 0;
    }

    fileSocketTransport_logError("%s: %d", type, (int) errorCode);
    return -1;
}

int fileSocketTransport_ReadImpl(char* buffer, int size) {
    DWORD nread = 0;

    ResetEvent(read_event.hEvent);

    if (!ReadFile(handle, (LPVOID) buffer, (DWORD) size, &nread, &read_event)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            HANDLE hOverlapped[2] = { read_event.hEvent, cancel_event.hEvent };
            DWORD waitResult = WaitForMultipleObjects(2, hOverlapped, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(handle, &read_event, &nread, FALSE)) {
                    return fileSocketTransport_logIOError("Read failed (overlap)", GetLastError());
                }
            } else {
                CancelIo(handle);
                fileSocketTransport_logError("Read failed (wait): %d", (int) waitResult);
                return -1;
            }
        } else {
            return fileSocketTransport_logIOError("Read failed", GetLastError());
        }
    }

    return (int) nread;
}

int fileSocketTransport_WriteImpl(char* buffer, int size) {
    DWORD nwrite = 0;

    ResetEvent(write_event.hEvent);

    if (!WriteFile(handle, buffer, (DWORD) size, &nwrite, &write_event)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            HANDLE hOverlapped[2] = { write_event.hEvent, cancel_event.hEvent };
            DWORD waitResult = WaitForMultipleObjects(2, hOverlapped, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(handle, &write_event, &nwrite, FALSE)) {
                    return fileSocketTransport_logIOError("Write failed (overlap)", GetLastError());
                }
            } else {
                CancelIo(handle);
                fileSocketTransport_logError("Write failed (wait): %d", (int) waitResult);
                return -1;
            }
        } else {
            return fileSocketTransport_logIOError("Write failed", GetLastError());
        }
    }

    return (int) nwrite;
}

static char default_name[160] = { 0, };

char* fileSocketTransport_GetDefaultAddress() {
    if (default_name[0] == '\0') {
        PTOKEN_USER user = GetUserSid();
        char* user_name = NULL;
        if (ConvertSidToStringSidA(user->User.Sid, &user_name)) {
            snprintf(default_name, sizeof(default_name), "\\\\.\\Pipe\\dt_filesocket_%lld_%s", (long long) _getpid(),
                user_name ? user_name : "unknown_user");
            LocalFree(user_name);
            default_name[sizeof(default_name) - 1] = '\0';
        } else {
            fileSocketTransport_logError("Could not convert sid: %d", GetLastError());
            return NULL;
        }
    }

    return default_name;
}
#else
#include <stdlib.h>

#include "jni.h"
#include "jvm.h"
#include "fileSocketTransport.h"

#include <windows.h>
#include <winsock2.h>
#include <afunix.h>
#include <assert.h>


#ifdef _WIN32
#define SOCKET_HANDLE SOCKET
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#define CLOSE_SOCKET_FUNC closesocket
#define CALL_INTERRUPTED (WSAGetLastError() == WSAEINTR)
#else
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
    if (!DeleteFile(name)) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? JNI_TRUE : JNI_FALSE;
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

#endif
