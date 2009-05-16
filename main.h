/*
 *  OpenVPN-GUI -- A Windows GUI for OpenVPN.
 *
 *  Copyright (C) 2004 Mathias Sundman <mathias@nilings.se>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdarg.h>
#include <tchar.h>

/* Define this to enable DEBUG build */
//#define DEBUG
#define DEBUG_FILE	"c:\\openvpngui_debug.txt"

/* Define this to disable Change Password support */
//#define DISABLE_CHANGE_PASSWORD

#define GUI_NAME	"OpenVPN GUI"
#define GUI_VERSION	"1.0.3"

/* Registry key for User Settings */
#define GUI_REGKEY_HKCU	"Software\\Nilings\\OpenVPN-GUI"

/* Registry key for Global Settings */
#define GUI_REGKEY_HKLM	"SOFTWARE\\OpenVPN-GUI"

#define MAX_LOG_LINES		500	/* Max number of lines in LogWindow */
#define DEL_LOG_LINES		10	/* Number of lines to delete from LogWindow */



/* bool definitions */
#define bool int
#define true 1
#define false 0

/* GCC function attributes */
#define UNUSED __attribute__ ((unused))
#define NORETURN __attribute__ ((noreturn))

#define PACKVERSION(major,minor) MAKELONG(minor,major)
struct security_attributes
{
  SECURITY_ATTRIBUTES sa;
  SECURITY_DESCRIPTOR sd;
};

/* Return the number of elements in a TCHAR array */
#define _tsizeof(x) (sizeof(x) / sizeof(*x))

/* clear an object */
#define CLEAR(x) memset(&(x), 0, sizeof(x))

/* _sntprintf with guaranteed \0 termination */
static inline int
_sntprintf_0(TCHAR *buf, TCHAR *format, ...)
{
    int i;
    va_list args;
    va_start(args, format);
    i = _vsntprintf(buf, _tsizeof(buf), format, args);
    buf[_tsizeof(buf) - 1] = _T('\0');
    va_end(args);
    return i;
}

#ifdef DEBUG
/* Print Debug Message */
#define PrintDebug(...) \
        { \
           TCHAR x_msg[256]; \
           _sntprintf_0(x_msg, __VA_ARGS__); \
           PrintDebugMsg(x_msg); \
        }

void PrintDebugMsg(TCHAR *msg);
void PrintErrorDebug(TCHAR *msg);
bool init_security_attributes_allow_all (struct security_attributes *obj);
#endif

DWORD GetDllVersion(LPCTSTR lpszDllName);

#endif
