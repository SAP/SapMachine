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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "jni.h"
#include "jdwpTransport.h"
#include "fileSocketTransport.h"

#ifdef _WIN32
#include <winsock2.h>
#include <afunix.h>
#define MAX_FILE_SOCKET_PATH_LEN UNIX_PATH_MAX
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#define MAX_FILE_SOCKET_PATH_LEN sizeof(((struct sockaddr_un *) 0)->sun_path)
#endif

#define MAX_DATA_SIZE 1000
#define HANDSHAKE "JDWP-Handshake"

/* Since the jdwp agent sometimes kills the VM outright when
 * the connection fails, we always fake a successful
 * connection and instead fail in the read/write packet methods,
 * which does not cause the VM to exit.
 */
static jboolean fake_open = JNI_FALSE;
static jboolean initialized = JNI_FALSE;
static JavaVM *jvm;
static char path[MAX_FILE_SOCKET_PATH_LEN + 1];
static jdwpTransportCallback *callback;
static char last_error[2048];
static struct jdwpTransportNativeInterface_ nif;
static jdwpTransportEnv single_env = (jdwpTransportEnv) &nif;

void fileSocketTransport_logError(char const* format, ...) {
    char* tmp = (*callback->alloc)(sizeof(last_error));
    va_list ap;

    if (tmp != NULL) {
        va_start(ap, format);
        vsnprintf(tmp, sizeof(last_error) - 1, format, ap);
        tmp[sizeof(last_error) - 1] = '\0';
        va_end(ap);

        printf("Error: %s\n", tmp);

        memcpy(last_error, tmp, sizeof(last_error));
        (*callback->free)(tmp);
    } else {
        printf("Could not get memory to print error.\n");
    }
}

static jdwpTransportError JNICALL fileSocketTransport_GetCapabilities(jdwpTransportEnv* env, JDWPTransportCapabilities *capabilities_ptr) {
    JDWPTransportCapabilities result;

    memset(&result, 0, sizeof(result));
    result.can_timeout_attach = JNI_FALSE;
    result.can_timeout_accept = JNI_FALSE;
    result.can_timeout_handshake = JNI_FALSE;

    *capabilities_ptr = result;

    return JDWPTRANSPORT_ERROR_NONE;
}

static jdwpTransportError JNICALL fileSocketTransport_SetTransportConfiguration(jdwpTransportEnv* env, jdwpTransportConfiguration *config) {
    return JDWPTRANSPORT_ERROR_NONE;
}

static jdwpTransportError JNICALL fileSocketTransport_Close(jdwpTransportEnv* env) {
    if (fileSocketTransport_HasValidHandle()) {
        fileSocketTransport_CloseImpl();
    }

    fake_open = JNI_FALSE;
    return JDWPTRANSPORT_ERROR_NONE;
}

static jdwpTransportError JNICALL fileSocketTransport_Attach(jdwpTransportEnv* env, const char* address, jlong attach_timeout, jlong handshake_timeout) {
    /* We don't support attach. */
    fileSocketTransport_logError("Only server=y mode is supported by dt_filesocket");
    return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
}

static jdwpTransportError JNICALL fileSocketTransport_StartListening(jdwpTransportEnv* env, const char* address, char** actual_address) {
    /* Only make sure we have no open connection. */
    fileSocketTransport_Close(env);

    if (address == NULL) {
        fileSocketTransport_logError("Default address not supported");
        return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
    }

    *actual_address = (char*) address;

    if (strlen(address) <= MAX_FILE_SOCKET_PATH_LEN) {
        strcpy(path, address);
    } else {
        fileSocketTransport_logError("Address too long: %s", address);
        fake_open = JNI_TRUE;
    }

    return JDWPTRANSPORT_ERROR_NONE;
}

static jdwpTransportError JNICALL fileSocketTransport_StopListening(jdwpTransportEnv* env) {
    return JDWPTRANSPORT_ERROR_NONE;
}

static jboolean JNICALL fileSocketTransport_IsOpen(jdwpTransportEnv* env) {
    return fake_open || fileSocketTransport_HasValidHandle();
}

static int fileSocketTransport_ReadFully(char* buf, int len) {
    int read = 0;

    while (len > 0) {
        int n = fileSocketTransport_ReadImpl(buf, len);

        if (n < 0) {
            return n;
        } else if (n == 0) {
            break;
        }

        buf += n;
        len -= n;
        read += n;
    }

    return read;
}

static int fileSocketTransport_WriteFully(char* buf, int len) {
    int written = 0;

    while (len > 0) {
        int n = fileSocketTransport_WriteImpl(buf, len);

        if (n < 0) {
            return n;
        } else if (n == 0) {
            break;
        }

        buf += n;
        len -= n;
        written += n;
    }

    return written;
}

static jdwpTransportError JNICALL fileSocketTransport_Accept(jdwpTransportEnv* env, jlong accept_timeout, jlong handshake_timeout) {
    fileSocketTransport_AcceptImpl(path);

    if (!fileSocketTransport_HasValidHandle()) {
        fake_open = JNI_TRUE;
    } else {
        char buf[sizeof(HANDSHAKE)];
        fileSocketTransport_ReadFully(buf, (int) strlen(HANDSHAKE));
        fileSocketTransport_WriteFully(HANDSHAKE, (int) strlen(HANDSHAKE));

        if (strcmp(buf, HANDSHAKE) != 0) {
            fake_open = JNI_TRUE;
        }
    }

    return JDWPTRANSPORT_ERROR_NONE;
}

static jdwpTransportError JNICALL fileSocketTransport_ReadPacket(jdwpTransportEnv* env, jdwpPacket *packet) {
    jint length, data_len, n;

    if (!fileSocketTransport_HasValidHandle()) {
        fake_open = JNI_FALSE;
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    } else if (fake_open) {
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    }

    if (packet == NULL) {
        fileSocketTransport_logError("Packet is null while reading");
        return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
    }

    /* Taken mostly from socketTransport.c */
    n = fileSocketTransport_ReadFully((char *) &length, sizeof(jint));

    if (n == 0) {
        packet->type.cmd.len = 0;
        return JDWPTRANSPORT_ERROR_NONE;
    }

    if (n != sizeof(jint)) {
        fileSocketTransport_logError("Only read %d instead of %d bytes for length field", (int) n, (int) sizeof(jint));
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    }

    length = (jint) ntohl(length);
    packet->type.cmd.len = length;
    n = fileSocketTransport_ReadFully((char *)&(packet->type.cmd.id), sizeof(jint));

    if (n < (int)sizeof(jint)) {
        fileSocketTransport_logError("Only read %d instead of %d bytes for command id", (int) n, (int) sizeof(jint));
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    }

    packet->type.cmd.id = (jint) ntohl(packet->type.cmd.id);
    n = fileSocketTransport_ReadFully((char *)&(packet->type.cmd.flags), sizeof(jbyte));

    if (n < (int) sizeof(jbyte)) {
        fileSocketTransport_logError("Only read %d bytes for flags", (int) n);
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    }

    if (packet->type.cmd.flags & JDWPTRANSPORT_FLAGS_REPLY) {
        n = fileSocketTransport_ReadFully((char *)&(packet->type.reply.errorCode), sizeof(jbyte));
        if (n < (int)sizeof(jshort)) {
            fileSocketTransport_logError("Only read %d bytes for error code", (int) n);
            return JDWPTRANSPORT_ERROR_IO_ERROR;
        }
    } else {
        n = fileSocketTransport_ReadFully((char *)&(packet->type.cmd.cmdSet), sizeof(jbyte));
        if (n < (int)sizeof(jbyte)) {
            fileSocketTransport_logError("Only read %d bytes for command set", (int) n);
            return JDWPTRANSPORT_ERROR_IO_ERROR;
        }

        n = fileSocketTransport_ReadFully((char *)&(packet->type.cmd.cmd), sizeof(jbyte));
        if (n < (int)sizeof(jbyte)) {
            fileSocketTransport_logError("Only read %d bytes for command", (int) n);
            return JDWPTRANSPORT_ERROR_IO_ERROR;
        }
    }

    data_len = length - ((sizeof(jint) * 2) + (sizeof(jbyte) * 3));

    if (data_len < 0) {
        fileSocketTransport_logError("Inavlid data length %d of read packet", (int) data_len);
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    } else if (data_len == 0) {
        packet->type.cmd.data = NULL;
    } else {
        packet->type.cmd.data = (*callback->alloc)(data_len);

        if (packet->type.cmd.data == NULL) {
            return JDWPTRANSPORT_ERROR_OUT_OF_MEMORY;
        }

        n = fileSocketTransport_ReadFully((char *) packet->type.cmd.data, data_len);

        if (n < data_len) {
            fileSocketTransport_logError("Only read %d bytes for JDWP payload but expected %d", (int) n, (int) data_len);
            return JDWPTRANSPORT_ERROR_IO_ERROR;
        }
    }

    return JDWPTRANSPORT_ERROR_NONE;
}

static jdwpTransportError JNICALL fileSocketTransport_WritePacket(jdwpTransportEnv* env, const jdwpPacket* packet) {
    jint len, data_len, id, n;
    char header[JDWP_HEADER_SIZE + MAX_DATA_SIZE];
    jbyte *data;

    if (!fileSocketTransport_HasValidHandle()) {
        fake_open = JNI_FALSE;
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    } else if (fake_open) {
        return JDWPTRANSPORT_ERROR_IO_ERROR;
    }

    /* Taken mostly from sockectTransport.c */
    if (packet == NULL) {
        fileSocketTransport_logError("Packet is null when writing");
        return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
    }

    len = packet->type.cmd.len;
    data_len = len - JDWP_HEADER_SIZE;

    if (data_len < 0) {
        fileSocketTransport_logError("Packet to write has illegal data length %d", (int) data_len);
        return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
    }

    /* prepare the header for transmission */
    len = (jint) htonl(len);
    id = (jint) htonl(packet->type.cmd.id);

    memcpy(header + 0, &len, 4);
    memcpy(header + 4, &id, 4);
    header[8] = packet->type.cmd.flags;

    if (packet->type.cmd.flags & JDWPTRANSPORT_FLAGS_REPLY) {
        jshort errorCode = htons(packet->type.reply.errorCode);
        memcpy(header + 9, &errorCode, 2);
    } else {
        header[9] = packet->type.cmd.cmdSet;
        header[10] = packet->type.cmd.cmd;
    }

    data = packet->type.cmd.data;

    /* Do one send for short packets, two for longer ones */
    if (data_len <= MAX_DATA_SIZE) {
        memcpy(header + JDWP_HEADER_SIZE, data, data_len);
        if ((n = fileSocketTransport_WriteFully((char *) &header, JDWP_HEADER_SIZE + data_len)) !=
            JDWP_HEADER_SIZE + data_len) {
            fileSocketTransport_logError("Could only write %d bytes instead of %d of the packet", (int) n, (int) (JDWP_HEADER_SIZE + data_len));
            return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
        }
    } else {
        memcpy(header + JDWP_HEADER_SIZE, data, MAX_DATA_SIZE);
        if ((n = fileSocketTransport_WriteFully((char *) &header, JDWP_HEADER_SIZE + MAX_DATA_SIZE)) !=
            JDWP_HEADER_SIZE + MAX_DATA_SIZE) {
            fileSocketTransport_logError("Could only write %d bytes instead of %d of the packet", (int) n, (int) (JDWP_HEADER_SIZE + MAX_DATA_SIZE));
            return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
        }
        /* Send the remaining data bytes right out of the data area. */
        if ((n = fileSocketTransport_WriteFully((char *) data + MAX_DATA_SIZE,
            data_len - MAX_DATA_SIZE)) != data_len - MAX_DATA_SIZE) {
            fileSocketTransport_logError("Could only write %d bytes instead of %d of the packet", (int) n, (int) (data_len - MAX_DATA_SIZE));
            return JDWPTRANSPORT_ERROR_ILLEGAL_ARGUMENT;
        }
    }

    return JDWPTRANSPORT_ERROR_NONE;
}

static jdwpTransportError JNICALL fileSocketTransport_GetLastError(jdwpTransportEnv* env, char** error) {
    *error = (*callback->alloc)(sizeof(last_error));
    memcpy(*error, last_error, sizeof(last_error));
    (*error)[sizeof(last_error) - 1] = '\0';
    return JDWPTRANSPORT_ERROR_NONE;
}

JNIEXPORT jint JNICALL
jdwpTransport_OnLoad(JavaVM *vm, jdwpTransportCallback* callbacks, jint version, jdwpTransportEnv** env)
{
    if (version < JDWPTRANSPORT_VERSION_1_0 ||version > JDWPTRANSPORT_VERSION_1_1) {
        return JNI_EVERSION;
    }

    if (initialized) {
        return JNI_EEXIST;
    }

    initialized = JNI_TRUE;
    jvm = vm;
    callback = callbacks;

    /* initialize interface table */
    nif.GetCapabilities = fileSocketTransport_GetCapabilities;
    nif.Attach = fileSocketTransport_Attach;
    nif.StartListening = fileSocketTransport_StartListening;
    nif.StopListening = fileSocketTransport_StopListening;
    nif.Accept = fileSocketTransport_Accept;
    nif.IsOpen = fileSocketTransport_IsOpen;
    nif.Close = fileSocketTransport_Close;
    nif.ReadPacket = fileSocketTransport_ReadPacket;
    nif.WritePacket = fileSocketTransport_WritePacket;
    nif.GetLastError = fileSocketTransport_GetLastError;
    if (version >= JDWPTRANSPORT_VERSION_1_1) {
        nif.SetTransportConfiguration = fileSocketTransport_SetTransportConfiguration;
    }
    *env = (jdwpTransportEnv*) &single_env;

    return JNI_OK;
}
