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
#include "arch.h"
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/ioctl.h>

void setpos(Int8 x, Int8 y)
{
  printf("\033[%d;%dH", (int)y, (int)x);
}

void clearscreen()
{
  printf("\033[2J");
}

void delay(UInt32 ms)
{
  struct timeval tv = { 0,  ms * 1000 };
  select(0, NULL, NULL, NULL, &tv);
}

UInt32 gettime()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000000 + tv.tv_usec;
}

UInt8 pollkbd()
{
  static int initialized = 0;
  if (!initialized) {
    initialized = 1;
    // Use termios to turn off line buffering and echo
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO);
#ifndef FIONREAD
    term.c_cc[VMIN] = 0;
    term.c_cc[VTIME] = 0;
#endif
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
    setbuf(stdin, NULL);
  }

#ifdef FIONREAD
  int bytesWaiting;
  ioctl(STDIN_FILENO, FIONREAD, &bytesWaiting);
  if (!bytesWaiting) return 0;
  char ch = getchar();
#else
  int ch_int = getchar();
  if (ch_int == EOF) return 0;
  char ch = (char)ch_int;
#endif
  if (ch == 27) {
    ch = getchar();
    if (ch == 91) {
      ch = getchar();
      if        (ch == 65) { // up
	ch = 28;
      } else if (ch == 66) { // down
	ch = 29;
      } else if (ch == 67) { // right
	ch = 31;
      } else if (ch == 68) { // left
	ch = 30;
      }
      // TODO handle more keys (like ESC, F1 .. F5, HOME, INS, ...)
    }
  }
  return ch;
}

void display(const char *buffer)
{
  setpos(0, 0);
  printf("%s", buffer);
}

void arch_optionhelp(void)
{
}

int arch_option(int argc, char **argv)
{
  return 0;
}
