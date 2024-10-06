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

#include "com_sap_jdk_ext_util_Console.h"

#include <windows.h>
#include <conio.h>

static DWORD consoleMode = -1;

/*
 * Class:     com_sap_jdk_ext_util_Console
 * Method:    setMode0
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_com_sap_jdk_ext_util_Console_setMode0
  (JNIEnv *env, jclass cls, jint mode)
{
    HANDLE hConsole = GetStdHandle(STD_INPUT_HANDLE);

    if (hConsole == INVALID_HANDLE_VALUE) {
      return;
    }

    if (mode == com_sap_jdk_ext_util_Console_MODE_NON_CANONICAL) {
        if (consoleMode == -1) {
            if (!GetConsoleMode(hConsole, &consoleMode)) {
                return;
            }
        }

        SetConsoleMode(hConsole, consoleMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT));
    } else if (mode == com_sap_jdk_ext_util_Console_MODE_DEFAULT) {
        if (consoleMode == -1) {
            return;
        }

        SetConsoleMode(hConsole, consoleMode);
    }
}

/*
 * Class:     com_sap_jdk_ext_util_Console
 * Method:    readChar0
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_com_sap_jdk_ext_util_Console_readChar0
  (JNIEnv *env, jclass cls)
{
    int c = getch();
    if (c == 224) {
        c = getch();
        switch (c) {
            case 71: return com_sap_jdk_ext_util_Console_CHR_MOVE_TO_BEG;      /* Map Pos1 to Ctrl-A */
            case 79: return com_sap_jdk_ext_util_Console_CHR_MOVE_TO_END;      /* Map End  to Ctrl-E */
            case 83: return com_sap_jdk_ext_util_Console_CHR_DELETE_NEXT_CHAR; /* Maps DEL           */
            default: return c += 256;                                          /* Arrow keys         */
        }
    }
    return c;
}

/*
 * Class:     com_sap_jdk_ext_util_Console
 * Method:    getConsoleWidth0
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_com_sap_jdk_ext_util_Console_getWidth0
  (JNIEnv *env, jclass cls)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &csbi)) {
            return -1;
        }
    }

    return csbi.dwSize.X;
}
