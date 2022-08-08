/*****************************************************************************
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
******************************************************************************
*/
#ifndef ARCH_H
#define ARCH_H

#ifdef WIN32

typedef unsigned char    UInt8;
typedef   signed char    Int8;
typedef unsigned short   UInt16;
typedef   signed short   Int16;
typedef unsigned long    UInt32;
typedef   signed long    Int32;
typedef unsigned __int64 UInt64;
typedef   signed __int64 Int64;
typedef   signed int     IntN;
typedef unsigned int     UIntN;
#define FARPTR *
#define FARMALLOC(n)         malloc(n)
#define FARMEMSET(b, c, n)   memset(b, c, n)
#define FARMEMCPY(s1, s2, n) memcpy(s1, s2, n)

#elif defined(MSDOS)

typedef unsigned char      UInt8;
typedef   signed char      Int8;
typedef unsigned short     UInt16;
typedef   signed short     Int16;
typedef unsigned long      UInt32;
typedef   signed long      Int32;
typedef unsigned long long UInt64;
typedef   signed long long Int64;
typedef   signed int     IntN;
typedef unsigned int     UIntN;
#define FARPTR __far *
#define FARMALLOC(n)         _fmalloc(n)
#define FARMEMSET(b, c, n)   _fmemset(b, c, n)
#define FARMEMCPY(s1, s2, n) _fmemcpy(s1, s2, n)

#else

typedef unsigned char      UInt8;
typedef   signed char      Int8;
typedef unsigned short     UInt16;
typedef   signed short     Int16;
typedef unsigned int       UInt32;
typedef   signed int       Int32;
typedef unsigned long long UInt64;
typedef   signed long long Int64;
typedef   signed int     IntN;
typedef unsigned int     UIntN;
#define FARPTR *
#define FARMALLOC(n)         malloc(n)
#define FARMEMSET(b, c, n)   memset(b, c, n)
#define FARMEMCPY(s1, s2, n) memcpy(s1, s2, n)

#endif


// Set cursor location in console
void setpos(Int8 x, Int8 y);

// Return pressed key or 0 if no key is pressed
UInt8 pollkbd(void);

// Get system time in microseconds
UInt32 gettime(void);

// Delay ms milliseconds
void delay(UInt32 ms);

// Clears console screen
void clearscreen(void);

// Displays the specified "framebuffer"
void display(const char *buffer);

// Handle architecture specific options
void arch_optionhelp(void);
int arch_option(int argc, char **argv);

#endif
