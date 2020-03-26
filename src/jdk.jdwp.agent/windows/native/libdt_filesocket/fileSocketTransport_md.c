/*
 * Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2018, 2019 SAP SE. All rights reserved.
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
