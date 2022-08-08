/*****************************************************************************
** Author: Daniel Vik
**
** Description: Emulation of the Z80/Z80 processor
**
** More info: http://www.bluemsx.com
**
** Copyright (C) 2003-2009 Daniel Vik
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
**
** References:
**     - Z80 Family CPU Users Manual, UM008001-1000, ZiLOG 2001
**     - The Undocumented Z80 Documented version 0.6, Sean Young 2003
**     - Z80 vs Z80 timing sheet, Tobias Keizer 2004
**
******************************************************************************
*/
#ifndef Z80_H
#define Z80_H

#include "arch.h"


extern UInt8 readIoPort(UInt16 port);
extern void  writeIoPort(UInt16 port, UInt8 value);
#if 0
extern UInt8 readMemory(UInt16 address);
extern void  writeMemory(UInt16 address, UInt8 value);
#endif
extern void  patch(void);
extern void  timeout(void);

/*****************************************************
** R800_MASTER_FREQUENCY
**
** Frequency of the system clock.
******************************************************
*/
#define Z80_FREQUENCY  3579545


/*****************************************************
** SystemTime
**
** Type used for the system time
******************************************************
*/
typedef UInt32 SystemTime;


/*****************************************************
** RegisterPair
**
** Defines a register pair. Define __BIG_ENDIAN__ on
** big endian host machines.
******************************************************
*/
typedef union {
  struct { 
#ifdef __BIG_ENDIAN__
      UInt8 h;
      UInt8 l; 
#else
      UInt8 l; 
      UInt8 h; 
#endif
  } B;
  UInt16 W;
} RegisterPair;


/*****************************************************
** CpuRegs
**
** CPU registers.
******************************************************
*/
typedef struct {
    RegisterPair AF;
    RegisterPair BC;
    RegisterPair DE;
    RegisterPair HL;
    RegisterPair IX;
    RegisterPair IY;
    RegisterPair PC;
    RegisterPair SP;
    RegisterPair AF1;
    RegisterPair BC1;
    RegisterPair DE1;
    RegisterPair HL1;
    RegisterPair SH;
    UInt8 I;
    IntN  R;
    IntN  R2;

    IntN iff1;
    IntN iff2;
    IntN im;
    IntN halt;
	IntN ei_mode;
} CpuRegs;


/*****************************************************
** Status flags.
**
** May be used in the disk patch and cassette patch
******************************************************
*/
#define C_FLAG      0x01
#define N_FLAG      0x02
#define P_FLAG      0x04
#define V_FLAG      0x04
#define X_FLAG      0x08
#define H_FLAG      0x10
#define Y_FLAG      0x20
#define Z_FLAG      0x40
#define S_FLAG      0x80

/*****************************************************
** Z80
**
** Structure that defines the Z80 core.
******************************************************
*/
typedef struct
{
    SystemTime    systemTime;       /* Current system time             */
    CpuRegs       regs;             /* Active register bank            */
    UInt8         dataBus;          /* Current value on the data bus   */
    UInt8         defaultDatabus;   /* Value that is set after im2     */
    IntN          intState;         /* Sate of interrupt line          */
    IntN          nmiState;         /* Current NMI state               */

    IntN          terminate;        /* Termination flag                */
    SystemTime    timeout;          /* User scheduled timeout          */
    SystemTime    fastTimeout;
} Z80;


/************************************************************************
** z80Create
**
** The method initializes the z80 emulation.
**
*************************************************************************
*/
void z80Init(void);

/************************************************************************
** z80Reset
**
** Resets the Z80.
**
*************************************************************************
*/
void z80Reset(UInt32 cpuTime);

/************************************************************************
** z80SetInt
**
** Raises the interrupt line.
**
*************************************************************************
*/
void z80SetInt(void);

/************************************************************************
** z80ClearInt
**
** Clears the interrupt line.
**
*************************************************************************
*/
void z80ClearInt(void);

/************************************************************************
** z80SetNmi
**
** Raises the non maskable interrupt line.
**
*************************************************************************
*/
void z80SetNmi(void);

/************************************************************************
** z80ClearNmi
**
** Clears the non maskable interrupt line.
**
*************************************************************************
*/
void z80ClearNmi(void);

/************************************************************************
** z80SetDataBus
**
** Sets the data on the data bus. The default value is 0xff.
**
** Arguments:
**      value       - New value on the data bus
**      defValue    - Value that the data bus restores to after int
**      useDef      - Tells whether to modify the def value
*************************************************************************
*/
void z80SetDataBus(UInt8 value, UInt8 defValue, Int8 useDef);

/************************************************************************
** z80Execute
**
** Executes CPU instructions until the z80StopExecution function is
** called.
**
** Arguments:
**      z80        - Pointer to an Z80 object
*************************************************************************
*/
void z80Execute(void);

/************************************************************************
** z80ExecuteInstruction
**
** Executes one CPU instruction.
**
*************************************************************************
*/
void z80StopExecution(void);
void z80SetTimeout(SystemTime time);

/************************************************************************
** z80GetSystemTime
**
** Returns the current system time.
**
** Return value:
**      Current system time.
*************************************************************************
*/
SystemTime z80GetSystemTime(void);

#endif /* Z80_H */
