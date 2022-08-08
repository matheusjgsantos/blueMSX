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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "z80.h"
#include "arch.h"


static struct {
    UInt8 status;
    UInt8 latch;
    UInt16 address;
    UInt8 data;
    UInt8 regs[8];
    Int8 key;
} vdp;

static struct {
    UInt8 regs[4];
} ppi;

static UInt8  FARPTR memory[4][4], FARPTR empty;
UInt8  FARPTR ram[4];
UInt8         slot[4];
static UInt8  vram[0x4000];
static UInt32 z80Timeout;
static UInt64 z80Frequency = 3579545;
static UInt16 frameCounter;
static UInt32 syncTime;
static UInt32 emuTime;
static UInt16 keyPressed = 0xffff;
static Int8   verbose = 0;
static Int8   normalSpeed = 0;
static Int8   vramDirtyFlag = 1;

static const UInt16 KeyMatrix[256] =
{
    0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xff75,0xffff,0xff77,0xffff,0xffff,0xff77,0xffff,0xffff,
    0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xff85,0xff86,0xff84,0xff87,

    0xff80,0x6001,0x6020,0x6003,0x6004,0x6005,0x6007,0xff20,0x6011,0x6000,0x6010,0x6013,0xff22,0xff12,0xff23,0xff24,
    0xff00,0xff01,0xff02,0xff03,0xff04,0xff05,0xff06,0xff07,0xff10,0xff11,0x6017,0xff17,0x6022,0xff13,0x6023,0x6024,

    0x6002,0x6026,0x6027,0x6030,0x6031,0x6032,0x6033,0x6034,0x6035,0x6036,0x6037,0x6040,0x6041,0x6042,0x6043,0x6044,
    0x6045,0x6046,0x6047,0x6050,0x6051,0x6052,0x6053,0x6054,0x6055,0x6056,0x6057,0xff16,0xff14,0xff21,0x6006,0x6012,

    0xff72,0xff26,0xff27,0xff30,0xff31,0xff32,0xff33,0xff34,0xff35,0xff36,0xff37,0xff40,0xff41,0xff42,0xff43,0xff44,
    0xff45,0xff46,0xff47,0xff50,0xff51,0xff52,0xff53,0xff54,0xff55,0xff56,0xff57,0x6016,0x6014,0x6021,0x6072,0xff75
};


UInt8 readIoPort(UInt16 port)
{
    switch (port & 0xff) {
    case 0xa8:
        return ppi.regs[3] & 0x10 ? 0xff : ppi.regs[0];
    case 0xa9:
        if (ppi.regs[3] & 0x02) {
            UInt8 row = (ppi.regs[3] & 0x01 ? 0x00 : ppi.regs[2]) & 0x0f;
            UInt8 val = 0;
            if (((keyPressed >>  4) & 0x0f) == row) val |= 1 << ((keyPressed >> 0) & 0x0f);
            if (((keyPressed >> 12) & 0x0f) == row) val |= 1 << ((keyPressed >> 8) & 0x0f);
            return ~val;
        }
        return ppi.regs[1];
    case 0xaa:
        return ((ppi.regs[3] & 0x01 ? 0xff : ppi.regs[2]) & 0x0f) |
               ((ppi.regs[3] & 0x08 ? 0xff : ppi.regs[2]) & 0xf0);
    case 0xab:
        return ppi.regs[3];
    case 0x98:
        {
    	    UInt8 value = vdp.data;
            vdp.data = vram[vdp.address++ & 0x3fff];
            vdp.key = 0;
            return value;
        }
    case 0x99:
        {
            UInt8 status = vdp.status;
            vdp.status &= 0x1f;
            z80ClearInt();
            return status;
        }
    }
    return 0xff;
}

static void updadeSlots(void)
{
    Int8 i;
    UInt8 slotMask = (ppi.regs[3] & 0x10) ? 0 : ppi.regs[0];
    for (i = 0; i < 4; i++) {
        ram[i] = memory[(slotMask & 3)][i];
        slotMask >>= 2;
        slot[i] = (ppi.regs[0] >> (i << 1)) & 3;

    }
}

void  writeIoPort(UInt16 port, UInt8 value)
{
    switch (port & 0xff) {
    case 0xa8:
        ppi.regs[0] = value;
        updadeSlots();
        break;
    case 0xa9:
    case 0xaa:
        ppi.regs[port & 0x03] = value;
        break;
    case 0xab:
        if (value & 0x80) {
            ppi.regs[3] = value;
            updadeSlots();
        }
        else {
            UInt8 mask = 1 << ((value >> 1) & 0x07);
            if (value & 1) {
                ppi.regs[2] |= mask;
            }
            else {
                ppi.regs[2] &= ~mask;
            }
        }
        break;
    case 0x98:
        vramDirtyFlag = 1;
        vram[vdp.address++ & 0x3fff] = value;
        vdp.key = 0;
        vdp.data = value;
        break;
    case 0x99:
        if (vdp.key) {
		    vdp.key = 0;
        	vdp.address = (UInt16)value << 8 | vdp.latch;
		    if ((value & 0xc0) == 0x80) {
                vdp.regs[value & 0x07] = vdp.latch;
                vramDirtyFlag = 1;
		    }
		    if ((value & 0xc0) == 0x00) {
				readIoPort(0x98);
		    }
	    }
        else {
		    vdp.key = 1;
		    vdp.latch = value;
	    }
        break;
    }
}

#if 0
UInt8 readMemory(UInt16 address)
{
    return ram[address >> 14][address & 0x3fff];
}

void  writeMemory(UInt16 address, UInt8 value)
{
    IntN page = address >> 14;
    if (slot[page] == 3) {
        ram[page][address & 0x3fff] = value;
    }
}
#endif

void  patch(void)
{
}

static void printScreen(void)
{
    static char buffers[2][24*41+1];
    static Int8 viewBuf = 0;

    Int8 width   = (vdp.regs[1] & 0x10) ? 40 : 32;
    UInt8* base = vram + ((vdp.regs[2] & 0x0f) << 10);
    char* buf = buffers[viewBuf ^ 1];
    Int8 x, y;

    for (y = 0; y < 24; y++) {
        for (x = 0; x < width; x++) {
            UInt8 val = *base++;
            buf[x] = val >= 32 && val < 126 ? val : val == 0xff ? '_' : ' ';
        }
        buf += width;
        if (width == 32) {
            buf[0] = buf[1] = buf[2] = buf[3] = 32;
            buf[4] = buf[5] = buf[6] = buf[7] = 32;
            buf += 8;
        }
        *buf++ = '\n';
    }
    *buf = 0;

    if (memcmp(buffers[0], buffers[1], sizeof(buffers[0])) != 0) { 
        viewBuf ^= 1;
        display(buffers[viewBuf]);
    }
}


void  timeout(void)
{
    vdp.status |= 0x80;
    if (vdp.regs[1] & 0x20) {
        z80SetInt();
    }

    keyPressed = KeyMatrix[pollkbd()];

    if (vramDirtyFlag && (frameCounter & 3) == 0) {
        vramDirtyFlag = 0;
        printScreen();
    }

    if (normalSpeed) {
        Int32 diffTime = (Int32)(syncTime - gettime());
        if (diffTime > 0) {
            delay(diffTime / 1000);
        }
        syncTime += 20000;
    }

    if (++frameCounter == 50) {
        UInt32 diffTime = gettime() - emuTime;

        frameCounter = 0;

        if (!normalSpeed) {
            z80Frequency = z80Frequency * 1000000 / diffTime;
        }
        emuTime += diffTime;

        if (verbose) {
            setpos(0, 24);
            printf("=== FPS: %d    CPU: %d MHz ===    \r",
                   50500000 / diffTime, (UInt32)(z80Frequency / 1000000));
        }
    }

    z80Timeout += (UInt32)(z80Frequency / 50);
    z80SetTimeout(z80Timeout);
}

static void loadRom(const char* romFile, int slot, int page)
{
    size_t n;
    FILE* f = fopen(romFile, "rb");
    if (f != NULL) {
        while (page < 4 && !ferror(f) && !feof(f)) {
            if ((n = fread(vram, 1, 0x4000, f)) > 0) {
                if (memory[slot][page] == empty) {
                    memory[slot][page] = FARMALLOC(0x4000);
                    if (!memory[slot][page]) {
                        printf("loadRom: out of memory\n");
                        exit(-1);
                    }
                }
                FARMEMCPY(memory[slot][page++], vram, n);
            }
        }
        fclose(f);
    }
}

static int init_memory(void)
{
    int slot, page;

    empty = FARMALLOC(0x4000);
    if (!empty) {
        return 0;
    }
    FARMEMSET(empty, 0xff, 0x4000);
    for (slot = 0; slot < 4; slot++) {
        for (page = 0; page < 4; page++) {
            if (slot < 3) {
                memory[slot][page] = empty;
            } else {
                memory[slot][page] = FARMALLOC(0x4000);
                if (!memory[slot][page]) {
                    return 0;
                }
                FARMEMSET(memory[slot][page], 0x00, 0x4000);
            }
        }
    }
    return 1;
}

int main(int argc, char** argv)
{
    int page;
    if (!init_memory()) {
        printf("out of memory\n");
        return -1;
    }
    for (argc--, argv++; argc > 0; argc--, argv++) {
        if (0 == strcmp(argv[0], "-s") && argc > 1) {
            loadRom(argv[1], 0, 0);
            argc--, argv++;
        } else if (0 == strcmp(argv[0], "-r") && argc > 1) {
            loadRom(argv[1], 1, 1);
            argc--, argv++;
        } else if (0 == strcmp(argv[0], "-R") && argc > 1) {
            loadRom(argv[1], 1, 0);
            argc--, argv++;
        } else if (0 == strcmp(argv[0], "-b") && argc > 1) {
            loadRom(argv[1], 1, 2);
            argc--, argv++;
        } else if (0 == strcmp(argv[0], "-v")) {
            verbose = 1;
        } else if (0 == strcmp(argv[0], "-n")) {
            normalSpeed = 1;
        } else if (0 == strcmp(argv[0], "-h")) {
            printf("bluemsxnano v0.9 is a compact MSX1 emulator for execution in a console\n\n");
            printf("Usage:\n\n");
            printf("    bluemsxnano [-s <bios>] [-r <rom>] [-R <rom>] [-b <rom>] [-v] [-n] [-h]\n\n");
            printf("    -s <bios>      Loads a bios rom into address 0x0000\n");
            printf("    -R <rom>       Loads a cartridge rom into address 0x0000\n");
            printf("    -r <rom>       Loads a cartridge rom into address 0x4000\n");
            printf("    -b <rom>       Loads a cartridge rom into address 0x8000\n");
            printf("    -v             Verbose, shows FPS and Z80 frequency\n");
            printf("    -n             Run emulation in normal speed (3.57MHz)\n");
            arch_optionhelp();
            printf("    -h             Shows help\n");
            return 0;
        } else {
            page = arch_option(argc, argv);
            argc -= page, argv += page;
        }
    }

    for (page = 0; page < 4; page++) {
        ram[page] = memory[0][page];
    }

    z80Init();

    clearscreen();

    z80Timeout = (UInt32)(z80Frequency / 50);
    emuTime = gettime();
    syncTime = emuTime + 20000;
    z80SetTimeout(z80Timeout);

    z80Execute();

    return 0;
}
