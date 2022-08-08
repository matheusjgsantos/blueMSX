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
#include <windows.h> 
#include <conio.h>
#include <stdio.h>

#include "arch.h"

void setpos(Int8 x, Int8 y)
{
  COORD coord = { (short)x, (short)y};
  SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
}

void clearscreen(void)
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 0);
	system("cls");
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 15 + (9 * 16));
}

void delay(UInt32 ms)
{
    Sleep(ms);
}

UInt8 pollkbd(void)
{
    if (_kbhit()) {
        char ch = _getch();
        if (ch != 0 && ch != -32) {
            return ch;
        }
        // Handling of special characters. Map Cursor keys to 'ascii codes' 28-31
        ch = _getch();
        return ch == 'H' ? 28 : ch == 'P' ? 29 : ch == 'K' ? 30 : ch == 'M' ? 31 : 0;
    }
    return 0;
}

UInt32 gettime(void)
{
    static LONGLONG hfFrequency = 0;
    LARGE_INTEGER li;

    if (!hfFrequency) {
        if (QueryPerformanceFrequency(&li)) {
            hfFrequency = li.QuadPart;
        }
        else {
            return 0;
        }
    }

    QueryPerformanceCounter(&li);

    return (DWORD)(li.QuadPart * 1000000 / hfFrequency);
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
