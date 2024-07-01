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

#include <sys/ioctl.h>
#include <sys/resource.h>
#include <termios.h>
#include <unistd.h>

struct termios consoleMode, *consoleModePtr = NULL;

/*
 * Class:     com_sap_jdk_ext_util_Console
 * Method:    setMode0
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_com_sap_jdk_ext_util_Console_setMode0
  (JNIEnv *env, jclass cls, jint mode)
{
    if (!isatty(STDIN_FILENO)) {
        return;
    }

    if (mode == com_sap_jdk_ext_util_Console_MODE_NON_CANONICAL) {
        if (consoleModePtr == NULL) {
            if (tcgetattr(STDIN_FILENO, &consoleMode) < 0) {
                return;
            }
            consoleModePtr = &consoleMode;
        }

        struct termios newTermState = consoleMode;
        newTermState.c_lflag &= ~(ECHO | ICANON);
        newTermState.c_cc [VMIN] = 1;
        newTermState.c_cc [VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &newTermState);
    } else if (mode == com_sap_jdk_ext_util_Console_MODE_DEFAULT) {
        if (consoleModePtr == NULL) {
            return;
        }

        tcsetattr(STDIN_FILENO, TCSANOW, consoleModePtr);
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
    // in Unix terminals, arrow keys are represented by a sequence of 3 characters. E.g., the up arrow key yields 27, 91, 68
    int c = getc(stdin);
    if (c == 27) {
        c = getc(stdin);
        if (c == 91) {
            c = getc(stdin);
            switch (c) {
                case 65: return com_sap_jdk_ext_util_Console_CHR_PREV_HISTORY;     /* Map arrow keys     */
                case 66: return com_sap_jdk_ext_util_Console_CHR_NEXT_HISTORY;
                case 67: return com_sap_jdk_ext_util_Console_CHR_NEXT_CHAR;
                case 68: return com_sap_jdk_ext_util_Console_CHR_PREV_CHAR;
                case 51: c = getc(stdin);                                          /* Map DEL key        */
                         if (c == 126) {
                             return com_sap_jdk_ext_util_Console_CHR_DELETE_NEXT_CHAR;
                         }
                         break;
                case 72: return com_sap_jdk_ext_util_Console_CHR_MOVE_TO_BEG;      /* Map Pos1 to Ctrl-A */
                case 70: return com_sap_jdk_ext_util_Console_CHR_MOVE_TO_END;      /* Map End to Ctrl-E  */
            }
        }
    } else if (c == 127) {
        c = com_sap_jdk_ext_util_Console_CHR_DELETE_PREV_CHAR;                     /* Map Backspace      */
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
#if defined(TIOCGWINSZ)
    struct winsize w;

    if (ioctl(0, TIOCGWINSZ, &w) == 0 || ioctl(1, TIOCGWINSZ, &w) == 0 || ioctl(2, TIOCGWINSZ, &w) == 0) {
        return w.ws_col;
    }
#endif
    return -1;
}
