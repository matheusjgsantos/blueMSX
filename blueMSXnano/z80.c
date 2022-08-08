/*****************************************************************************
** $Source: /cvsroot/bluemsx/blueMSX/Src/Z80/Z80.c,v $
**
** $Revision: 1.31 $
**
** $Date: 2007/08/05 18:05:05 $
**
** Author: Daniel Vik
**
** Description: Emulation of the Z80/Z80 processor
**
** More info: http://www.bluemsx.com
**
** Copyright (C) 2003-2006 Daniel Vik
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

#include "z80.h"
#include <stdlib.h>
#include <stdio.h>

#define INT_LOW   0
#define INT_EDGE  1
#define INT_HIGH  2

#define DELAY_MEM      z80.systemTime += 3
#define DELAY_MEMOP    z80.systemTime += 3
#define DELAY_MEMPAGE  z80.systemTime += 0
#define DELAY_PREIO    z80.systemTime += 1
#define DELAY_POSTIO   z80.systemTime += 3
#define DELAY_M1       z80.systemTime += 2
#define DELAY_XD       z80.systemTime += 1
#define DELAY_IM       z80.systemTime += 2
#define DELAY_IM2      z80.systemTime += 19
#define DELAY_NMI      z80.systemTime += 11
#define DELAY_PARALLEL z80.systemTime += 2
#define DELAY_BLOCK    z80.systemTime += 5
#define DELAY_ADD8     z80.systemTime += 5
#define DELAY_ADD16    z80.systemTime += 7
#define DELAY_BIT      z80.systemTime += 1
#define DELAY_CALL     z80.systemTime += 1
#define DELAY_DJNZ     z80.systemTime += 1
#define DELAY_EXSPHL   z80.systemTime += 3
#define DELAY_INC      z80.systemTime += 1
#define DELAY_INC16    z80.systemTime += 2
#define DELAY_INOUT    z80.systemTime += 1
#define DELAY_LD       z80.systemTime += 1
#define DELAY_LDI      z80.systemTime += 2
#define DELAY_MUL8     z80.systemTime += 0
#define DELAY_MUL16    z80.systemTime += 0
#define DELAY_PUSH     z80.systemTime += 1
#define DELAY_RET      z80.systemTime += 1
#define DELAY_RLD      z80.systemTime += 4
#define DELAY_T9769    z80.systemTime += 0
#define DELAY_LDSPHL   z80.systemTime += 2
#define DELAY_BITIX    z80.systemTime += 2

typedef void (*Opcode)(void);
typedef void (*OpcodeNn)(UInt16);

static UInt8  ZSXYTable[256];
static UInt8  ZSPXYTable[256];
static UInt8  ZSPHTable[256];
static UIntN  DAATable[0x800];

#if 1
extern UInt8 FARPTR ram[4];
extern UInt8 slot[4];

static UIntN tmpAddr;
static UIntN tmpPage;

#define readMemory(addr) ( tmpAddr = addr, ram[tmpAddr >> 14][tmpAddr & 0x3fff] )
#define writeMemory(addr, val) { tmpAddr = addr; tmpPage = tmpAddr >> 14; if (slot[tmpPage] == 3) ram[tmpPage][tmpAddr & 0x3fff] = val; }
#endif

static Z80 z80;

static void cb(void);
static void dd(void);
static void ed(void);
static void fd(void);
static void dd_cb(void);
static void fd_cb(void);

static void updateFastLoop(void)
{
    if (z80.regs.halt) {
        z80.fastTimeout = z80.timeout;
        return;
    }

	if (z80.regs.ei_mode) {
        z80.fastTimeout = z80.systemTime;
        return;
    }

    if (! ((z80.intState==INT_LOW && z80.regs.iff1)||(z80.nmiState==INT_EDGE)) ) {
        z80.fastTimeout = z80.timeout;
        return;
    }

    z80.fastTimeout = z80.systemTime;
}

static UInt8 readPort(UInt16 port) {
    UInt8 value;

    z80.regs.SH.W = port + 1;
    DELAY_PREIO;

    value = readIoPort(port);
    DELAY_POSTIO;

    return value;

}

static void writePort(UInt16 port, UInt8 value) {
    z80.regs.SH.W = port + 1;
    DELAY_PREIO;

    writeIoPort(port, value);
    DELAY_POSTIO;

}


static UInt8 readMem(UIntN address) {
    DELAY_MEM;
    return readMemory(address);
}

static UIntN readOpcode(UIntN address) {
    DELAY_MEMOP;
    return readMemory(address);
}

static void writeMem(UIntN address, UInt8 value) {
    DELAY_MEM;
    writeMemory(address, value);
}

static void INC(UInt8* reg) {
    UInt8 regVal = ++(*reg);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | ZSXYTable[regVal] |
        (regVal == 0x80 ? V_FLAG : 0) |
        (!(regVal & 0x0f) ? H_FLAG : 0);
}

static void DEC(UInt8* reg) {
    UInt8 regVal = --(*reg);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | ZSXYTable[regVal] | 
        N_FLAG | (regVal == 0x7f ? V_FLAG : 0) |
        ((regVal & 0x0f) == 0x0f ? H_FLAG : 0);
}

static void ADD(UInt8 reg) {
    Int16 rv = z80.regs.AF.B.h;
    rv += reg;
    z80.regs.AF.B.l = ZSXYTable[rv & 0xff] | ((rv >> 8) & C_FLAG) |
        ((z80.regs.AF.B.h ^ rv ^ reg) & H_FLAG) |
        ((((reg ^ z80.regs.AF.B.h ^ 0x80) & (reg ^ rv)) >> 5) & V_FLAG);
    z80.regs.AF.B.h = (UInt8)rv;
}

static void ADDW(UInt16* reg1, UInt16 reg2) { //DIFF

    Int32 rv = *reg1;
    rv += reg2;
    
    z80.regs.SH.W   = *reg1 + 1;
    z80.regs.AF.B.l = (UInt8)((z80.regs.AF.B.l & (S_FLAG | Z_FLAG | V_FLAG)) |
        (((*reg1 ^ reg2 ^ rv) >> 8) & H_FLAG) |
        ((rv >> 16) & C_FLAG) |
        ((rv >> 8) & (X_FLAG | Y_FLAG)));
    *reg1 = (UInt16)rv;
    DELAY_ADD16;
}

static void ADC(UInt8 reg) {
    Int16 rv = z80.regs.AF.B.h;
    rv += reg;
    rv += (z80.regs.AF.B.l & C_FLAG);
    z80.regs.AF.B.l = ZSXYTable[rv & 0xff] | ((rv >> 8) & C_FLAG) |
        ((z80.regs.AF.B.h ^ rv ^ reg) & H_FLAG) |
        ((((reg ^ z80.regs.AF.B.h ^ 0x80) & (reg ^ rv)) >> 5) & V_FLAG);
    z80.regs.AF.B.h = (UInt8)rv;
}

static void ADCW(UInt16 reg) {
    Int32 rv = z80.regs.HL.W;
    rv += reg;
    rv += (z80.regs.AF.B.l & C_FLAG);
    
    z80.regs.SH.W   = z80.regs.HL.W + 1;
    z80.regs.AF.B.l = (UInt8)((((z80.regs.HL.W ^ reg ^ rv) >> 8) & H_FLAG) | 
        ((rv >> 16) & C_FLAG) | ((rv & 0xffff) ? 0 : Z_FLAG) |
        ((((reg ^ z80.regs.HL.W ^ 0x8000) & (reg ^ rv)) >> 13) & V_FLAG) |
        ((rv >> 8) & (S_FLAG | X_FLAG | Y_FLAG)));
    z80.regs.HL.W = (UInt16)rv;
    DELAY_ADD16;
}

static void SUB(UInt8 reg) {
    Int16 regVal = z80.regs.AF.B.h;
    Int16 rv = regVal - reg;
    z80.regs.AF.B.l = ZSXYTable[rv & 0xff] | ((rv >> 8) & C_FLAG) |
        ((regVal ^ rv ^ reg) & H_FLAG) | N_FLAG |
        ((((reg ^ regVal) & (rv ^ regVal)) >> 5) & V_FLAG);
    z80.regs.AF.B.h = (UInt8)rv;
} 

static void SBC(UInt8 reg) {
    Int16 regVal = z80.regs.AF.B.h;
    Int16 rv = regVal - reg - (z80.regs.AF.B.l & C_FLAG);
    z80.regs.AF.B.l = ZSXYTable[rv & 0xff] | ((rv >> 8) & C_FLAG) |
        ((regVal ^ rv ^ reg) & H_FLAG) | N_FLAG |
        ((((reg ^ regVal) & (rv ^ regVal)) >> 5) & V_FLAG);
    z80.regs.AF.B.h = (UInt8)rv;
}

static void SBCW(UInt16 reg) {
    Int32 regVal = z80.regs.HL.W;
    Int32 rv = regVal - reg - (z80.regs.AF.B.l & C_FLAG);
    z80.regs.SH.W   = (UInt16)(regVal + 1);
    z80.regs.AF.B.l = (UInt8)((((regVal ^ reg ^ rv) >> 8) & H_FLAG) | N_FLAG |
        ((rv >> 16) & C_FLAG) | ((rv & 0xffff) ? 0 : Z_FLAG) | 
        ((((reg ^ regVal) & (regVal ^ rv)) >> 13) & V_FLAG) |
        ((rv >> 8) & (S_FLAG | X_FLAG | Y_FLAG)));
    z80.regs.HL.W = (UInt16)rv;
    DELAY_ADD16;
}

static void CP(UInt8 reg) {
    Int16 regVal = z80.regs.AF.B.h;
    Int16 rv = regVal - reg;
    z80.regs.AF.B.l = (ZSPXYTable[rv & 0xff] & (Z_FLAG | S_FLAG)) | 
        ((rv >> 8) & C_FLAG) |
        ((regVal ^ rv ^ reg) & H_FLAG) | N_FLAG |
        ((((reg ^ regVal) & (rv ^ regVal)) >> 5) & V_FLAG) |
        (reg & (X_FLAG | Y_FLAG));
}

static void AND(UInt8 reg) {
    z80.regs.AF.B.h &= reg;
    z80.regs.AF.B.l = ZSPXYTable[z80.regs.AF.B.h] | H_FLAG;
} 

static void OR(UInt8 reg) {
    z80.regs.AF.B.h |= reg;
    z80.regs.AF.B.l = ZSPXYTable[z80.regs.AF.B.h];
} 

static void XOR(UInt8 reg) {
    z80.regs.AF.B.h ^= reg;
    z80.regs.AF.B.l = ZSPXYTable[z80.regs.AF.B.h];
}

//static void MULU(UInt8 reg) { // Diff on mask // RuMSX: (S_FLAG & V_FLAG)
//    z80.regs.HL.W = (Int16)z80.regs.AF.B.h * reg;
//    z80.regs.AF.B.l = (z80.regs.AF.B.l & (N_FLAG | H_FLAG)) |
//        (z80.regs.HL.W ? 0 : Z_FLAG) | ((z80.regs.HL.W >> 15) & C_FLAG);
//    DELAY_MUL8;
//}

//static void MULUW(UInt16 reg) { // Diff on mask // RuMSX: (S_FLAG & V_FLAG)
//    UInt32 rv = (UInt32)z80.regs.HL.W * reg;
//    z80.regs.DE.W = (UInt16)(rv >> 16);
//    z80.regs.HL.W = (UInt16)(rv & 0xffff);
//    z80.regs.AF.B.l = (z80.regs.AF.B.l & (N_FLAG | H_FLAG)) |
//        (rv ? 0 : Z_FLAG) | (UInt8)((rv >> 31) & C_FLAG);
//    DELAY_MUL16;
//}

static void SLA(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal = regVal << 1;
    z80.regs.AF.B.l = ZSPXYTable[regVal] | ((*reg >> 7) & C_FLAG);
    *reg = regVal;
}

static void SLL(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal = (regVal << 1) | 1;
    z80.regs.AF.B.l = ZSPXYTable[regVal] | ((*reg >> 7) & C_FLAG);
    *reg = regVal;
}

static void SRA(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal = (regVal >> 1) | (regVal & 0x80);
    z80.regs.AF.B.l = ZSPXYTable[regVal] | (*reg & C_FLAG);
    *reg = regVal;
}

static void SRL(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal = regVal >> 1;
    z80.regs.AF.B.l = ZSPXYTable[regVal] | (*reg & C_FLAG);
    *reg = regVal;
}

static void RL(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal = (regVal << 1) | (z80.regs.AF.B.l & 0x01);
    z80.regs.AF.B.l = ZSPXYTable[regVal] | ((*reg >> 7) & C_FLAG);
    *reg = regVal;
}

static void RLC(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal= (regVal << 1) | (regVal >> 7);
    z80.regs.AF.B.l = ZSPXYTable[regVal] | (regVal & C_FLAG);
    *reg = regVal;
}

static void RR(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal = (regVal >> 1) | (z80.regs.AF.B.l << 7);
    z80.regs.AF.B.l = ZSPXYTable[regVal] | (*reg & C_FLAG);
    *reg = regVal;
}

static void RRC(UInt8* reg) {
    UInt8 regVal = *reg;
    regVal= (regVal >> 1) | (regVal << 7);
    z80.regs.AF.B.l = ZSPXYTable[regVal] | ((regVal >> 7) & C_FLAG);
    *reg = regVal;
}

static void BIT(UInt8 bit, UInt8 reg) {
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) |
        (reg & (X_FLAG | Y_FLAG)) | ZSPHTable[reg & (1 << bit)];
}

static void RES(UInt8 bit, UInt8* reg) {
    *reg &= ~(1 << bit);
}

static void SET(UInt8 bit, UInt8* reg) {
    *reg |= 1 << bit;
}

#define JR_COND(cond) \
    if (cond) { \
        RegisterPair addr; \
        addr.W = z80.regs.PC.W + 1 + (Int8)readOpcode(z80.regs.PC.W); \
        z80.regs.PC.W = addr.W; \
        z80.regs.SH.W = addr.W; \
        DELAY_ADD8; \
    } else { \
        readOpcode(z80.regs.PC.W++); \
    }

#define JP_COND(cond) \
    RegisterPair addr; \
    addr.B.l = readOpcode(z80.regs.PC.W++); \
    addr.B.h = readOpcode(z80.regs.PC.W++); \
    z80.regs.SH.W = addr.W; \
    if (cond) { \
        z80.regs.PC.W = addr.W; \
    }

#define CALL_COND(cond) \
    RegisterPair addr; \
    addr.B.l = readOpcode(z80.regs.PC.W++); \
    addr.B.h = readOpcode(z80.regs.PC.W++); \
    z80.regs.SH.W = addr.W; \
    if (cond) { \
        DELAY_CALL; \
        writeMem(--z80.regs.SP.W, z80.regs.PC.B.h); \
        writeMem(--z80.regs.SP.W, z80.regs.PC.B.l); \
        z80.regs.PC.W = addr.W; \
    }
static void PUSH(UInt16* reg) {
    RegisterPair* pair = (RegisterPair*)reg;
    DELAY_PUSH;
    writeMem(--z80.regs.SP.W, pair->B.h);
    writeMem(--z80.regs.SP.W, pair->B.l);
}

static void POP(UInt16* reg) {
    RegisterPair* pair = (RegisterPair*)reg;
    pair->B.l = readMem(z80.regs.SP.W++);
    pair->B.h = readMem(z80.regs.SP.W++);
}

static void RST(UInt16 vector) {
    PUSH(&z80.regs.PC.W);
    z80.regs.PC.W = vector;
    z80.regs.SH.W = vector;
}

static void EX_SP(UInt16* reg) {
    RegisterPair* pair = (RegisterPair*)reg;
    RegisterPair addr;
    
    addr.B.l = readMem(z80.regs.SP.W++);
    addr.B.h = readMem(z80.regs.SP.W);
    writeMem(z80.regs.SP.W--, pair->B.h);
    writeMem(z80.regs.SP.W,   pair->B.l);
    pair->W   = addr.W;
    z80.regs.SH.W = addr.W;
    DELAY_EXSPHL;
}

#if 1
#define M1() { z80.regs.R++; DELAY_M1; }
#else
static void M1(void) {
    z80.regs.R++; 
    DELAY_M1;
}
#endif


static void nop(void) {
}

static void ld_bc_word(void) {
    z80.regs.BC.B.l = readOpcode(z80.regs.PC.W++);
    z80.regs.BC.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_de_word(void) {
    z80.regs.DE.B.l = readOpcode(z80.regs.PC.W++);
    z80.regs.DE.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_hl_word(void) {
    z80.regs.HL.B.l = readOpcode(z80.regs.PC.W++);
    z80.regs.HL.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_ix_word(void) {
    z80.regs.IX.B.l = readOpcode(z80.regs.PC.W++);
    z80.regs.IX.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_iy_word(void) {
    z80.regs.IY.B.l = readOpcode(z80.regs.PC.W++);
    z80.regs.IY.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_sp_word(void) {
    z80.regs.SP.B.l = readOpcode(z80.regs.PC.W++);
    z80.regs.SP.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_sp_hl(void) { 
    DELAY_LDSPHL;                  // white cat append
    z80.regs.SP.W = z80.regs.HL.W; 
}

static void ld_sp_ix(void) {
    DELAY_LDSPHL;                  // white cat append
    z80.regs.SP.W = z80.regs.IX.W; 
}

static void ld_sp_iy(void) { 
    DELAY_LDSPHL;                  // white cat append
    z80.regs.SP.W = z80.regs.IY.W; 
}

static void ld_xbc_a(void) {
    writeMem(z80.regs.BC.W, z80.regs.AF.B.h);
}

static void ld_xde_a(void) {
    writeMem(z80.regs.DE.W, z80.regs.AF.B.h);
}

static void ld_xhl_a(void) {
    writeMem(z80.regs.HL.W, z80.regs.AF.B.h);
}

static void ld_a_xbc(void) {
    z80.regs.AF.B.h = readMem(z80.regs.BC.W);
}

static void ld_a_xde(void) {
    z80.regs.AF.B.h = readMem(z80.regs.DE.W);
}

static void ld_xhl_byte(void) {
    writeMem(z80.regs.HL.W, readOpcode(z80.regs.PC.W++));
}

static void ld_i_a(void) {
    DELAY_LD;
    z80.regs.I = z80.regs.AF.B.h;
}

static void ld_r_a(void) {
    DELAY_LD;
    z80.regs.R = z80.regs.R2 = z80.regs.AF.B.h;
}

static void ld_a_i(void) {
    DELAY_LD;
    z80.regs.AF.B.h = z80.regs.I;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSXYTable[z80.regs.AF.B.h] | (z80.regs.iff2 << 2);
}

static void ld_a_r(void) {
    DELAY_LD;
    z80.regs.AF.B.h = (z80.regs.R & 0x7f) | (z80.regs.R2 & 0x80);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSXYTable[z80.regs.AF.B.h] | (z80.regs.iff2 << 2);
}

static void inc_bc(void) {
    z80.regs.BC.W++; DELAY_INC16;
}

static void inc_de(void) {
    z80.regs.DE.W++; DELAY_INC16;
}

static void inc_hl(void) {
    z80.regs.HL.W++; DELAY_INC16;
}

static void inc_ix(void) {
    z80.regs.IX.W++; DELAY_INC16;
}

static void inc_iy(void) {
    z80.regs.IY.W++; DELAY_INC16;
}

static void inc_sp(void) {
    z80.regs.SP.W++; DELAY_INC16;
}

static void inc_a(void) {
    INC(&z80.regs.AF.B.h);
}

static void inc_b(void) {
    INC(&z80.regs.BC.B.h);
}

static void inc_c(void) {
    INC(&z80.regs.BC.B.l);
}

static void inc_d(void) {
    INC(&z80.regs.DE.B.h);
}

static void inc_e(void) {
    INC(&z80.regs.DE.B.l);
}

static void inc_h(void) {
    INC(&z80.regs.HL.B.h);
}

static void inc_l(void) {
    INC(&z80.regs.HL.B.l);
}

static void inc_ixh(void) { 
    INC(&z80.regs.IX.B.h); 
}

static void inc_ixl(void) { 
    INC(&z80.regs.IX.B.l); 
}

static void inc_iyh(void) { 
    INC(&z80.regs.IY.B.h); 
}


static void inc_iyl(void) { 
    INC(&z80.regs.IY.B.l); 
}

static void inc_xhl(void) {
    UInt8 value = readMem(z80.regs.HL.W);
    INC(&value);
    DELAY_INC;
    writeMem(z80.regs.HL.W, value);
}

static void inc_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    UInt8 value;
    DELAY_ADD8;
    value = readMem(addr);
    INC(&value);
    DELAY_INC;
    writeMem(addr, value);
    z80.regs.SH.W = addr;
}

static void inc_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    UInt8 value;
    DELAY_ADD8;
    value = readMem(addr);
    INC(&value);
    DELAY_INC;
    writeMem(addr, value);
    z80.regs.SH.W = addr;
}

static void dec_bc(void) {
    z80.regs.BC.W--; DELAY_INC16;
}

static void dec_de(void) {
    z80.regs.DE.W--; DELAY_INC16;
}

static void dec_hl(void) {
    z80.regs.HL.W--; DELAY_INC16;
}

static void dec_ix(void) {
    z80.regs.IX.W--; DELAY_INC16;
}

static void dec_iy(void) {
    z80.regs.IY.W--; DELAY_INC16;
}

static void dec_sp(void) {
    z80.regs.SP.W--; DELAY_INC16;
}

static void dec_a(void) {
    DEC(&z80.regs.AF.B.h);
}

static void dec_b(void) {
    DEC(&z80.regs.BC.B.h);
}

static void dec_c(void) {
    DEC(&z80.regs.BC.B.l);
}

static void dec_d(void) {
    DEC(&z80.regs.DE.B.h);
}

static void dec_e(void) {
    DEC(&z80.regs.DE.B.l);
}

static void dec_h(void) {
    DEC(&z80.regs.HL.B.h);
}

static void dec_l(void) {
    DEC(&z80.regs.HL.B.l);
}

static void dec_ixh(void) { 
    DEC(&z80.regs.IX.B.h); 
}

static void dec_ixl(void) { 
    DEC(&z80.regs.IX.B.l); 
}

static void dec_iyh(void) { 
    DEC(&z80.regs.IY.B.h); 
}

static void dec_iyl(void) { 
    DEC(&z80.regs.IY.B.l); 
}

static void dec_xhl(void) {
    UInt8 value = readMem(z80.regs.HL.W);
    DEC(&value);
    DELAY_INC;
    writeMem(z80.regs.HL.W, value);
}

static void dec_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    UInt8 value;
    DELAY_ADD8;
    value = readMem(addr);
    DEC(&value);
    DELAY_INC;
    writeMem(addr, value);
    z80.regs.SH.W = addr;
}

static void dec_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    UInt8 value;
    DELAY_ADD8;
    value = readMem(addr);
    DEC(&value);
    DELAY_INC;
    writeMem(addr, value);
    z80.regs.SH.W = addr;
}

static void ld_a_a(void) { 
}

static void ld_a_b(void) { 
    z80.regs.AF.B.h = z80.regs.BC.B.h; 
}

static void ld_a_c(void) { 
    z80.regs.AF.B.h = z80.regs.BC.B.l; 
}

static void ld_a_d(void) { 
    z80.regs.AF.B.h = z80.regs.DE.B.h; 
}

static void ld_a_e(void) { 
    z80.regs.AF.B.h = z80.regs.DE.B.l; 
}

static void ld_a_h(void) { 
    z80.regs.AF.B.h = z80.regs.HL.B.h; 
}

static void ld_a_l(void) { 
    z80.regs.AF.B.h = z80.regs.HL.B.l; 
}

static void ld_a_ixh(void) { 
    z80.regs.AF.B.h = z80.regs.IX.B.h; 
}

static void ld_a_ixl(void) { 
    z80.regs.AF.B.h = z80.regs.IX.B.l; 
}

static void ld_a_iyh(void) { 
    z80.regs.AF.B.h = z80.regs.IY.B.h; 
}

static void ld_a_iyl(void) { 
    z80.regs.AF.B.h = z80.regs.IY.B.l; 
}

static void ld_a_xhl(void) { 
    z80.regs.AF.B.h = readMem(z80.regs.HL.W); 
}

static void ld_a_xix(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.AF.B.h = readMem(addr);
}

static void ld_a_xiy(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.AF.B.h = readMem(addr);
}

static void ld_xiy_a(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.AF.B.h);
}

static void ld_xix_a(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.AF.B.h);
}

static void ld_b_a(void) { 
    z80.regs.BC.B.h = z80.regs.AF.B.h; 
}

static void ld_b_b(void) { 
}

static void ld_b_c(void) { 
    z80.regs.BC.B.h = z80.regs.BC.B.l; 
}

static void ld_b_d(void) { 
    z80.regs.BC.B.h = z80.regs.DE.B.h; 
}

static void ld_b_e(void) { 
    z80.regs.BC.B.h = z80.regs.DE.B.l; 
}

static void ld_b_h(void) { 
    z80.regs.BC.B.h = z80.regs.HL.B.h; 
}

static void ld_b_l(void) { 
    z80.regs.BC.B.h = z80.regs.HL.B.l; 
}

static void ld_b_ixh(void) { 
    z80.regs.BC.B.h = z80.regs.IX.B.h; 
}

static void ld_b_ixl(void) { 
    z80.regs.BC.B.h = z80.regs.IX.B.l; 
}

static void ld_b_iyh(void) { 
    z80.regs.BC.B.h = z80.regs.IY.B.h; 
}

static void ld_b_iyl(void) { 
    z80.regs.BC.B.h = z80.regs.IY.B.l; 
}

static void ld_b_xhl(void) { 
    z80.regs.BC.B.h = readMem(z80.regs.HL.W); 
}

static void ld_b_xix(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.BC.B.h = readMem(addr);
}

static void ld_b_xiy(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.BC.B.h = readMem(addr);
}

static void ld_xix_b(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.BC.B.h);
}

static void ld_xiy_b(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.BC.B.h);
}

static void ld_c_a(void) { 
    z80.regs.BC.B.l = z80.regs.AF.B.h; 
}

static void ld_c_b(void) { 
    z80.regs.BC.B.l = z80.regs.BC.B.h; 
}

static void ld_c_c(void) { 
}

static void ld_c_d(void) { 
    z80.regs.BC.B.l = z80.regs.DE.B.h; 
}

static void ld_c_e(void) {
    z80.regs.BC.B.l = z80.regs.DE.B.l;
}

static void ld_c_h(void) { 
    z80.regs.BC.B.l = z80.regs.HL.B.h; 
}

static void ld_c_l(void) { 
    z80.regs.BC.B.l = z80.regs.HL.B.l; 
}

static void ld_c_ixh(void) { 
    z80.regs.BC.B.l = z80.regs.IX.B.h; 
}

static void ld_c_ixl(void) { 
    z80.regs.BC.B.l = z80.regs.IX.B.l; 
}

static void ld_c_iyh(void) { 
    z80.regs.BC.B.l = z80.regs.IY.B.h; 
}

static void ld_c_iyl(void) { 
    z80.regs.BC.B.l = z80.regs.IY.B.l; 
}

static void ld_c_xhl(void) { 
    z80.regs.BC.B.l = readMem(z80.regs.HL.W); 
}

static void ld_c_xix(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.BC.B.l = readMem(addr);
}

static void ld_c_xiy(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.BC.B.l = readMem(addr);
}

static void ld_xix_c(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.BC.B.l);
}

static void ld_xiy_c(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.BC.B.l);
}

static void ld_d_a(void) { 
    z80.regs.DE.B.h = z80.regs.AF.B.h; 
}

static void ld_d_b(void) { 
    z80.regs.DE.B.h = z80.regs.BC.B.h; 
}

static void ld_d_c(void) { 
    z80.regs.DE.B.h = z80.regs.BC.B.l; 
}

static void ld_d_d(void) { 
}

static void ld_d_e(void) { 
    z80.regs.DE.B.h = z80.regs.DE.B.l; 
}

static void ld_d_h(void) { 
    z80.regs.DE.B.h = z80.regs.HL.B.h; 
}

static void ld_d_l(void) { 
    z80.regs.DE.B.h = z80.regs.HL.B.l; 
}

static void ld_d_ixh(void) { 
    z80.regs.DE.B.h = z80.regs.IX.B.h; 
}

static void ld_d_ixl(void) { 
    z80.regs.DE.B.h = z80.regs.IX.B.l; 
}

static void ld_d_iyh(void) { 
    z80.regs.DE.B.h = z80.regs.IY.B.h; 
}

static void ld_d_iyl(void) { 
    z80.regs.DE.B.h = z80.regs.IY.B.l; 
}

static void ld_d_xhl(void) { 
    z80.regs.DE.B.h = readMem(z80.regs.HL.W); 
}

static void ld_d_xix(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.DE.B.h = readMem(addr);
}

static void ld_d_xiy(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.DE.B.h = readMem(addr);
}

static void ld_xix_d(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.DE.B.h);

}
static void ld_xiy_d(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.DE.B.h);
}

static void ld_e_a(void) { 
    z80.regs.DE.B.l = z80.regs.AF.B.h; 
}

static void ld_e_b(void) { 
    z80.regs.DE.B.l = z80.regs.BC.B.h; 
}

static void ld_e_c(void) { 
    z80.regs.DE.B.l = z80.regs.BC.B.l; 
}

static void ld_e_d(void) {
    z80.regs.DE.B.l = z80.regs.DE.B.h; 
}

static void ld_e_e(void) { 
}

static void ld_e_h(void) {
    z80.regs.DE.B.l = z80.regs.HL.B.h; 
}

static void ld_e_l(void) { 
    z80.regs.DE.B.l = z80.regs.HL.B.l; 
}

static void ld_e_ixh(void) { 
    z80.regs.DE.B.l = z80.regs.IX.B.h; 
}

static void ld_e_ixl(void) { 
    z80.regs.DE.B.l = z80.regs.IX.B.l; 
}

static void ld_e_iyh(void) { 
    z80.regs.DE.B.l = z80.regs.IY.B.h; 
}

static void ld_e_iyl(void) { 
    z80.regs.DE.B.l = z80.regs.IY.B.l; 
}

static void ld_e_xhl(void) { 
    z80.regs.DE.B.l = readMem(z80.regs.HL.W); 
}

static void ld_e_xix(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.DE.B.l = readMem(addr);
}

static void ld_e_xiy(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.DE.B.l = readMem(addr);
}

static void ld_xix_e(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.DE.B.l);
}

static void ld_xiy_e(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.DE.B.l);
}

static void ld_h_a(void) { 
    z80.regs.HL.B.h = z80.regs.AF.B.h;
}

static void ld_h_b(void) { 
    z80.regs.HL.B.h = z80.regs.BC.B.h;
}

static void ld_h_c(void) { 
    z80.regs.HL.B.h = z80.regs.BC.B.l;
}

static void ld_h_d(void) {
    z80.regs.HL.B.h = z80.regs.DE.B.h;
}

static void ld_h_e(void) {
    z80.regs.HL.B.h = z80.regs.DE.B.l; 
}

static void ld_h_h(void) { 
    z80.regs.HL.B.h = z80.regs.HL.B.h; 
}

static void ld_h_l(void) { 
    z80.regs.HL.B.h = z80.regs.HL.B.l; 
}

//static void ld_h_ixh(void) {
//    z80.regs.HL.B.h = z80.regs.IX.B.h; 
//}

//static void ld_h_ixl(void) {
//    z80.regs.HL.B.h = z80.regs.IX.B.l;
//}

//static void ld_h_iyh(void) {
//    z80.regs.HL.B.h = z80.regs.IY.B.h; 
//}

//static void ld_h_iyl(void) {
//    z80.regs.HL.B.h = z80.regs.IY.B.l; 
//}

static void ld_h_xhl(void) {
    z80.regs.HL.B.h = readMem(z80.regs.HL.W); 
}

static void ld_h_xix(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.HL.B.h = readMem(addr);
}

static void ld_h_xiy(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.HL.B.h = readMem(addr);
}

static void ld_xix_h(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.HL.B.h);
}

static void ld_xiy_h(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.HL.B.h);
}

static void ld_l_a(void) { 
    z80.regs.HL.B.l = z80.regs.AF.B.h; 
}

static void ld_l_b(void) {
    z80.regs.HL.B.l = z80.regs.BC.B.h; 
}

static void ld_l_c(void) {
    z80.regs.HL.B.l = z80.regs.BC.B.l;
}

static void ld_l_d(void) {
    z80.regs.HL.B.l = z80.regs.DE.B.h;
}

static void ld_l_e(void) {
    z80.regs.HL.B.l = z80.regs.DE.B.l;
}

static void ld_l_h(void) {
    z80.regs.HL.B.l = z80.regs.HL.B.h;
}

static void ld_l_l(void) {
    z80.regs.HL.B.l = z80.regs.HL.B.l;
}

//static void ld_l_ixh(void) {
//    z80.regs.HL.B.l = z80.regs.IX.B.h;
//}

//static void ld_l_ixl(void) {
//    z80.regs.HL.B.l = z80.regs.IX.B.l;
//}

//static void ld_l_iyh(void) {
//    z80.regs.HL.B.l = z80.regs.IY.B.h;
//}

//static void ld_l_iyl(void) {
//    z80.regs.HL.B.l = z80.regs.IY.B.l; 
//}

static void ld_l_xhl(void) {
    z80.regs.HL.B.l = readMem(z80.regs.HL.W); 
}

static void ld_l_xix(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.HL.B.l = readMem(addr);
}

static void ld_l_xiy(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    z80.regs.HL.B.l = readMem(addr);
}

static void ld_xix_l(void) { 
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.HL.B.l);
}

static void ld_xiy_l(void) { 
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8;
    z80.regs.SH.W = addr;
    writeMem(addr, z80.regs.HL.B.l);
}

static void ld_ixh_a(void) { 
    z80.regs.IX.B.h = z80.regs.AF.B.h;
}

static void ld_ixh_b(void) {
    z80.regs.IX.B.h = z80.regs.BC.B.h;
}

static void ld_ixh_c(void) {
    z80.regs.IX.B.h = z80.regs.BC.B.l;
}

static void ld_ixh_d(void) {
    z80.regs.IX.B.h = z80.regs.DE.B.h; 
}

static void ld_ixh_e(void) {
    z80.regs.IX.B.h = z80.regs.DE.B.l;
}

static void ld_ixh_ixh(void) {
}

static void ld_ixh_ixl(void) {
    z80.regs.IX.B.h = z80.regs.IX.B.l;
}

static void ld_ixl_a(void) { 
    z80.regs.IX.B.l = z80.regs.AF.B.h;
}

static void ld_ixl_b(void) {
    z80.regs.IX.B.l = z80.regs.BC.B.h;
}

static void ld_ixl_c(void) {
    z80.regs.IX.B.l = z80.regs.BC.B.l; 
}

static void ld_ixl_d(void) { 
    z80.regs.IX.B.l = z80.regs.DE.B.h;
}

static void ld_ixl_e(void) {
    z80.regs.IX.B.l = z80.regs.DE.B.l;
}

static void ld_ixl_ixh(void) {
    z80.regs.IX.B.l = z80.regs.IX.B.h;
}

static void ld_ixl_ixl(void) {
}

static void ld_iyh_a(void) {
    z80.regs.IY.B.h = z80.regs.AF.B.h;
}

static void ld_iyh_b(void) {
    z80.regs.IY.B.h = z80.regs.BC.B.h;
}

static void ld_iyh_c(void) { 
    z80.regs.IY.B.h = z80.regs.BC.B.l;
}

static void ld_iyh_d(void) {
    z80.regs.IY.B.h = z80.regs.DE.B.h; 
}

static void ld_iyh_e(void) {
    z80.regs.IY.B.h = z80.regs.DE.B.l; 
}

static void ld_iyh_iyh(void) {
}

static void ld_iyh_iyl(void) {
    z80.regs.IY.B.h = z80.regs.IY.B.l; 
}

static void ld_iyl_a(void) {
    z80.regs.IY.B.l = z80.regs.AF.B.h;
}

static void ld_iyl_b(void) { 
    z80.regs.IY.B.l = z80.regs.BC.B.h;
}

static void ld_iyl_c(void) { 
    z80.regs.IY.B.l = z80.regs.BC.B.l;
}

static void ld_iyl_d(void) { 
    z80.regs.IY.B.l = z80.regs.DE.B.h;
}

static void ld_iyl_e(void) { 
    z80.regs.IY.B.l = z80.regs.DE.B.l;
}

static void ld_iyl_iyh(void) { 
    z80.regs.IY.B.l = z80.regs.IY.B.h;
}

static void ld_iyl_iyl(void) {
}

static void ld_xhl_b(void) { 
    writeMem(z80.regs.HL.W, z80.regs.BC.B.h);
}

static void ld_xhl_c(void) { 
    writeMem(z80.regs.HL.W, z80.regs.BC.B.l); 
}

static void ld_xhl_d(void) { 
    writeMem(z80.regs.HL.W, z80.regs.DE.B.h);
}

static void ld_xhl_e(void) { 
    writeMem(z80.regs.HL.W, z80.regs.DE.B.l);
}

static void ld_xhl_h(void) { 
    writeMem(z80.regs.HL.W, z80.regs.HL.B.h);
}

static void ld_xhl_l(void) { 
    writeMem(z80.regs.HL.W, z80.regs.HL.B.l);
}

static void ld_a_byte(void) {
    z80.regs.AF.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_b_byte(void) {
    z80.regs.BC.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_c_byte(void) {
    z80.regs.BC.B.l = readOpcode(z80.regs.PC.W++);
}

static void ld_d_byte(void) {
    z80.regs.DE.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_e_byte(void) {
    z80.regs.DE.B.l = readOpcode(z80.regs.PC.W++);
}

static void ld_h_byte(void) {
    z80.regs.HL.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_l_byte(void) {
    z80.regs.HL.B.l = readOpcode(z80.regs.PC.W++);
}

static void ld_ixh_byte(void) {

    z80.regs.IX.B.h = readOpcode(z80.regs.PC.W++);

}

static void ld_ixl_byte(void) {
    z80.regs.IX.B.l = readOpcode(z80.regs.PC.W++);
}

static void ld_iyh_byte(void) { 
    z80.regs.IY.B.h = readOpcode(z80.regs.PC.W++);
}

static void ld_iyl_byte(void) { 
    z80.regs.IY.B.l = readOpcode(z80.regs.PC.W++);
}

static void ld_xbyte_a(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.SH.W = z80.regs.AF.B.h << 8;
    writeMem(addr.W, z80.regs.AF.B.h);
}

static void ld_a_xbyte(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.AF.B.h = readMem(addr.W);
    z80.regs.SH.W = addr.W + 1;
}


static void ld_xix_byte(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    UInt8 value = readOpcode(z80.regs.PC.W++);
    DELAY_PARALLEL; 
    z80.regs.SH.W = addr;
    writeMem(addr, value);
}

static void ld_xiy_byte(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    UInt8 value = readOpcode(z80.regs.PC.W++);
    DELAY_PARALLEL; 
    z80.regs.SH.W = addr;
    writeMem(addr, value);
}

static void ld_xword_bc(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    writeMem(addr.W++, z80.regs.BC.B.l);
    writeMem(addr.W,   z80.regs.BC.B.h);
    z80.regs.SH.W = addr.W;
}

static void ld_xword_de(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    writeMem(addr.W++, z80.regs.DE.B.l);
    writeMem(addr.W,   z80.regs.DE.B.h);
    z80.regs.SH.W = addr.W;
}

static void ld_xword_hl(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    writeMem(addr.W++, z80.regs.HL.B.l);
    writeMem(addr.W,   z80.regs.HL.B.h);
    z80.regs.SH.W = addr.W;
}

static void ld_xword_ix(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    writeMem(addr.W++, z80.regs.IX.B.l);
    writeMem(addr.W,   z80.regs.IX.B.h);
    z80.regs.SH.W = addr.W;
}

static void ld_xword_iy(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    writeMem(addr.W++, z80.regs.IY.B.l);
    writeMem(addr.W,   z80.regs.IY.B.h);
    z80.regs.SH.W = addr.W;
}

static void ld_xword_sp(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    writeMem(addr.W++, z80.regs.SP.B.l);
    writeMem(addr.W,   z80.regs.SP.B.h);
    z80.regs.SH.W = addr.W;
}

static void ld_bc_xword(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.BC.B.l = readMem(addr.W++);
    z80.regs.BC.B.h = readMem(addr.W);
    z80.regs.SH.W = addr.W;
}

static void ld_de_xword(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.DE.B.l = readMem(addr.W++);
    z80.regs.DE.B.h = readMem(addr.W);
    z80.regs.SH.W = addr.W;
}

static void ld_hl_xword(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.HL.B.l = readMem(addr.W++);
    z80.regs.HL.B.h = readMem(addr.W);
    z80.regs.SH.W = addr.W;
}

static void ld_ix_xword(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.IX.B.l = readMem(addr.W++);
    z80.regs.IX.B.h = readMem(addr.W);
    z80.regs.SH.W = addr.W;
}

static void ld_iy_xword(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.IY.B.l = readMem(addr.W++);
    z80.regs.IY.B.h = readMem(addr.W);
    z80.regs.SH.W = addr.W;
}

static void ld_sp_xword(void) {
    RegisterPair addr;
    addr.B.l = readOpcode(z80.regs.PC.W++);
    addr.B.h = readOpcode(z80.regs.PC.W++);
    z80.regs.SP.B.l = readMem(addr.W++);
    z80.regs.SP.B.h = readMem(addr.W);
    z80.regs.SH.W = addr.W;
}

static void add_a_a(void) {
    ADD(z80.regs.AF.B.h); 
}

static void add_a_b(void) {
    ADD(z80.regs.BC.B.h);
}

static void add_a_c(void) {
    ADD(z80.regs.BC.B.l);
}

static void add_a_d(void) {
    ADD(z80.regs.DE.B.h);
}

static void add_a_e(void) {
    ADD(z80.regs.DE.B.l);
}

static void add_a_h(void) {
    ADD(z80.regs.HL.B.h); 
}

static void add_a_l(void) { 
    ADD(z80.regs.HL.B.l);
}

static void add_a_ixl(void) {
    ADD(z80.regs.IX.B.l); 
}

static void add_a_ixh(void) {
    ADD(z80.regs.IX.B.h);
}

static void add_a_iyl(void) {
    ADD(z80.regs.IY.B.l);
}

static void add_a_iyh(void) {
    ADD(z80.regs.IY.B.h);
}

static void add_a_byte(void) {
    ADD(readOpcode(z80.regs.PC.W++));
}

static void add_a_xhl(void) { 
    ADD(readMem(z80.regs.HL.W)); 
}

static void add_a_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    ADD(readMem(addr));
    z80.regs.SH.W = addr;
}

static void add_a_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    ADD(readMem(addr));
    z80.regs.SH.W = addr;
}

static void adc_a_a(void) {
    ADC(z80.regs.AF.B.h);
}

static void adc_a_b(void) {
    ADC(z80.regs.BC.B.h); 
}

static void adc_a_c(void) {
    ADC(z80.regs.BC.B.l); 
}

static void adc_a_d(void) {
    ADC(z80.regs.DE.B.h); 
}

static void adc_a_e(void) {
    ADC(z80.regs.DE.B.l);
}

static void adc_a_h(void) {
    ADC(z80.regs.HL.B.h);
}

static void adc_a_l(void) {
    ADC(z80.regs.HL.B.l);
}

static void adc_a_ixl(void) {
    ADC(z80.regs.IX.B.l);
}

static void adc_a_ixh(void) {
    ADC(z80.regs.IX.B.h);
}

static void adc_a_iyl(void) {
    ADC(z80.regs.IY.B.l);
}

static void adc_a_iyh(void) { 
    ADC(z80.regs.IY.B.h);
}

static void adc_a_byte(void) {
    ADC(readOpcode(z80.regs.PC.W++)); 
}

static void adc_a_xhl(void) {
    ADC(readMem(z80.regs.HL.W));
}

static void adc_a_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    z80.regs.SH.W = addr;
    ADC(readMem(addr));
}

static void adc_a_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    z80.regs.SH.W = addr;
    ADC(readMem(addr));
}

static void adc_hl_bc(void) {
    ADCW(z80.regs.BC.W);
}

static void adc_hl_de(void) { 
    ADCW(z80.regs.DE.W);
}

static void adc_hl_hl(void) {
    ADCW(z80.regs.HL.W);
}

static void adc_hl_sp(void) {
    ADCW(z80.regs.SP.W);
}

static void sub_a(void) {
    SUB(z80.regs.AF.B.h);
}

static void sub_b(void) {
    SUB(z80.regs.BC.B.h); 
}

static void sub_c(void) { 
    SUB(z80.regs.BC.B.l); 
}

static void sub_d(void) {
    SUB(z80.regs.DE.B.h); 
}

static void sub_e(void) {
    SUB(z80.regs.DE.B.l); 
}

static void sub_h(void) {
    SUB(z80.regs.HL.B.h);
}

static void sub_l(void) {
    SUB(z80.regs.HL.B.l);
}

static void sub_ixl(void) {
    SUB(z80.regs.IX.B.l); 
}

static void sub_ixh(void) {
    SUB(z80.regs.IX.B.h);
}

static void sub_iyl(void) {
    SUB(z80.regs.IY.B.l);
}

static void sub_iyh(void) {
    SUB(z80.regs.IY.B.h);
}

static void sub_byte(void) {
    SUB(readOpcode(z80.regs.PC.W++)); 
}

static void sub_xhl(void) { 
    SUB(readMem(z80.regs.HL.W)); 
}

static void sub_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    z80.regs.SH.W = addr;
    SUB(readMem(addr));
}

static void sub_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    z80.regs.SH.W = addr;
    SUB(readMem(addr));
}

static void neg(void) {
    UInt8 regVal = z80.regs.AF.B.h;
    z80.regs.AF.B.h = 0;
    SUB(regVal);
}

static void sbc_a_a(void) {
    SBC(z80.regs.AF.B.h); 
}

static void sbc_a_b(void) {
    SBC(z80.regs.BC.B.h); 
}

static void sbc_a_c(void) {
    SBC(z80.regs.BC.B.l); 
}

static void sbc_a_d(void) {
    SBC(z80.regs.DE.B.h);
}

static void sbc_a_e(void) {
    SBC(z80.regs.DE.B.l);
}

static void sbc_a_h(void) {
    SBC(z80.regs.HL.B.h);
}

static void sbc_a_l(void) {
    SBC(z80.regs.HL.B.l);
}

static void sbc_a_ixl(void) {
    SBC(z80.regs.IX.B.l);
}

static void sbc_a_ixh(void) {
    SBC(z80.regs.IX.B.h);
}

static void sbc_a_iyl(void) {
    SBC(z80.regs.IY.B.l);
}

static void sbc_a_iyh(void) { 
    SBC(z80.regs.IY.B.h);
}

static void sbc_a_byte(void) { 
    SBC(readOpcode(z80.regs.PC.W++));
}

static void sbc_a_xhl(void) { 
    SBC(readMem(z80.regs.HL.W)); 
}

static void sbc_a_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    SBC(readMem(addr));
    z80.regs.SH.W = addr;
}

static void sbc_a_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    SBC(readMem(addr));
    z80.regs.SH.W = addr;
}

static void sbc_hl_bc(void) {
    SBCW(z80.regs.BC.W);
}

static void sbc_hl_de(void) {
    SBCW(z80.regs.DE.W); 
}

static void sbc_hl_hl(void) {
    SBCW(z80.regs.HL.W);
}

static void sbc_hl_sp(void) {
    SBCW(z80.regs.SP.W);
}

static void cp_a(void) {
    CP(z80.regs.AF.B.h);
}

static void cp_b(void) {
    CP(z80.regs.BC.B.h);
}

static void cp_c(void) {
    CP(z80.regs.BC.B.l);
}

static void cp_d(void) {
    CP(z80.regs.DE.B.h);
}

static void cp_e(void) {
    CP(z80.regs.DE.B.l);
}

static void cp_h(void) {
    CP(z80.regs.HL.B.h);
}

static void cp_l(void) {
    CP(z80.regs.HL.B.l);
}

static void cp_ixl(void) {
    CP(z80.regs.IX.B.l);
}

static void cp_ixh(void) {
    CP(z80.regs.IX.B.h);
}

static void cp_iyl(void) { 
    CP(z80.regs.IY.B.l);
}

static void cp_iyh(void) {
    CP(z80.regs.IY.B.h);
}

static void cp_byte(void) {
    CP(readOpcode(z80.regs.PC.W++)); 
}

static void cp_xhl(void) { 
    CP(readMem(z80.regs.HL.W)); 
}

static void cp_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    CP(readMem(addr));
    z80.regs.SH.W = addr;
}

static void cp_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    CP(readMem(addr));
    z80.regs.SH.W = addr;
}

static void and_a(void) {
    AND(z80.regs.AF.B.h); 
}

static void and_b(void) {
    AND(z80.regs.BC.B.h); 
}

static void and_c(void) {
    AND(z80.regs.BC.B.l);
}

static void and_d(void) {
    AND(z80.regs.DE.B.h);
}

static void and_e(void) {
    AND(z80.regs.DE.B.l); 
}

static void and_h(void) {
    AND(z80.regs.HL.B.h);
}

static void and_l(void) { 
    AND(z80.regs.HL.B.l); 
}

static void and_ixl(void) {
    AND(z80.regs.IX.B.l);
}

static void and_ixh(void) {
    AND(z80.regs.IX.B.h); 
}

static void and_iyl(void) {
    AND(z80.regs.IY.B.l); 
}

static void and_iyh(void) {
    AND(z80.regs.IY.B.h); 
}

static void and_byte(void) {
    AND(readOpcode(z80.regs.PC.W++)); 
}

static void and_xhl(void) { 
    AND(readMem(z80.regs.HL.W));
}

static void and_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    AND(readMem(addr));
    z80.regs.SH.W = addr;
}

static void and_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    AND(readMem(addr));
    z80.regs.SH.W = addr;
}

static void or_a(void) {
    OR(z80.regs.AF.B.h);
}

static void or_b(void) {
    OR(z80.regs.BC.B.h);
}

static void or_c(void) {
    OR(z80.regs.BC.B.l);
}

static void or_d(void) {
    OR(z80.regs.DE.B.h);
}

static void or_e(void) {
    OR(z80.regs.DE.B.l);
}

static void or_h(void) {
    OR(z80.regs.HL.B.h); 
}

static void or_l(void) {
    OR(z80.regs.HL.B.l); 
}

static void or_ixl(void) {
    OR(z80.regs.IX.B.l); 
}

static void or_ixh(void) {
    OR(z80.regs.IX.B.h);
}

static void or_iyl(void) {
    OR(z80.regs.IY.B.l); 
}

static void or_iyh(void) {
    OR(z80.regs.IY.B.h); 
}

static void or_byte(void) {
    OR(readOpcode(z80.regs.PC.W++)); 
}

static void or_xhl(void) { 
    OR(readMem(z80.regs.HL.W));
}

static void or_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    OR(readMem(addr));
    z80.regs.SH.W = addr;
}

static void or_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    OR(readMem(addr));
    z80.regs.SH.W = addr;
}

static void xor_a(void) { 
    XOR(z80.regs.AF.B.h); 
}

static void xor_b(void) {
    XOR(z80.regs.BC.B.h); 
}

static void xor_c(void) { 
    XOR(z80.regs.BC.B.l); 
}

static void xor_d(void) { 
    XOR(z80.regs.DE.B.h);
}

static void xor_e(void) {
    XOR(z80.regs.DE.B.l);
}

static void xor_h(void) {
    XOR(z80.regs.HL.B.h);
}

static void xor_l(void) {
    XOR(z80.regs.HL.B.l);
}

static void xor_ixl(void) {
    XOR(z80.regs.IX.B.l); 
}

static void xor_ixh(void) { 
    XOR(z80.regs.IX.B.h); 
}

static void xor_iyl(void) {
    XOR(z80.regs.IY.B.l); 
}

static void xor_iyh(void) { 
    XOR(z80.regs.IY.B.h);
}

static void xor_byte(void) {
    XOR(readOpcode(z80.regs.PC.W++));
}

static void xor_xhl(void) {
    XOR(readMem(z80.regs.HL.W));
}

static void xor_xix(void) {
    UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    XOR(readMem(addr));
    z80.regs.SH.W = addr;
}

static void xor_xiy(void) {
    UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    DELAY_ADD8; 
    XOR(readMem(addr));
    z80.regs.SH.W = addr;
}

static void add_hl_bc(void) {
    ADDW(&z80.regs.HL.W, z80.regs.BC.W);
}

static void add_hl_de(void) {
    ADDW(&z80.regs.HL.W, z80.regs.DE.W);
}

static void add_hl_hl(void) {
    ADDW(&z80.regs.HL.W, z80.regs.HL.W);
}

static void add_hl_sp(void) {
    ADDW(&z80.regs.HL.W, z80.regs.SP.W);
}

static void add_ix_bc(void) {
    ADDW(&z80.regs.IX.W, z80.regs.BC.W);
}

static void add_ix_de(void) {
    ADDW(&z80.regs.IX.W, z80.regs.DE.W);
}

static void add_ix_ix(void) {
    ADDW(&z80.regs.IX.W, z80.regs.IX.W);
}

static void add_ix_sp(void) {
    ADDW(&z80.regs.IX.W, z80.regs.SP.W);
}

static void add_iy_bc(void) {
    ADDW(&z80.regs.IY.W, z80.regs.BC.W);
}

static void add_iy_de(void) {
    ADDW(&z80.regs.IY.W, z80.regs.DE.W);
}

static void add_iy_iy(void) {
    ADDW(&z80.regs.IY.W, z80.regs.IY.W);
}

static void add_iy_sp(void) {
    ADDW(&z80.regs.IY.W, z80.regs.SP.W);
}

static void mulu_xhl(void) { 
}

static void mulu_a(void) { 
}

static void mulu_b(void) { 
}

static void mulu_c(void) {
}

static void mulu_d(void) {
}

static void mulu_e(void) {
}

static void mulu_h(void) { 
}

static void mulu_l(void) { 
}

static void muluw_bc(void) { 
}

static void muluw_de(void) {
}

static void muluw_hl(void) {
}

static void muluw_sp(void) {
}

static void sla_a(void) { 
    SLA(&z80.regs.AF.B.h); 
}

static void sla_b(void) { 
    SLA(&z80.regs.BC.B.h); 
}

static void sla_c(void) {
    SLA(&z80.regs.BC.B.l); 
}

static void sla_d(void) {
    SLA(&z80.regs.DE.B.h);
}

static void sla_e(void) { 
    SLA(&z80.regs.DE.B.l); 
}

static void sla_h(void) {
    SLA(&z80.regs.HL.B.h);
}

static void sla_l(void) {
    SLA(&z80.regs.HL.B.l); 
}

static void sla_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    SLA(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 sla_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SLA(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void sla_xnn(UInt16 addr) {
    sla_xnn_v(addr);
}

static void sla_xnn_a(UInt16 addr) { 
    z80.regs.AF.B.h = sla_xnn_v(addr);
}

static void sla_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = sla_xnn_v(addr);
}

static void sla_xnn_c(UInt16 addr) { 
    z80.regs.BC.B.l = sla_xnn_v(addr);
}

static void sla_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = sla_xnn_v(addr);
}

static void sla_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = sla_xnn_v(addr);
}

static void sla_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = sla_xnn_v(addr);
}

static void sla_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = sla_xnn_v(addr);
}

static void sll_a(void) {
    SLL(&z80.regs.AF.B.h); 
}

static void sll_b(void) {
    SLL(&z80.regs.BC.B.h); 
}

static void sll_c(void) {
    SLL(&z80.regs.BC.B.l);
}

static void sll_d(void) {
    SLL(&z80.regs.DE.B.h);
}

static void sll_e(void) {
    SLL(&z80.regs.DE.B.l);
}

static void sll_h(void) {
    SLL(&z80.regs.HL.B.h);
}

static void sll_l(void) {
    SLL(&z80.regs.HL.B.l); 
}

static void sll_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    SLL(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 sll_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SLL(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void sll_xnn(UInt16 addr) { 
    sll_xnn_v(addr);
}

static void sll_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = sll_xnn_v(addr);
}

static void sll_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = sll_xnn_v(addr);
}

static void sll_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = sll_xnn_v(addr);
}

static void sll_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = sll_xnn_v(addr);
}

static void sll_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = sll_xnn_v(addr);
}

static void sll_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = sll_xnn_v(addr);
}

static void sll_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = sll_xnn_v(addr); 
}

static void sra_a(void) {    
    SRA(&z80.regs.AF.B.h);
}

static void sra_b(void) { 
    SRA(&z80.regs.BC.B.h);
}

static void sra_c(void) { 
    SRA(&z80.regs.BC.B.l);
}

static void sra_d(void) { 
    SRA(&z80.regs.DE.B.h);
}

static void sra_e(void) { 
    SRA(&z80.regs.DE.B.l); 
}

static void sra_h(void) {
    SRA(&z80.regs.HL.B.h);
}

static void sra_l(void) {
    SRA(&z80.regs.HL.B.l); 
}

static void sra_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    SRA(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 sra_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SRA(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void sra_xnn(UInt16 addr) {
    sra_xnn_v(addr); 
}

static void sra_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = sra_xnn_v(addr); 
}

static void sra_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = sra_xnn_v(addr); 
}

static void sra_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = sra_xnn_v(addr); 
}

static void sra_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = sra_xnn_v(addr); 
}

static void sra_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = sra_xnn_v(addr); 
}

static void sra_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = sra_xnn_v(addr); 
}

static void sra_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = sra_xnn_v(addr);
}

static void srl_a(void) { 
    SRL(&z80.regs.AF.B.h); 
}

static void srl_b(void) {
    SRL(&z80.regs.BC.B.h); 
}

static void srl_c(void) { 
    SRL(&z80.regs.BC.B.l); 
}

static void srl_d(void) {
    SRL(&z80.regs.DE.B.h);
}

static void srl_e(void) {
    SRL(&z80.regs.DE.B.l); 
}

static void srl_h(void) {
    SRL(&z80.regs.HL.B.h); 
}

static void srl_l(void) {
    SRL(&z80.regs.HL.B.l); 
}

static void srl_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    SRL(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 srl_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SRL(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void srl_xnn(UInt16 addr) {
    srl_xnn_v(addr);
}

static void srl_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = srl_xnn_v(addr);
}

static void srl_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = srl_xnn_v(addr); 
}

static void srl_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = srl_xnn_v(addr); 
}

static void srl_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = srl_xnn_v(addr);
}

static void srl_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = srl_xnn_v(addr); 
}

static void srl_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = srl_xnn_v(addr);
}

static void srl_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = srl_xnn_v(addr);
}

static void rl_a(void) {
    RL(&z80.regs.AF.B.h);
}

static void rl_b(void) {
    RL(&z80.regs.BC.B.h);
}

static void rl_c(void) { 
    RL(&z80.regs.BC.B.l); 
}

static void rl_d(void) {
    RL(&z80.regs.DE.B.h);
}

static void rl_e(void) { 
    RL(&z80.regs.DE.B.l);
}

static void rl_h(void) {
    RL(&z80.regs.HL.B.h);
}

static void rl_l(void) { 
    RL(&z80.regs.HL.B.l);
}

static void rl_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    RL(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 rl_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RL(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void rl_xnn(UInt16 addr) {
    rl_xnn_v(addr);
}

static void rl_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = rl_xnn_v(addr);
}

static void rl_xnn_b(UInt16 addr) { 
    z80.regs.BC.B.h = rl_xnn_v(addr); 
}

static void rl_xnn_c(UInt16 addr) { 
    z80.regs.BC.B.l = rl_xnn_v(addr); 
}

static void rl_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = rl_xnn_v(addr);
}

static void rl_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = rl_xnn_v(addr);
}

static void rl_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = rl_xnn_v(addr);
}

static void rl_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = rl_xnn_v(addr);
}

static void rlc_a(void) {
    RLC(&z80.regs.AF.B.h);
}

static void rlc_b(void) {
    RLC(&z80.regs.BC.B.h);
}

static void rlc_c(void) { 
    RLC(&z80.regs.BC.B.l);
}

static void rlc_d(void) {
    RLC(&z80.regs.DE.B.h);
}

static void rlc_e(void) { 
    RLC(&z80.regs.DE.B.l);
}

static void rlc_h(void) {
    RLC(&z80.regs.HL.B.h);
}

static void rlc_l(void) { 
    RLC(&z80.regs.HL.B.l);
}

static void rlc_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    RLC(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 rlc_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RLC(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void rlc_xnn(UInt16 addr) { 
    rlc_xnn_v(addr);
}

static void rlc_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = rlc_xnn_v(addr);
}

static void rlc_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = rlc_xnn_v(addr);
}

static void rlc_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = rlc_xnn_v(addr); 
}

static void rlc_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = rlc_xnn_v(addr); 
}

static void rlc_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = rlc_xnn_v(addr); 
}

static void rlc_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = rlc_xnn_v(addr);
}

static void rlc_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = rlc_xnn_v(addr); 
}

static void rr_a(void) {
    RR(&z80.regs.AF.B.h);
}

static void rr_b(void) {
    RR(&z80.regs.BC.B.h); 
}

static void rr_c(void) {
    RR(&z80.regs.BC.B.l);
}

static void rr_d(void) {
    RR(&z80.regs.DE.B.h);
}

static void rr_e(void) {
    RR(&z80.regs.DE.B.l);
}

static void rr_h(void) {
    RR(&z80.regs.HL.B.h);
}

static void rr_l(void) { 
    RR(&z80.regs.HL.B.l);
}

static void rr_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    RR(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 rr_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RR(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void rr_xnn(UInt16 addr) {
    rr_xnn_v(addr);
}

static void rr_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = rr_xnn_v(addr);
}

static void rr_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = rr_xnn_v(addr);
}

static void rr_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = rr_xnn_v(addr);
}

static void rr_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = rr_xnn_v(addr);
}

static void rr_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = rr_xnn_v(addr); 
}

static void rr_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = rr_xnn_v(addr);
}

static void rr_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = rr_xnn_v(addr);
}

static void rrc_a(void) {
    RRC(&z80.regs.AF.B.h);
}

static void rrc_b(void) {
    RRC(&z80.regs.BC.B.h);
}

static void rrc_c(void) {
    RRC(&z80.regs.BC.B.l);
}

static void rrc_d(void) {
    RRC(&z80.regs.DE.B.h);
}

static void rrc_e(void) {
    RRC(&z80.regs.DE.B.l);
}

static void rrc_h(void) { 
    RRC(&z80.regs.HL.B.h);
}

static void rrc_l(void) { 
    RRC(&z80.regs.HL.B.l); 
}

static void rrc_xhl(void) { 
    UInt8 val = readMem(z80.regs.HL.W);
    RRC(&val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 rrc_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RRC(&val);
    DELAY_BIT;                 // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void rrc_xnn(UInt16 addr) {
    rrc_xnn_v(addr);
}

static void rrc_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = rrc_xnn_v(addr); 
}

static void rrc_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = rrc_xnn_v(addr);
}

static void rrc_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = rrc_xnn_v(addr);
}

static void rrc_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = rrc_xnn_v(addr);
}

static void rrc_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = rrc_xnn_v(addr);
}

static void rrc_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = rrc_xnn_v(addr);
}

static void rrc_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = rrc_xnn_v(addr);
}

static void bit_0_a(void) { 
    BIT(0, z80.regs.AF.B.h);
}

static void bit_0_b(void) {
    BIT(0, z80.regs.BC.B.h);
}

static void bit_0_c(void) {
    BIT(0, z80.regs.BC.B.l);
}

static void bit_0_d(void) {
    BIT(0, z80.regs.DE.B.h); 
}

static void bit_0_e(void) {
    BIT(0, z80.regs.DE.B.l);
}

static void bit_0_h(void) { 
    BIT(0, z80.regs.HL.B.h);
}

static void bit_0_l(void) {
    BIT(0, z80.regs.HL.B.l); 
}

static void bit_0_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 0)];
}

static void bit_0_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 0)];
}

static void bit_1_a(void) {
    BIT(1, z80.regs.AF.B.h);
}

static void bit_1_b(void) {
    BIT(1, z80.regs.BC.B.h);
}

static void bit_1_c(void) {
    BIT(1, z80.regs.BC.B.l);
}

static void bit_1_d(void) {
    BIT(1, z80.regs.DE.B.h);
}

static void bit_1_e(void) {
    BIT(1, z80.regs.DE.B.l);
}

static void bit_1_h(void) {
    BIT(1, z80.regs.HL.B.h);
}

static void bit_1_l(void) {
    BIT(1, z80.regs.HL.B.l);
}

static void bit_1_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 1)];
}

static void bit_1_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 1)];
}

static void bit_2_a(void) {
    BIT(2, z80.regs.AF.B.h); 
}

static void bit_2_b(void) {
    BIT(2, z80.regs.BC.B.h);
}

static void bit_2_c(void) {
    BIT(2, z80.regs.BC.B.l);
}

static void bit_2_d(void) {
    BIT(2, z80.regs.DE.B.h);
}

static void bit_2_e(void) {
    BIT(2, z80.regs.DE.B.l);
}

static void bit_2_h(void) {
    BIT(2, z80.regs.HL.B.h);
}

static void bit_2_l(void) { 
    BIT(2, z80.regs.HL.B.l);
}

static void bit_2_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 2)];
}

static void bit_2_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 2)];
}

static void bit_3_a(void) {
    BIT(3, z80.regs.AF.B.h);
}

static void bit_3_b(void) {
    BIT(3, z80.regs.BC.B.h);
}

static void bit_3_c(void) { 
    BIT(3, z80.regs.BC.B.l);
}

static void bit_3_d(void) {
    BIT(3, z80.regs.DE.B.h); 
}

static void bit_3_e(void) {
    BIT(3, z80.regs.DE.B.l);
}

static void bit_3_h(void) { 
    BIT(3, z80.regs.HL.B.h);
}

static void bit_3_l(void) { 
    BIT(3, z80.regs.HL.B.l);
}

static void bit_3_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 3)];
}

static void bit_3_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 3)];
}

static void bit_4_a(void) {
    BIT(4, z80.regs.AF.B.h);
}

static void bit_4_b(void) { 
    BIT(4, z80.regs.BC.B.h);
}

static void bit_4_c(void) { 
    BIT(4, z80.regs.BC.B.l);
}

static void bit_4_d(void) {
    BIT(4, z80.regs.DE.B.h);
}

static void bit_4_e(void) {
    BIT(4, z80.regs.DE.B.l);
}

static void bit_4_h(void) {
    BIT(4, z80.regs.HL.B.h);
}

static void bit_4_l(void) { 
    BIT(4, z80.regs.HL.B.l);
}

static void bit_4_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 4)];
}

static void bit_4_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 4)];
}

static void bit_5_a(void) {
    BIT(5, z80.regs.AF.B.h); 
}

static void bit_5_b(void) {
    BIT(5, z80.regs.BC.B.h); 
}

static void bit_5_c(void) { 
    BIT(5, z80.regs.BC.B.l); 
}

static void bit_5_d(void) {
    BIT(5, z80.regs.DE.B.h); 
}

static void bit_5_e(void) {
    BIT(5, z80.regs.DE.B.l); 
}

static void bit_5_h(void) {
    BIT(5, z80.regs.HL.B.h);
}

static void bit_5_l(void) { 
    BIT(5, z80.regs.HL.B.l); 
}

static void bit_5_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 5)];
}

static void bit_5_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 5)];
}

static void bit_6_a(void) { 
    BIT(6, z80.regs.AF.B.h);
}

static void bit_6_b(void) {
    BIT(6, z80.regs.BC.B.h);
}

static void bit_6_c(void) {
    BIT(6, z80.regs.BC.B.l); 
}

static void bit_6_d(void) { 
    BIT(6, z80.regs.DE.B.h);
}

static void bit_6_e(void) {
    BIT(6, z80.regs.DE.B.l);
}

static void bit_6_h(void) {
    BIT(6, z80.regs.HL.B.h);
}

static void bit_6_l(void) {
    BIT(6, z80.regs.HL.B.l);
}

static void bit_6_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 6)];
}

static void bit_6_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 6)];
}

static void bit_7_a(void) { 
    BIT(7, z80.regs.AF.B.h); 
}

static void bit_7_b(void) { 
    BIT(7, z80.regs.BC.B.h);
}

static void bit_7_c(void) {
    BIT(7, z80.regs.BC.B.l);
}

static void bit_7_d(void) {
    BIT(7, z80.regs.DE.B.h);
}

static void bit_7_e(void) {
    BIT(7, z80.regs.DE.B.l); 
}

static void bit_7_h(void) {
    BIT(7, z80.regs.HL.B.h);
}

static void bit_7_l(void) {
    BIT(7, z80.regs.HL.B.l); 
}

static void bit_7_xhl(void) {
    DELAY_BIT;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) | 
        ZSPHTable[readMem(z80.regs.HL.W) & (1 << 7)];
}

static void bit_7_xnn(UInt16 addr) { 
    DELAY_BITIX;           // white cat append
    z80.regs.SH.W   = addr;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        (z80.regs.SH.B.h & (X_FLAG | Y_FLAG)) |
        ZSPHTable[readMem(addr) & (1 << 7)];
}

static void res_0_a(void) {
    RES(0, &z80.regs.AF.B.h);
}

static void res_0_b(void) {
    RES(0, &z80.regs.BC.B.h);
}

static void res_0_c(void) {
    RES(0, &z80.regs.BC.B.l);
}

static void res_0_d(void) {
    RES(0, &z80.regs.DE.B.h);
}

static void res_0_e(void) {
    RES(0, &z80.regs.DE.B.l);
}

static void res_0_h(void) {
    RES(0, &z80.regs.HL.B.h);
}

static void res_0_l(void) {
    RES(0, &z80.regs.HL.B.l);
}

static void res_0_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(0, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_0_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(0, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_0_xnn  (UInt16 addr) {
    res_0_xnn_v(addr);
}

static void res_0_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = res_0_xnn_v(addr);
}

static void res_0_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = res_0_xnn_v(addr);
}

static void res_0_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = res_0_xnn_v(addr);
}

static void res_0_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = res_0_xnn_v(addr);
}

static void res_0_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = res_0_xnn_v(addr);
}

static void res_0_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = res_0_xnn_v(addr);
}

static void res_0_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = res_0_xnn_v(addr); 
}

static void res_1_a(void) { 
    RES(1, &z80.regs.AF.B.h);
}

static void res_1_b(void) {
    RES(1, &z80.regs.BC.B.h);
}

static void res_1_c(void) {
    RES(1, &z80.regs.BC.B.l); 
}

static void res_1_d(void) {
    RES(1, &z80.regs.DE.B.h); 
}

static void res_1_e(void) {
    RES(1, &z80.regs.DE.B.l); 
}

static void res_1_h(void) {
    RES(1, &z80.regs.HL.B.h);
}

static void res_1_l(void) {
    RES(1, &z80.regs.HL.B.l); 
}

static void res_1_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(1, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_1_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(1, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_1_xnn  (UInt16 addr) {
    res_1_xnn_v(addr); 
}

static void res_1_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = res_1_xnn_v(addr);
}

static void res_1_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = res_1_xnn_v(addr);
}

static void res_1_xnn_c(UInt16 addr) { 
    z80.regs.BC.B.l = res_1_xnn_v(addr); 
}

static void res_1_xnn_d(UInt16 addr) { 
    z80.regs.DE.B.h = res_1_xnn_v(addr);
}

static void res_1_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = res_1_xnn_v(addr);
}

static void res_1_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = res_1_xnn_v(addr);
}

static void res_1_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = res_1_xnn_v(addr);
}

static void res_2_a(void) {
    RES(2, &z80.regs.AF.B.h);
}

static void res_2_b(void) {
    RES(2, &z80.regs.BC.B.h);
}

static void res_2_c(void) {
    RES(2, &z80.regs.BC.B.l);
}

static void res_2_d(void) {
    RES(2, &z80.regs.DE.B.h);
}

static void res_2_e(void) { 
    RES(2, &z80.regs.DE.B.l); 
}

static void res_2_h(void) {
    RES(2, &z80.regs.HL.B.h); 
}

static void res_2_l(void) {
    RES(2, &z80.regs.HL.B.l);
}

static void res_2_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(2, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_2_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(2, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_2_xnn  (UInt16 addr) {
    res_2_xnn_v(addr); 
}

static void res_2_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = res_2_xnn_v(addr);
}

static void res_2_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = res_2_xnn_v(addr);
}

static void res_2_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = res_2_xnn_v(addr);
}

static void res_2_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = res_2_xnn_v(addr);
}

static void res_2_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = res_2_xnn_v(addr);
}

static void res_2_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = res_2_xnn_v(addr); 
}

static void res_2_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = res_2_xnn_v(addr);
}

static void res_3_a(void) {
    RES(3, &z80.regs.AF.B.h);
}

static void res_3_b(void) {
    RES(3, &z80.regs.BC.B.h);
}

static void res_3_c(void) { 
    RES(3, &z80.regs.BC.B.l);
}

static void res_3_d(void) {
    RES(3, &z80.regs.DE.B.h);
}

static void res_3_e(void) {
    RES(3, &z80.regs.DE.B.l);
}

static void res_3_h(void) {
    RES(3, &z80.regs.HL.B.h);
}

static void res_3_l(void) {
    RES(3, &z80.regs.HL.B.l);
}

static void res_3_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(3, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_3_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(3, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_3_xnn  (UInt16 addr) {
    res_3_xnn_v(addr);
}

static void res_3_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = res_3_xnn_v(addr); 
}

static void res_3_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = res_3_xnn_v(addr);
}

static void res_3_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = res_3_xnn_v(addr); 
}

static void res_3_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = res_3_xnn_v(addr);
}

static void res_3_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = res_3_xnn_v(addr);
}

static void res_3_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = res_3_xnn_v(addr); 
}

static void res_3_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = res_3_xnn_v(addr);
}

static void res_4_a(void) {
    RES(4, &z80.regs.AF.B.h);
}

static void res_4_b(void) {
    RES(4, &z80.regs.BC.B.h);
}

static void res_4_c(void) { 
    RES(4, &z80.regs.BC.B.l);
}

static void res_4_d(void) {
    RES(4, &z80.regs.DE.B.h);
}

static void res_4_e(void) {
    RES(4, &z80.regs.DE.B.l);
}

static void res_4_h(void) {
    RES(4, &z80.regs.HL.B.h);
}

static void res_4_l(void) {
    RES(4, &z80.regs.HL.B.l); 
}

static void res_4_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(4, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_4_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(4, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_4_xnn  (UInt16 addr) {
    res_4_xnn_v(addr); 
}

static void res_4_xnn_a(UInt16 addr) { 
    z80.regs.AF.B.h = res_4_xnn_v(addr); 
}

static void res_4_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = res_4_xnn_v(addr);
}

static void res_4_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = res_4_xnn_v(addr);
}

static void res_4_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = res_4_xnn_v(addr);
}

static void res_4_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = res_4_xnn_v(addr);
}

static void res_4_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = res_4_xnn_v(addr);
}

static void res_4_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = res_4_xnn_v(addr);
}

static void res_5_a(void) {
    RES(5, &z80.regs.AF.B.h);
}

static void res_5_b(void) {
    RES(5, &z80.regs.BC.B.h);
}

static void res_5_c(void) {
    RES(5, &z80.regs.BC.B.l);
}

static void res_5_d(void) {
    RES(5, &z80.regs.DE.B.h);
}

static void res_5_e(void) {
    RES(5, &z80.regs.DE.B.l);
}

static void res_5_h(void) {
    RES(5, &z80.regs.HL.B.h);
}

static void res_5_l(void) {
    RES(5, &z80.regs.HL.B.l);
}

static void res_5_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(5, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_5_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(5, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_5_xnn  (UInt16 addr) {
    res_5_xnn_v(addr);
}

static void res_5_xnn_a(UInt16 addr) { 
    z80.regs.AF.B.h = res_5_xnn_v(addr);
}

static void res_5_xnn_b(UInt16 addr) { 
    z80.regs.BC.B.h = res_5_xnn_v(addr);
}

static void res_5_xnn_c(UInt16 addr) { 
    z80.regs.BC.B.l = res_5_xnn_v(addr);
}

static void res_5_xnn_d(UInt16 addr) { 
    z80.regs.DE.B.h = res_5_xnn_v(addr);
}

static void res_5_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = res_5_xnn_v(addr);
}

static void res_5_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = res_5_xnn_v(addr);
}

static void res_5_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = res_5_xnn_v(addr);
}

static void res_6_a(void) {
    RES(6, &z80.regs.AF.B.h);
}

static void res_6_b(void) {
    RES(6, &z80.regs.BC.B.h);
}

static void res_6_c(void) {
    RES(6, &z80.regs.BC.B.l); 
}

static void res_6_d(void) { 
    RES(6, &z80.regs.DE.B.h);
}

static void res_6_e(void) { 
    RES(6, &z80.regs.DE.B.l);
}

static void res_6_h(void) { 
    RES(6, &z80.regs.HL.B.h);
}

static void res_6_l(void) { 
    RES(6, &z80.regs.HL.B.l);
}

static void res_6_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(6, &val); 
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_6_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(6, &val);
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_6_xnn  (UInt16 addr) {
    res_6_xnn_v(addr);
}

static void res_6_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = res_6_xnn_v(addr);
}

static void res_6_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = res_6_xnn_v(addr);
}

static void res_6_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = res_6_xnn_v(addr); 
}

static void res_6_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = res_6_xnn_v(addr);
}

static void res_6_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = res_6_xnn_v(addr);
}

static void res_6_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = res_6_xnn_v(addr);
}

static void res_6_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = res_6_xnn_v(addr);
}

static void res_7_a(void) {
    RES(7, &z80.regs.AF.B.h);
}

static void res_7_b(void) {
    RES(7, &z80.regs.BC.B.h);
}

static void res_7_c(void) {
    RES(7, &z80.regs.BC.B.l);
}

static void res_7_d(void) { 
    RES(7, &z80.regs.DE.B.h);
}

static void res_7_e(void) {
    RES(7, &z80.regs.DE.B.l);
}

static void res_7_h(void) {
    RES(7, &z80.regs.HL.B.h);
}

static void res_7_l(void) {
    RES(7, &z80.regs.HL.B.l); 
}

static void res_7_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    RES(7, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 res_7_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    RES(7, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void res_7_xnn  (UInt16 addr) {
    res_7_xnn_v(addr); 
}

static void res_7_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = res_7_xnn_v(addr);
}

static void res_7_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = res_7_xnn_v(addr); 
}

static void res_7_xnn_c(UInt16 addr) { 
    z80.regs.BC.B.l = res_7_xnn_v(addr); 
}

static void res_7_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = res_7_xnn_v(addr);
}

static void res_7_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = res_7_xnn_v(addr); 
}

static void res_7_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = res_7_xnn_v(addr); 
}

static void res_7_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = res_7_xnn_v(addr); 
}

static void set_0_a(void) {
    SET(0, &z80.regs.AF.B.h); 
}

static void set_0_b(void) {
    SET(0, &z80.regs.BC.B.h);
}

static void set_0_c(void) {
    SET(0, &z80.regs.BC.B.l);
}

static void set_0_d(void) {
    SET(0, &z80.regs.DE.B.h);
}

static void set_0_e(void) { 
    SET(0, &z80.regs.DE.B.l);
}

static void set_0_h(void) { 
    SET(0, &z80.regs.HL.B.h);
}

static void set_0_l(void) {
    SET(0, &z80.regs.HL.B.l);
}

static void set_0_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(0, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_0_xnn_v(UInt16 addr) {
    
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    
    SET(0, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    
    writeMem(addr, val);
    return val;
}

static void set_0_xnn  (UInt16 addr) { 
    set_0_xnn_v(addr);
}

static void set_0_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = set_0_xnn_v(addr);
}

static void set_0_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_0_xnn_v(addr);
}

static void set_0_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = set_0_xnn_v(addr);
}

static void set_0_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = set_0_xnn_v(addr);
}

static void set_0_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = set_0_xnn_v(addr);
}

static void set_0_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = set_0_xnn_v(addr); 
}

static void set_0_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = set_0_xnn_v(addr);
}

static void set_1_a(void) {
    SET(1, &z80.regs.AF.B.h);
}

static void set_1_b(void) {
    SET(1, &z80.regs.BC.B.h);
}

static void set_1_c(void) {
    SET(1, &z80.regs.BC.B.l);
}

static void set_1_d(void) {
    SET(1, &z80.regs.DE.B.h);
}

static void set_1_e(void) {
    SET(1, &z80.regs.DE.B.l);
}

static void set_1_h(void) {
    SET(1, &z80.regs.HL.B.h);
}

static void set_1_l(void) { 
    SET(1, &z80.regs.HL.B.l);
}

static void set_1_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(1, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_1_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SET(1, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void set_1_xnn  (UInt16 addr) { 
    set_1_xnn_v(addr);
}

static void set_1_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = set_1_xnn_v(addr); 
}

static void set_1_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_1_xnn_v(addr);
}

static void set_1_xnn_c(UInt16 addr) { 
    z80.regs.BC.B.l = set_1_xnn_v(addr);
}

static void set_1_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = set_1_xnn_v(addr);
}

static void set_1_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = set_1_xnn_v(addr);
}

static void set_1_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = set_1_xnn_v(addr);
}

static void set_1_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = set_1_xnn_v(addr);
}

static void set_2_a(void) { 
    SET(2, &z80.regs.AF.B.h);
}

static void set_2_b(void) {
    SET(2, &z80.regs.BC.B.h); 
}

static void set_2_c(void) {
    SET(2, &z80.regs.BC.B.l); 
}

static void set_2_d(void) { 
    SET(2, &z80.regs.DE.B.h);
}

static void set_2_e(void) { 
    SET(2, &z80.regs.DE.B.l);
}

static void set_2_h(void) {
    SET(2, &z80.regs.HL.B.h);
}

static void set_2_l(void) {
    SET(2, &z80.regs.HL.B.l);
}

static void set_2_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(2, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_2_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SET(2, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void set_2_xnn  (UInt16 addr) {
    set_2_xnn_v(addr);
}

static void set_2_xnn_a(UInt16 addr) { 
    z80.regs.AF.B.h = set_2_xnn_v(addr); 
}

static void set_2_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_2_xnn_v(addr); 
}

static void set_2_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = set_2_xnn_v(addr);
}

static void set_2_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = set_2_xnn_v(addr);
}

static void set_2_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = set_2_xnn_v(addr);
}

static void set_2_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = set_2_xnn_v(addr); 
}

static void set_2_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = set_2_xnn_v(addr); 
}

static void set_3_a(void) {
    SET(3, &z80.regs.AF.B.h);
}

static void set_3_b(void) {
    SET(3, &z80.regs.BC.B.h);
}

static void set_3_c(void) {
    SET(3, &z80.regs.BC.B.l);
}

static void set_3_d(void) { 
    SET(3, &z80.regs.DE.B.h);
}

static void set_3_e(void) { 
    SET(3, &z80.regs.DE.B.l);
}

static void set_3_h(void) { 
    SET(3, &z80.regs.HL.B.h);
}

static void set_3_l(void) { 
    SET(3, &z80.regs.HL.B.l);
}

static void set_3_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(3, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_3_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SET(3, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void set_3_xnn  (UInt16 addr) {
    set_3_xnn_v(addr);
}

static void set_3_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = set_3_xnn_v(addr); 
}

static void set_3_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_3_xnn_v(addr);
}

static void set_3_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = set_3_xnn_v(addr);
}

static void set_3_xnn_d(UInt16 addr) { 
    z80.regs.DE.B.h = set_3_xnn_v(addr);
}

static void set_3_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = set_3_xnn_v(addr);
}

static void set_3_xnn_h(UInt16 addr) { 
    z80.regs.HL.B.h = set_3_xnn_v(addr);
}

static void set_3_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = set_3_xnn_v(addr);
}

static void set_4_a(void) {
    SET(4, &z80.regs.AF.B.h);
}

static void set_4_b(void) {
    SET(4, &z80.regs.BC.B.h);
}

static void set_4_c(void) {
    SET(4, &z80.regs.BC.B.l);
}

static void set_4_d(void) {
    SET(4, &z80.regs.DE.B.h);
}

static void set_4_e(void) {
    SET(4, &z80.regs.DE.B.l);
}

static void set_4_h(void) {
    SET(4, &z80.regs.HL.B.h); 
}

static void set_4_l(void) {
    SET(4, &z80.regs.HL.B.l); 
}

static void set_4_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(4, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_4_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SET(4, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void set_4_xnn  (UInt16 addr) {
    set_4_xnn_v(addr);
}

static void set_4_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = set_4_xnn_v(addr);
}

static void set_4_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_4_xnn_v(addr);
}

static void set_4_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = set_4_xnn_v(addr);
}

static void set_4_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = set_4_xnn_v(addr);
}

static void set_4_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = set_4_xnn_v(addr);
}

static void set_4_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = set_4_xnn_v(addr);
}

static void set_4_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = set_4_xnn_v(addr);
}

static void set_5_a(void) { 
    SET(5, &z80.regs.AF.B.h); 
}

static void set_5_b(void) {
    SET(5, &z80.regs.BC.B.h);
}

static void set_5_c(void) {
    SET(5, &z80.regs.BC.B.l);
}

static void set_5_d(void) { 
    SET(5, &z80.regs.DE.B.h);
}

static void set_5_e(void) { 
    SET(5, &z80.regs.DE.B.l); 
}

static void set_5_h(void) { 
    SET(5, &z80.regs.HL.B.h); 
}

static void set_5_l(void) { 
    SET(5, &z80.regs.HL.B.l);
}

static void set_5_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(5, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_5_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SET(5, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void set_5_xnn  (UInt16 addr) {
    set_5_xnn_v(addr);
}

static void set_5_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = set_5_xnn_v(addr);
}

static void set_5_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_5_xnn_v(addr);
}

static void set_5_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = set_5_xnn_v(addr);
}

static void set_5_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = set_5_xnn_v(addr);
}

static void set_5_xnn_e(UInt16 addr) { 
    z80.regs.DE.B.l = set_5_xnn_v(addr);
}

static void set_5_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = set_5_xnn_v(addr);
}

static void set_5_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = set_5_xnn_v(addr);
}

static void set_6_a(void) {
    SET(6, &z80.regs.AF.B.h);
}

static void set_6_b(void) {
    SET(6, &z80.regs.BC.B.h);
}

static void set_6_c(void) {
    SET(6, &z80.regs.BC.B.l);
}

static void set_6_d(void) {
    SET(6, &z80.regs.DE.B.h);
}

static void set_6_e(void) {
    SET(6, &z80.regs.DE.B.l); 
}

static void set_6_h(void) {
    SET(6, &z80.regs.HL.B.h);
}

static void set_6_l(void) { 
    SET(6, &z80.regs.HL.B.l); 
}

static void set_6_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(6, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_6_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SET(6, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void set_6_xnn  (UInt16 addr) {
    set_6_xnn_v(addr); 
}

static void set_6_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = set_6_xnn_v(addr); 
}

static void set_6_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_6_xnn_v(addr);
}

static void set_6_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = set_6_xnn_v(addr);
}

static void set_6_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = set_6_xnn_v(addr);
}

static void set_6_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = set_6_xnn_v(addr); 
}

static void set_6_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = set_6_xnn_v(addr);
}

static void set_6_xnn_l(UInt16 addr) { 
    z80.regs.HL.B.l = set_6_xnn_v(addr);
}

static void set_7_a(void) {
    SET(7, &z80.regs.AF.B.h); 
}

static void set_7_b(void) {
    SET(7, &z80.regs.BC.B.h); 
}

static void set_7_c(void) {
    SET(7, &z80.regs.BC.B.l); 
}

static void set_7_d(void) {
    SET(7, &z80.regs.DE.B.h); 
}

static void set_7_e(void) {
    SET(7, &z80.regs.DE.B.l);
}

static void set_7_h(void) {
    SET(7, &z80.regs.HL.B.h);
}

static void set_7_l(void) {
    SET(7, &z80.regs.HL.B.l); 
}

static void set_7_xhl(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    SET(7, &val); 
    DELAY_INC;
    writeMem(z80.regs.HL.W, val);
}

static UInt8 set_7_xnn_v(UInt16 addr) {
    UInt8 val = readMem(addr);
    z80.regs.SH.W = addr;
    SET(7, &val);
    DELAY_BIT;             // white cat append
    DELAY_INC;
    writeMem(addr, val);
    return val;
}

static void set_7_xnn  (UInt16 addr) {
    set_7_xnn_v(addr);
}

static void set_7_xnn_a(UInt16 addr) {
    z80.regs.AF.B.h = set_7_xnn_v(addr);
}

static void set_7_xnn_b(UInt16 addr) {
    z80.regs.BC.B.h = set_7_xnn_v(addr);
}

static void set_7_xnn_c(UInt16 addr) {
    z80.regs.BC.B.l = set_7_xnn_v(addr);
}

static void set_7_xnn_d(UInt16 addr) {
    z80.regs.DE.B.h = set_7_xnn_v(addr);
}

static void set_7_xnn_e(UInt16 addr) {
    z80.regs.DE.B.l = set_7_xnn_v(addr); 
}

static void set_7_xnn_h(UInt16 addr) {
    z80.regs.HL.B.h = set_7_xnn_v(addr);
}

static void set_7_xnn_l(UInt16 addr) {
    z80.regs.HL.B.l = set_7_xnn_v(addr);
}

static void ex_af_af(void) {
    UInt16 regVal = z80.regs.AF.W;
    z80.regs.AF.W = z80.regs.AF1.W;
    z80.regs.AF1.W = regVal;
}

static void djnz(void) {
    DELAY_DJNZ;
    JR_COND(--z80.regs.BC.B.h != 0);
}

static void jr(void) {
    JR_COND(1);
}

static void jr_z(void) {
    JR_COND(z80.regs.AF.B.l & Z_FLAG);
}

static void jr_nz(void) {
    JR_COND(!(z80.regs.AF.B.l & Z_FLAG));
}

static void jr_c(void) {
    JR_COND(z80.regs.AF.B.l & C_FLAG);
}

static void jr_nc(void) {
    JR_COND(!(z80.regs.AF.B.l & C_FLAG));
}

static void jp(void) {
    JP_COND(1);
}

static void jp_hl(void) { 
    z80.regs.PC.W = z80.regs.HL.W; 
}

static void jp_ix(void) { 
    z80.regs.PC.W = z80.regs.IX.W; 
}

static void jp_iy(void) { 
    z80.regs.PC.W = z80.regs.IY.W; 
}

static void jp_z(void) {
    JP_COND(z80.regs.AF.B.l & Z_FLAG);
}

static void jp_nz(void) {
    JP_COND(!(z80.regs.AF.B.l & Z_FLAG));
}

static void jp_c(void) {
    JP_COND(z80.regs.AF.B.l & C_FLAG);
}

static void jp_nc(void) {
    JP_COND(!(z80.regs.AF.B.l & C_FLAG));
}

static void jp_m(void) {
    JP_COND(z80.regs.AF.B.l & S_FLAG);
}

static void jp_p(void) {
    JP_COND(!(z80.regs.AF.B.l & S_FLAG));
}

static void jp_pe(void) {
    JP_COND(z80.regs.AF.B.l & V_FLAG);
}

static void jp_po(void) {
    JP_COND(!(z80.regs.AF.B.l & V_FLAG));
}

static void call(void) {
    CALL_COND(1);
}

static void call_z(void) {
    CALL_COND(z80.regs.AF.B.l & Z_FLAG);
}

static void call_nz(void) {
    CALL_COND(!(z80.regs.AF.B.l & Z_FLAG));
}

static void call_c(void) {
    CALL_COND(z80.regs.AF.B.l & C_FLAG);
}

static void call_nc(void) {
    CALL_COND(!(z80.regs.AF.B.l & C_FLAG));
}

static void call_m(void) {
    CALL_COND(z80.regs.AF.B.l & S_FLAG);
}

static void call_p(void) {
    CALL_COND(!(z80.regs.AF.B.l & S_FLAG));
}

static void call_pe(void) {
    CALL_COND(z80.regs.AF.B.l & V_FLAG);
}

static void call_po(void) {
    CALL_COND(!(z80.regs.AF.B.l & V_FLAG));
}

static void ret(void) {
    RegisterPair addr;
    addr.B.l = readMem(z80.regs.SP.W++);
    addr.B.h = readMem(z80.regs.SP.W++);
    z80.regs.PC.W = addr.W;
    z80.regs.SH.W = addr.W;
}

static void ret_c(void) {
    DELAY_RET;
    if (z80.regs.AF.B.l & C_FLAG) {
        ret();
    }
}

static void ret_nc(void) {
    DELAY_RET;
    if (!(z80.regs.AF.B.l & C_FLAG)) {
        ret();
    }
}

static void ret_z(void) {
    DELAY_RET;
    if (z80.regs.AF.B.l & Z_FLAG) {
        ret();
    }
}

static void ret_nz(void) {
    DELAY_RET;
    if (!(z80.regs.AF.B.l & Z_FLAG)) {
        ret();
    }
}

static void ret_m(void) {
    DELAY_RET;
    if (z80.regs.AF.B.l & S_FLAG) {
        ret();
    }
}

static void ret_p(void) {
    DELAY_RET;
    if (!(z80.regs.AF.B.l & S_FLAG)) {
        ret();
    }
}

static void ret_pe(void) {
    DELAY_RET;
    if (z80.regs.AF.B.l & V_FLAG) {
        ret();
    }
}

static void ret_po(void) {
    DELAY_RET;
    if (!(z80.regs.AF.B.l & V_FLAG)) {
        ret();
    }
}

static void reti(void) {
    z80.regs.iff1 = z80.regs.iff2;
    ret();
    updateFastLoop();
}

static void retn(void) {
    z80.regs.iff1 = z80.regs.iff2;
    ret(); 
    updateFastLoop();
}

static void ex_xsp_hl(void) { 
    EX_SP(&z80.regs.HL.W);
}

static void ex_xsp_ix(void) { 
    EX_SP(&z80.regs.IX.W); 
}

static void ex_xsp_iy(void) { 

    EX_SP(&z80.regs.IY.W); 

}

static void ex_de_hl(void) {
    UInt16 tmp = z80.regs.DE.W;
    z80.regs.DE.W  = z80.regs.HL.W;
    z80.regs.HL.W  = tmp;
}


static void rlca(void) {
    UInt8 regVal = z80.regs.AF.B.h;
    z80.regs.AF.B.h = (regVal << 1) | (regVal >> 7);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | P_FLAG)) |
        (z80.regs.AF.B.h & (Y_FLAG | X_FLAG | C_FLAG));
}

static void rrca(void) {

    UInt8 regVal = z80.regs.AF.B.h;
    z80.regs.AF.B.h = (regVal >> 1) | (regVal << 7);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | P_FLAG)) | 
        (regVal &  C_FLAG) | (z80.regs.AF.B.h & (X_FLAG | Y_FLAG));
}

static void rla(void) {
    UInt8 regVal = z80.regs.AF.B.h;
    z80.regs.AF.B.h = (regVal << 1) | (z80.regs.AF.B.l & C_FLAG);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | P_FLAG)) |
        ((regVal >> 7) & C_FLAG) | (z80.regs.AF.B.h & (X_FLAG | Y_FLAG));
}

static void rra(void) {
    UInt8 regVal = z80.regs.AF.B.h;
    z80.regs.AF.B.h = (regVal >> 1) | ((z80.regs.AF.B.l & C_FLAG) << 7);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | P_FLAG)) |
        (regVal & C_FLAG) | (z80.regs.AF.B.h & (X_FLAG | Y_FLAG));
}

static void daa(void) {
    Int16 regVal = z80.regs.AF.B.l;
    z80.regs.AF.W = DAATable[(Int16)z80.regs.AF.B.h | ((regVal & 3) << 8) |
        ((regVal & 0x10) << 6)];
}

static void cpl(void) {
    z80.regs.AF.B.h ^= 0xff;
    z80.regs.AF.B.l = 
        (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | P_FLAG | C_FLAG)) |
        H_FLAG | N_FLAG | (z80.regs.AF.B.h & (X_FLAG | Y_FLAG));
}

static void scf(void) {
    z80.regs.AF.B.l = (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | P_FLAG)) |
        C_FLAG | (z80.regs.AF.B.h & (X_FLAG | Y_FLAG));
}

static void ccf(void) { //DIFF
    z80.regs.AF.B.l = 
        ((z80.regs.AF.B.l & (S_FLAG | Z_FLAG | P_FLAG | C_FLAG)) |
        ((z80.regs.AF.B.l & C_FLAG) << 4) |
        (z80.regs.AF.B.h & (X_FLAG | Y_FLAG))) ^ C_FLAG;
}

static void halt(void) {
    if ((z80.intState == INT_LOW && z80.regs.iff1) || (z80.nmiState == INT_EDGE)) {
		z80.regs.halt=0;
    }
	else {
		z80.regs.PC.W--;
		z80.regs.halt=1;
	}
    updateFastLoop();
}

static void push_af(void) {
    PUSH(&z80.regs.AF.W);
}

static void push_bc(void) {
    PUSH(&z80.regs.BC.W); 
}

static void push_de(void) {
    PUSH(&z80.regs.DE.W);
}

static void push_hl(void) {
    PUSH(&z80.regs.HL.W);
}

static void push_ix(void) {
    PUSH(&z80.regs.IX.W);
}

static void push_iy(void) { 
    PUSH(&z80.regs.IY.W);
}

static void pop_af(void) {
    POP(&z80.regs.AF.W);
}

static void pop_bc(void) {
    POP(&z80.regs.BC.W);
}

static void pop_de(void) {
    POP(&z80.regs.DE.W);
}

static void pop_hl(void) {
    POP(&z80.regs.HL.W); 
}

static void pop_ix(void) {
    POP(&z80.regs.IX.W); 
}

static void pop_iy(void) {
    POP(&z80.regs.IY.W);
}

static void rst_00(void) {
    RST(0x0000);
}
static void rst_08(void) {
    RST(0x0008);
}
static void rst_10(void) {
    RST(0x0010);
}
static void rst_18(void) {
    RST(0x0018);
}
static void rst_20(void) {
    RST(0x0020);
}
static void rst_28(void) {
    RST(0x0028);
}
static void rst_30(void) {
    RST(0x0030);
}
static void rst_38(void) {
    RST(0x0038);
}

static void out_byte_a(void) {
    RegisterPair port;
    port.B.l = readOpcode(z80.regs.PC.W++);
    port.B.h = z80.regs.AF.B.h;
    writePort(port.W, z80.regs.AF.B.h);
}

static void in_a_byte(void) {
    RegisterPair port;
    port.B.l = readOpcode(z80.regs.PC.W++);
    port.B.h = z80.regs.AF.B.h;
    z80.regs.AF.B.h = readPort(port.W);
}

static void exx(void) {
    UInt16 tmp;
    tmp        = z80.regs.BC.W; 
    z80.regs.BC.W  = z80.regs.BC1.W; 
    z80.regs.BC1.W = tmp;
    tmp        = z80.regs.DE.W; 
    z80.regs.DE.W  = z80.regs.DE1.W; 
    z80.regs.DE1.W = tmp;
    tmp        = z80.regs.HL.W; 
    z80.regs.HL.W  = z80.regs.HL1.W; 
    z80.regs.HL1.W = tmp;
}

static void rld(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    z80.regs.SH.W = z80.regs.HL.W + 1;
    DELAY_RLD;
    writeMem(z80.regs.HL.W, (val << 4) | (z80.regs.AF.B.h & 0x0f));
    z80.regs.AF.B.h = (z80.regs.AF.B.h & 0xf0) | (val >> 4);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.AF.B.h];
}

static void rrd(void) {
    UInt8 val = readMem(z80.regs.HL.W);
    z80.regs.SH.W = z80.regs.HL.W + 1;
    DELAY_RLD;
    writeMem(z80.regs.HL.W, (val >> 4) | (z80.regs.AF.B.h << 4));
    z80.regs.AF.B.h = (z80.regs.AF.B.h & 0xf0) | (val & 0x0f);
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.AF.B.h];
}

static void di(void) {
    z80.regs.iff1 = 0;
    z80.regs.iff2 = 0;
    updateFastLoop();
}

static void ei(void) {
/*    if (!z80.regs.iff1) {
        z80.regs.iff2 = 1;
        z80.regs.iff1 = 2;
    }*/
        z80.regs.iff2 = 1;
        z80.regs.iff1 = 1;
		z80.regs.ei_mode=1;
        
    updateFastLoop();
}

static void im_0(void) {
    z80.regs.im = 0;
}

static void im_1(void) {
    z80.regs.im = 1;
}

static void im_2(void) {
    z80.regs.im = 2;
}

static void in_a_c(void) { 
    z80.regs.AF.B.h = readPort(z80.regs.BC.W); 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.AF.B.h]; 
}

static void in_b_c(void) { 
    z80.regs.BC.B.h = readPort(z80.regs.BC.W); 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.BC.B.h]; 
}

static void in_c_c(void) { 
    z80.regs.BC.B.l = readPort(z80.regs.BC.W); 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.BC.B.l]; 
}

static void in_d_c(void) { 
    z80.regs.DE.B.h = readPort(z80.regs.BC.W); 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.DE.B.h]; 
}

static void in_e_c(void) { 
    z80.regs.DE.B.l = readPort(z80.regs.BC.W); 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.DE.B.l]; 
}

static void out_c_a(void) {
    writePort(z80.regs.BC.W, z80.regs.AF.B.h); 
}

static void out_c_b(void) {
    writePort(z80.regs.BC.W, z80.regs.BC.B.h);
}

static void out_c_c(void) { 
    writePort(z80.regs.BC.W, z80.regs.BC.B.l);
}

static void out_c_d(void) {
    writePort(z80.regs.BC.W, z80.regs.DE.B.h);
}

static void out_c_e(void) {
    writePort(z80.regs.BC.W, z80.regs.DE.B.l);
}

static void out_c_h(void) {
    writePort(z80.regs.BC.W, z80.regs.HL.B.h);
}

static void out_c_l(void) {
    writePort(z80.regs.BC.W, z80.regs.HL.B.l);
}

static void out_c_0(void) { 
    writePort(z80.regs.BC.W, 0); 
}

static void in_h_c(void) { 
    z80.regs.HL.B.h = readPort(z80.regs.BC.W); 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.HL.B.h]; 
}

static void in_l_c(void) { 
    z80.regs.HL.B.l = readPort(z80.regs.BC.W); 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[z80.regs.HL.B.l];
}

static void in_0_c(void) { 
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ZSPXYTable[readPort(z80.regs.BC.W)]; 
}

static void cpi(void) { 
    UInt8 val = readMem(z80.regs.HL.W++);
    UInt8 rv = z80.regs.AF.B.h - val;
    DELAY_BLOCK;

    z80.regs.BC.W--;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ((z80.regs.AF.B.h ^ val ^ rv) & H_FLAG) | 
        (ZSPXYTable[rv & 0xff] & (Z_FLAG | S_FLAG)) | N_FLAG;
    rv -= (z80.regs.AF.B.l & H_FLAG) >> 4;
    z80.regs.AF.B.l |= ((rv << 4) & Y_FLAG) | (rv & X_FLAG) | 
        (z80.regs.BC.W ? P_FLAG : 0);
}

static void cpir(void) { 
    cpi();
    if (z80.regs.BC.W && !(z80.regs.AF.B.l & Z_FLAG)) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2;
    }
}

static void cpd(void) { 
    UInt8 val = readMem(z80.regs.HL.W--);
    UInt8 rv = z80.regs.AF.B.h - val;
    DELAY_BLOCK;

    z80.regs.BC.W--;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & C_FLAG) | 
        ((z80.regs.AF.B.h ^ val ^ rv) & H_FLAG) | 
        (ZSPXYTable[rv & 0xff] & (Z_FLAG | S_FLAG)) | N_FLAG;
    rv -= (z80.regs.AF.B.l & H_FLAG) >> 4;
    z80.regs.AF.B.l |= ((rv << 4) & Y_FLAG) | (rv & X_FLAG) |
        (z80.regs.BC.W ? P_FLAG : 0);
}

static void cpdr(void) { 
    cpd();
    if (z80.regs.BC.W && !(z80.regs.AF.B.l & Z_FLAG)) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2;
    }
}

static void ldi(void) { 
    UInt8 val = readMem(z80.regs.HL.W++);
    writeMem(z80.regs.DE.W++, val);
    DELAY_LDI;

    z80.regs.BC.W--;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | C_FLAG)) |
        (((z80.regs.AF.B.h + val) << 4) & Y_FLAG) | 
        ((z80.regs.AF.B.h + val) & X_FLAG) | (z80.regs.BC.W ? P_FLAG : 0);
}

static void ldir(void) { 
    ldi();
    if (z80.regs.BC.W != 0) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2; 
    }
}

static void ldd(void) { 
    UInt8 val = readMem(z80.regs.HL.W--);
    writeMem(z80.regs.DE.W--, val);
    DELAY_LDI;

    z80.regs.BC.W--;
    z80.regs.AF.B.l = (z80.regs.AF.B.l & (S_FLAG | Z_FLAG | C_FLAG)) |
        (((z80.regs.AF.B.h + val) << 4) & Y_FLAG) | 
        ((z80.regs.AF.B.h + val) & X_FLAG) | (z80.regs.BC.W ? P_FLAG : 0);
}

static void lddr(void) { 
    ldd();
    if (z80.regs.BC.W != 0) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2; 
    }
}

static void ini(void) {  // Diff on flags
    UInt8  val;
    UInt16 tmp;
    DELAY_INOUT;
    z80.regs.BC.B.h--;
    val = readPort(z80.regs.BC.W);
    writeMem(z80.regs.HL.W++, val);
    z80.regs.AF.B.l = (ZSPXYTable[z80.regs.BC.B.h] & (Z_FLAG | S_FLAG)) |
        ((val >> 6) & N_FLAG);
    tmp = val + ((z80.regs.BC.B.l + 1) & 0xFF);
    z80.regs.AF.B.l |= (tmp >> 8) * (H_FLAG | C_FLAG) |
        (ZSPXYTable[(tmp & 0x07) ^ z80.regs.BC.B.h] & P_FLAG);
}

static void inir(void) { 
    ini();
    if (z80.regs.BC.B.h != 0) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2; 
    }
}


static void ind(void) {
    UInt8 val;
    UInt16 tmp;
    DELAY_INOUT;
    z80.regs.BC.B.h--;
    val = readPort(z80.regs.BC.W);
    writeMem(z80.regs.HL.W--, val);
    z80.regs.AF.B.l = (ZSPXYTable[z80.regs.BC.B.h] & (Z_FLAG | S_FLAG)) | 
        ((val >> 6) & N_FLAG);
    tmp = val + ((z80.regs.BC.B.l - 1) & 0xFF);
    z80.regs.AF.B.l |= (tmp >> 8) * (H_FLAG | C_FLAG) |
        (ZSPXYTable[(tmp & 0x07) ^ z80.regs.BC.B.h] & P_FLAG);
}

static void indr(void) { 
    ind();
    if (z80.regs.BC.B.h != 0) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2; 
    }
}

static void outi(void) {
    UInt8  val;
    UInt16 tmp;
    DELAY_INOUT;
    val = readMem(z80.regs.HL.W++);
    writePort(z80.regs.BC.W, val);
    z80.regs.BC.B.h--;
    z80.regs.AF.B.l = (ZSXYTable[z80.regs.BC.B.h]) |
        ((val >> 6) & N_FLAG);
    tmp = val + z80.regs.HL.B.l;
    z80.regs.AF.B.l |= (tmp >> 8) * (H_FLAG | C_FLAG) |
        (ZSPXYTable[(tmp & 0x07) ^ z80.regs.BC.B.h] & P_FLAG);
}

static void otir(void) { 
    outi();
    if (z80.regs.BC.B.h != 0) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2; 
    }
}

static void outd(void) {
    UInt8 val;
    UInt16 tmp;
    DELAY_INOUT;
    val = readMem(z80.regs.HL.W--);
    writePort(z80.regs.BC.W, val);
    z80.regs.BC.B.h--;
    z80.regs.AF.B.l = (ZSXYTable[z80.regs.BC.B.h]) |
        ((val >> 6) & N_FLAG);
    tmp = val + z80.regs.HL.B.l;
    z80.regs.AF.B.l |= (tmp >> 8) * (H_FLAG | C_FLAG) |
        (ZSPXYTable[(tmp & 0x07) ^ z80.regs.BC.B.h] & P_FLAG);
}

static void otdr(void) { 
    outd();
    if (z80.regs.BC.B.h != 0) {
        DELAY_BLOCK; 
        z80.regs.PC.W -= 2; 
    }
}

static Opcode opcodeMain[256] = {
    nop,         ld_bc_word,  ld_xbc_a,    inc_bc,      inc_b,       dec_b,       ld_b_byte,   rlca,
    ex_af_af,    add_hl_bc,   ld_a_xbc,    dec_bc,      inc_c,       dec_c,       ld_c_byte,   rrca,
    djnz,        ld_de_word,  ld_xde_a,    inc_de,      inc_d,       dec_d,       ld_d_byte,   rla,
    jr,          add_hl_de,   ld_a_xde,    dec_de,      inc_e,       dec_e,       ld_e_byte,   rra,
    jr_nz,       ld_hl_word,  ld_xword_hl, inc_hl,      inc_h,       dec_h,       ld_h_byte,   daa,
    jr_z,        add_hl_hl,   ld_hl_xword, dec_hl,      inc_l,       dec_l,       ld_l_byte,   cpl,
    jr_nc,       ld_sp_word,  ld_xbyte_a,  inc_sp,      inc_xhl,     dec_xhl,     ld_xhl_byte, scf,
    jr_c,        add_hl_sp,   ld_a_xbyte,  dec_sp,      inc_a,       dec_a,       ld_a_byte,   ccf,
    ld_b_b,      ld_b_c,      ld_b_d,      ld_b_e,      ld_b_h,      ld_b_l,      ld_b_xhl,    ld_b_a,
    ld_c_b,      ld_c_c,      ld_c_d,      ld_c_e,      ld_c_h,      ld_c_l,      ld_c_xhl,    ld_c_a,
    ld_d_b,      ld_d_c,      ld_d_d,      ld_d_e,      ld_d_h,      ld_d_l,      ld_d_xhl,    ld_d_a,
    ld_e_b,      ld_e_c,      ld_e_d,      ld_e_e,      ld_e_h,      ld_e_l,      ld_e_xhl,    ld_e_a,
    ld_h_b,      ld_h_c,      ld_h_d,      ld_h_e,      ld_h_h,      ld_h_l,      ld_h_xhl,    ld_h_a,
    ld_l_b,      ld_l_c,      ld_l_d,      ld_l_e,      ld_l_h,      ld_l_l,      ld_l_xhl,    ld_l_a,
    ld_xhl_b,    ld_xhl_c,    ld_xhl_d,    ld_xhl_e,    ld_xhl_h,    ld_xhl_l,    halt,        ld_xhl_a,
    ld_a_b,      ld_a_c,      ld_a_d,      ld_a_e,      ld_a_h,      ld_a_l,      ld_a_xhl,    ld_a_a,
    add_a_b,     add_a_c,     add_a_d,     add_a_e,     add_a_h,     add_a_l,     add_a_xhl,   add_a_a,
    adc_a_b,     adc_a_c,     adc_a_d,     adc_a_e,     adc_a_h,     adc_a_l,     adc_a_xhl,   adc_a_a,
    sub_b,       sub_c,       sub_d,       sub_e,       sub_h,       sub_l,       sub_xhl,     sub_a,
    sbc_a_b,     sbc_a_c,     sbc_a_d,     sbc_a_e,     sbc_a_h,     sbc_a_l,     sbc_a_xhl,   sbc_a_a,
    and_b,       and_c,       and_d,       and_e,       and_h,       and_l,       and_xhl,     and_a,
    xor_b,       xor_c,       xor_d,       xor_e,       xor_h,       xor_l,       xor_xhl,     xor_a,
    or_b,        or_c,        or_d,        or_e,        or_h,        or_l,        or_xhl,      or_a,
    cp_b,        cp_c,        cp_d,        cp_e,        cp_h,        cp_l,        cp_xhl,      cp_a,
    ret_nz,      pop_bc,      jp_nz,       jp,          call_nz,     push_bc,     add_a_byte,  rst_00,
    ret_z,       ret,         jp_z,        cb,          call_z,      call,        adc_a_byte,  rst_08,
    ret_nc,      pop_de,      jp_nc,       out_byte_a,  call_nc,     push_de,     sub_byte,    rst_10,
    ret_c,       exx,         jp_c,        in_a_byte,   call_c,      dd,          sbc_a_byte,  rst_18,
    ret_po,      pop_hl,      jp_po,       ex_xsp_hl,   call_po,     push_hl,     and_byte,    rst_20,
    ret_pe,      jp_hl,       jp_pe,       ex_de_hl,    call_pe,     ed,          xor_byte,    rst_28,
    ret_p,       pop_af,      jp_p,        di,          call_p,      push_af,     or_byte,     rst_30,
    ret_m,       ld_sp_hl,    jp_m,        ei,          call_m,      fd,          cp_byte,     rst_38
};

static Opcode opcodeCb[256] = {
    rlc_b,       rlc_c,       rlc_d,       rlc_e,       rlc_h,       rlc_l,       rlc_xhl,     rlc_a,
    rrc_b,       rrc_c,       rrc_d,       rrc_e,       rrc_h,       rrc_l,       rrc_xhl,     rrc_a,
    rl_b,        rl_c,        rl_d,        rl_e,        rl_h,        rl_l,        rl_xhl,      rl_a ,
    rr_b,        rr_c,        rr_d,        rr_e,        rr_h,        rr_l,        rr_xhl,      rr_a ,
    sla_b,       sla_c,       sla_d,       sla_e,       sla_h,       sla_l,       sla_xhl,     sla_a,
    sra_b,       sra_c,       sra_d,       sra_e,       sra_h,       sra_l,       sra_xhl,     sra_a,
    sll_b,       sll_c,       sll_d,       sll_e,       sll_h,       sll_l,       sll_xhl,     sll_a,
    srl_b,       srl_c,       srl_d,       srl_e,       srl_h,       srl_l,       srl_xhl,     srl_a,
    bit_0_b,     bit_0_c,     bit_0_d,     bit_0_e,     bit_0_h,     bit_0_l,     bit_0_xhl,   bit_0_a,
    bit_1_b,     bit_1_c,     bit_1_d,     bit_1_e,     bit_1_h,     bit_1_l,     bit_1_xhl,   bit_1_a,
    bit_2_b,     bit_2_c,     bit_2_d,     bit_2_e,     bit_2_h,     bit_2_l,     bit_2_xhl,   bit_2_a,
    bit_3_b,     bit_3_c,     bit_3_d,     bit_3_e,     bit_3_h,     bit_3_l,     bit_3_xhl,   bit_3_a,
    bit_4_b,     bit_4_c,     bit_4_d,     bit_4_e,     bit_4_h,     bit_4_l,     bit_4_xhl,   bit_4_a,
    bit_5_b,     bit_5_c,     bit_5_d,     bit_5_e,     bit_5_h,     bit_5_l,     bit_5_xhl,   bit_5_a,
    bit_6_b,     bit_6_c,     bit_6_d,     bit_6_e,     bit_6_h,     bit_6_l,     bit_6_xhl,   bit_6_a,
    bit_7_b,     bit_7_c,     bit_7_d,     bit_7_e,     bit_7_h,     bit_7_l,     bit_7_xhl,   bit_7_a,
    res_0_b,     res_0_c,     res_0_d,     res_0_e,     res_0_h,     res_0_l,     res_0_xhl,   res_0_a,
    res_1_b,     res_1_c,     res_1_d,     res_1_e,     res_1_h,     res_1_l,     res_1_xhl,   res_1_a,
    res_2_b,     res_2_c,     res_2_d,     res_2_e,     res_2_h,     res_2_l,     res_2_xhl,   res_2_a,
    res_3_b,     res_3_c,     res_3_d,     res_3_e,     res_3_h,     res_3_l,     res_3_xhl,   res_3_a,
    res_4_b,     res_4_c,     res_4_d,     res_4_e,     res_4_h,     res_4_l,     res_4_xhl,   res_4_a,
    res_5_b,     res_5_c,     res_5_d,     res_5_e,     res_5_h,     res_5_l,     res_5_xhl,   res_5_a,
    res_6_b,     res_6_c,     res_6_d,     res_6_e,     res_6_h,     res_6_l,     res_6_xhl,   res_6_a,
    res_7_b,     res_7_c,     res_7_d,     res_7_e,     res_7_h,     res_7_l,     res_7_xhl,   res_7_a,
    set_0_b,     set_0_c,     set_0_d,     set_0_e,     set_0_h,     set_0_l,     set_0_xhl,   set_0_a,
    set_1_b,     set_1_c,     set_1_d,     set_1_e,     set_1_h,     set_1_l,     set_1_xhl,   set_1_a,
    set_2_b,     set_2_c,     set_2_d,     set_2_e,     set_2_h,     set_2_l,     set_2_xhl,   set_2_a,
    set_3_b,     set_3_c,     set_3_d,     set_3_e,     set_3_h,     set_3_l,     set_3_xhl,   set_3_a,
    set_4_b,     set_4_c,     set_4_d,     set_4_e,     set_4_h,     set_4_l,     set_4_xhl,   set_4_a,
    set_5_b,     set_5_c,     set_5_d,     set_5_e,     set_5_h,     set_5_l,     set_5_xhl,   set_5_a,
    set_6_b,     set_6_c,     set_6_d,     set_6_e,     set_6_h,     set_6_l,     set_6_xhl,   set_6_a,
    set_7_b,     set_7_c,     set_7_d,     set_7_e,     set_7_h,     set_7_l,     set_7_xhl,   set_7_a
};

static Opcode opcodeDd[256] = {
    nop,         ld_bc_word,  ld_xbc_a,    inc_bc,      inc_b,       dec_b,       ld_b_byte,   rlca,
    ex_af_af,    add_ix_bc,   ld_a_xbc,    dec_bc,      inc_c,       dec_c,       ld_c_byte,   rrca,
    djnz,        ld_de_word,  ld_xde_a,    inc_de,      inc_d,       dec_d,       ld_d_byte,   rla,
    jr,          add_ix_de,   ld_a_xde,    dec_de,      inc_e,       dec_e,       ld_e_byte,   rra,
    jr_nz,       ld_ix_word,  ld_xword_ix, inc_ix,      inc_ixh,     dec_ixh,     ld_ixh_byte, daa,
    jr_z,        add_ix_ix,   ld_ix_xword, dec_ix,      inc_ixl,     dec_ixl,     ld_ixl_byte, cpl,
    jr_nc,       ld_sp_word,  ld_xbyte_a,  inc_sp,      inc_xix,     dec_xix,     ld_xix_byte, scf,
    jr_c,        add_ix_sp,   ld_a_xbyte,  dec_sp,      inc_a,       dec_a,       ld_a_byte,   ccf,
    ld_b_b,      ld_b_c,      ld_b_d,      ld_b_e,      ld_b_ixh,    ld_b_ixl,    ld_b_xix,    ld_b_a,
    ld_c_b,      ld_c_c,      ld_c_d,      ld_c_e,      ld_c_ixh,    ld_c_ixl,    ld_c_xix,    ld_c_a,
    ld_d_b,      ld_d_c,      ld_d_d,      ld_d_e,      ld_d_ixh,    ld_d_ixl,    ld_d_xix,    ld_d_a,
    ld_e_b,      ld_e_c,      ld_e_d,      ld_e_e,      ld_e_ixh,    ld_e_ixl,    ld_e_xix,    ld_e_a,
    ld_ixh_b,    ld_ixh_c,    ld_ixh_d,    ld_ixh_e,    ld_ixh_ixh,  ld_ixh_ixl,  ld_h_xix,    ld_ixh_a,
    ld_ixl_b,    ld_ixl_c,    ld_ixl_d,    ld_ixl_e,    ld_ixl_ixh,  ld_ixl_ixl,  ld_l_xix,    ld_ixl_a,
    ld_xix_b,    ld_xix_c,    ld_xix_d,    ld_xix_e,    ld_xix_h,    ld_xix_l,    halt,        ld_xix_a,
    ld_a_b,      ld_a_c,      ld_a_d,      ld_a_e,      ld_a_ixh,    ld_a_ixl,    ld_a_xix,    ld_a_a,
    add_a_b,     add_a_c,     add_a_d,     add_a_e,     add_a_ixh,   add_a_ixl,   add_a_xix,   add_a_a,
    adc_a_b,     adc_a_c,     adc_a_d,     adc_a_e,     adc_a_ixh,   adc_a_ixl,   adc_a_xix,   adc_a_a,
    sub_b,       sub_c,       sub_d,       sub_e,       sub_ixh,     sub_ixl,     sub_xix,     sub_a,
    sbc_a_b,     sbc_a_c,     sbc_a_d,     sbc_a_e,     sbc_a_ixh,   sbc_a_ixl,   sbc_a_xix,   sbc_a_a,
    and_b,       and_c,       and_d,       and_e,       and_ixh,     and_ixl,     and_xix,     and_a,
    xor_b,       xor_c,       xor_d,       xor_e,       xor_ixh,     xor_ixl,     xor_xix,     xor_a,
    or_b,        or_c,        or_d,        or_e,        or_ixh,      or_ixl,      or_xix,      or_a,
    cp_b,        cp_c,        cp_d,        cp_e,        cp_ixh,      cp_ixl,      cp_xix,      cp_a,
    ret_nz,      pop_bc,      jp_nz,       jp,          call_nz,     push_bc,     add_a_byte,  rst_00,
    ret_z,       ret,         jp_z,        dd_cb,       call_z,      call,        adc_a_byte,  rst_08,
    ret_nc,      pop_de,      jp_nc,       out_byte_a,  call_nc,     push_de,     sub_byte,    rst_10,
    ret_c,       exx,         jp_c,        in_a_byte,   call_c,      dd,          sbc_a_byte,  rst_18,
    ret_po,      pop_ix,      jp_po,       ex_xsp_ix,   call_po,     push_ix,     and_byte,    rst_20,
    ret_pe,      jp_ix,       jp_pe,       ex_de_hl,    call_pe,     ed,          xor_byte,    rst_28,
    ret_p,       pop_af,      jp_p,        di,          call_p,      push_af,     or_byte,     rst_30,
    ret_m,       ld_sp_ix,    jp_m,        ei,          call_m,      fd,          cp_byte,     rst_38  
};

static Opcode opcodeEd[256] = {
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    in_b_c,      out_c_b,     sbc_hl_bc,   ld_xword_bc, neg,         retn,        im_0,        ld_i_a,
    in_c_c,      out_c_c,     adc_hl_bc,   ld_bc_xword, neg,         reti,        im_0,        ld_r_a,
    in_d_c,      out_c_d,     sbc_hl_de,   ld_xword_de, neg,         retn,        im_1,        ld_a_i,
    in_e_c,      out_c_e,     adc_hl_de,   ld_de_xword, neg,         retn,        im_2,        ld_a_r,
    in_h_c,      out_c_h,     sbc_hl_hl,   ld_xword_hl, neg,         retn,        im_0,        rrd,
    in_l_c,      out_c_l,     adc_hl_hl,   ld_hl_xword, neg,         retn,        im_0,        rld,
    in_0_c,      out_c_0,     sbc_hl_sp,   ld_xword_sp, neg,         retn,        im_1,        nop,
    in_a_c,      out_c_a,     adc_hl_sp,   ld_sp_xword, neg,         retn,        im_2,        nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    nop,         nop,         nop,         nop,         nop,         nop,         nop,         nop,
    ldi,         cpi,         ini,         outi,        nop,         nop,         nop,         nop,
    ldd,         cpd,         ind,         outd,        nop,         nop,         nop,         nop,
    ldir,        cpir,        inir,        otir,        nop,         nop,         nop,         nop,
    lddr,        cpdr,        indr,        otdr,        nop,         nop,         nop,         nop,
    nop,         mulu_b,      nop,         muluw_bc,    nop,         nop,         nop,         nop,
    nop,         mulu_c,      nop,         nop,         nop,         nop,         nop,         nop,
    nop,         mulu_d,      nop,         muluw_de,    nop,         nop,         nop,         nop,
    nop,         mulu_e,      nop,         nop,         nop,         nop,         nop,         nop,
    nop,         mulu_h,      nop,         muluw_hl,    nop,         nop,         nop,         nop,
    nop,         mulu_l,      nop,         nop,         nop,         nop,         nop,         nop,
    nop,         mulu_xhl,    nop,         muluw_sp,    nop,         nop,         nop,         nop,
    nop,         mulu_a,      nop,         nop,         nop,         nop,         patch,       nop
};

static Opcode opcodeFd[256] = {
    nop,         ld_bc_word,ld_xbc_a,      inc_bc,      inc_b,       dec_b,       ld_b_byte,   rlca,
    ex_af_af,    add_iy_bc,   ld_a_xbc,    dec_bc,      inc_c,       dec_c,       ld_c_byte,   rrca,
    djnz,        ld_de_word,  ld_xde_a,    inc_de,      inc_d,       dec_d,       ld_d_byte,   rla,
    jr,          add_iy_de,   ld_a_xde,    dec_de,      inc_e,       dec_e,       ld_e_byte,   rra,
    jr_nz,       ld_iy_word,  ld_xword_iy, inc_iy,      inc_iyh,     dec_iyh,     ld_iyh_byte, daa,
    jr_z,        add_iy_iy,   ld_iy_xword, dec_iy,      inc_iyl,     dec_iyl,     ld_iyl_byte, cpl,
    jr_nc,       ld_sp_word,  ld_xbyte_a,  inc_sp,      inc_xiy,     dec_xiy,     ld_xiy_byte, scf,
    jr_c,        add_iy_sp,   ld_a_xbyte,  dec_sp,      inc_a,       dec_a,       ld_a_byte,   ccf,
    ld_b_b,      ld_b_c,      ld_b_d,      ld_b_e,      ld_b_iyh,    ld_b_iyl,    ld_b_xiy,    ld_b_a,
    ld_c_b,      ld_c_c,      ld_c_d,      ld_c_e,      ld_c_iyh,    ld_c_iyl,    ld_c_xiy,    ld_c_a,
    ld_d_b,      ld_d_c,      ld_d_d,      ld_d_e,      ld_d_iyh,    ld_d_iyl,    ld_d_xiy,    ld_d_a,
    ld_e_b,      ld_e_c,      ld_e_d,      ld_e_e,      ld_e_iyh,    ld_e_iyl,    ld_e_xiy,    ld_e_a,
    ld_iyh_b,    ld_iyh_c,    ld_iyh_d,    ld_iyh_e,    ld_iyh_iyh,  ld_iyh_iyl,  ld_h_xiy,    ld_iyh_a,
    ld_iyl_b,    ld_iyl_c,    ld_iyl_d,    ld_iyl_e,    ld_iyl_iyh,  ld_iyl_iyl,  ld_l_xiy,    ld_iyl_a,
    ld_xiy_b,    ld_xiy_c,    ld_xiy_d,    ld_xiy_e,    ld_xiy_h,    ld_xiy_l,    halt,        ld_xiy_a,
    ld_a_b,      ld_a_c,      ld_a_d,      ld_a_e,      ld_a_iyh,    ld_a_iyl,    ld_a_xiy,    ld_a_a,
    add_a_b,     add_a_c,     add_a_d,     add_a_e,     add_a_iyh,   add_a_iyl,   add_a_xiy,   add_a_a,
    adc_a_b,     adc_a_c,     adc_a_d,     adc_a_e,     adc_a_iyh,   adc_a_iyl,   adc_a_xiy,   adc_a_a,
    sub_b,       sub_c,       sub_d,       sub_e,       sub_iyh,     sub_iyl,     sub_xiy,     sub_a,
    sbc_a_b,     sbc_a_c,     sbc_a_d,     sbc_a_e,     sbc_a_iyh,   sbc_a_iyl,   sbc_a_xiy,   sbc_a_a,
    and_b,       and_c,       and_d,       and_e,       and_iyh,     and_iyl,     and_xiy,     and_a,
    xor_b,       xor_c,       xor_d,       xor_e,       xor_iyh,     xor_iyl,     xor_xiy,     xor_a,
    or_b,        or_c,        or_d,        or_e,        or_iyh,      or_iyl,      or_xiy,      or_a,
    cp_b,        cp_c,        cp_d,        cp_e,        cp_iyh,      cp_iyl,      cp_xiy,      cp_a,
    ret_nz,      pop_bc,      jp_nz,       jp,          call_nz,     push_bc,     add_a_byte,  rst_00,
    ret_z,       ret,         jp_z,        fd_cb,       call_z,      call,        adc_a_byte,  rst_08,
    ret_nc,      pop_de,      jp_nc,       out_byte_a,  call_nc,     push_de,     sub_byte,    rst_10,
    ret_c,       exx,         jp_c,        in_a_byte,   call_c,      dd,          sbc_a_byte,  rst_18,
    ret_po,      pop_iy,      jp_po,       ex_xsp_iy,   call_po,     push_iy,     and_byte,    rst_20,
    ret_pe,      jp_iy,       jp_pe,       ex_de_hl,    call_pe,     ed,          xor_byte,    rst_28,
    ret_p,       pop_af,      jp_p,        di,          call_p,      push_af,     or_byte,     rst_30,
    ret_m,       ld_sp_iy,    jp_m,        ei,          call_m,      fd,          cp_byte,     rst_38  
};

static OpcodeNn opcodeNnCb[256] = {
    rlc_xnn_b,   rlc_xnn_c,   rlc_xnn_d,   rlc_xnn_e,   rlc_xnn_h,   rlc_xnn_l,   rlc_xnn,     rlc_xnn_a,
    rrc_xnn_b,   rrc_xnn_c,   rrc_xnn_d,   rrc_xnn_e,   rrc_xnn_h,   rrc_xnn_l,   rrc_xnn,     rrc_xnn_a,
    rl_xnn_b,    rl_xnn_c,    rl_xnn_d,    rl_xnn_e,    rl_xnn_h,    rl_xnn_l,    rl_xnn,      rl_xnn_a,
    rr_xnn_b,    rr_xnn_c,    rr_xnn_d,    rr_xnn_e,    rr_xnn_h,    rr_xnn_l,    rr_xnn,      rr_xnn_a,
    sla_xnn_b,   sla_xnn_c,   sla_xnn_d,   sla_xnn_e,   sla_xnn_h,   sla_xnn_l,   sla_xnn,     sla_xnn_a,   
    sra_xnn_b,   sra_xnn_c,   sra_xnn_d,   sra_xnn_e,   sra_xnn_h,   sra_xnn_l,   sra_xnn,     sra_xnn_a,
    sll_xnn_b,   sll_xnn_c,   sll_xnn_d,   sll_xnn_e,   sll_xnn_h,   sll_xnn_l,   sll_xnn,     sll_xnn_a,
    srl_xnn_b,   srl_xnn_c,   srl_xnn_d,   srl_xnn_e,   srl_xnn_h,   srl_xnn_l,   srl_xnn,     srl_xnn_a,
    bit_0_xnn,   bit_0_xnn,   bit_0_xnn,   bit_0_xnn,   bit_0_xnn,   bit_0_xnn,   bit_0_xnn,   bit_0_xnn,   
    bit_1_xnn,   bit_1_xnn,   bit_1_xnn,   bit_1_xnn,   bit_1_xnn,   bit_1_xnn,   bit_1_xnn,   bit_1_xnn,   
    bit_2_xnn,   bit_2_xnn,   bit_2_xnn,   bit_2_xnn,   bit_2_xnn,   bit_2_xnn,   bit_2_xnn,   bit_2_xnn,   
    bit_3_xnn,   bit_3_xnn,   bit_3_xnn,   bit_3_xnn,   bit_3_xnn,   bit_3_xnn,   bit_3_xnn,   bit_3_xnn,   
    bit_4_xnn,   bit_4_xnn,   bit_4_xnn,   bit_4_xnn,   bit_4_xnn,   bit_4_xnn,   bit_4_xnn,   bit_4_xnn,   
    bit_5_xnn,   bit_5_xnn,   bit_5_xnn,   bit_5_xnn,   bit_5_xnn,   bit_5_xnn,   bit_5_xnn,   bit_5_xnn,   
    bit_6_xnn,   bit_6_xnn,   bit_6_xnn,   bit_6_xnn,   bit_6_xnn,   bit_6_xnn,   bit_6_xnn,   bit_6_xnn,   
    bit_7_xnn,   bit_7_xnn,   bit_7_xnn,   bit_7_xnn,   bit_7_xnn,   bit_7_xnn,   bit_7_xnn,   bit_7_xnn,   
    res_0_xnn_b, res_0_xnn_c, res_0_xnn_d, res_0_xnn_e, res_0_xnn_h, res_0_xnn_l, res_0_xnn,   res_0_xnn_a,
    res_1_xnn_b, res_1_xnn_c, res_1_xnn_d, res_1_xnn_e, res_1_xnn_h, res_1_xnn_l, res_1_xnn,   res_1_xnn_a,
    res_2_xnn_b, res_2_xnn_c, res_2_xnn_d, res_2_xnn_e, res_2_xnn_h, res_2_xnn_l, res_2_xnn,   res_2_xnn_a,
    res_3_xnn_b, res_3_xnn_c, res_3_xnn_d, res_3_xnn_e, res_3_xnn_h, res_3_xnn_l, res_3_xnn,   res_3_xnn_a,
    res_4_xnn_b, res_4_xnn_c, res_4_xnn_d, res_4_xnn_e, res_4_xnn_h, res_4_xnn_l, res_4_xnn,   res_4_xnn_a,
    res_5_xnn_b, res_5_xnn_c, res_5_xnn_d, res_5_xnn_e, res_5_xnn_h, res_5_xnn_l, res_5_xnn,   res_5_xnn_a,
    res_6_xnn_b, res_6_xnn_c, res_6_xnn_d, res_6_xnn_e, res_6_xnn_h, res_6_xnn_l, res_6_xnn,   res_6_xnn_a,
    res_7_xnn_b, res_7_xnn_c, res_7_xnn_d, res_7_xnn_e, res_7_xnn_h, res_7_xnn_l, res_7_xnn,   res_7_xnn_a,
    set_0_xnn_b, set_0_xnn_c, set_0_xnn_d, set_0_xnn_e, set_0_xnn_h, set_0_xnn_l, set_0_xnn,   set_0_xnn_a,
    set_1_xnn_b, set_1_xnn_c, set_1_xnn_d, set_1_xnn_e, set_1_xnn_h, set_1_xnn_l, set_1_xnn,   set_1_xnn_a,
    set_2_xnn_b, set_2_xnn_c, set_2_xnn_d, set_2_xnn_e, set_2_xnn_h, set_2_xnn_l, set_2_xnn,   set_2_xnn_a,
    set_3_xnn_b, set_3_xnn_c, set_3_xnn_d, set_3_xnn_e, set_3_xnn_h, set_3_xnn_l, set_3_xnn,   set_3_xnn_a,
    set_4_xnn_b, set_4_xnn_c, set_4_xnn_d, set_4_xnn_e, set_4_xnn_h, set_4_xnn_l, set_4_xnn,   set_4_xnn_a,
    set_5_xnn_b, set_5_xnn_c, set_5_xnn_d, set_5_xnn_e, set_5_xnn_h, set_5_xnn_l, set_5_xnn,   set_5_xnn_a,
    set_6_xnn_b, set_6_xnn_c, set_6_xnn_d, set_6_xnn_e, set_6_xnn_h, set_6_xnn_l, set_6_xnn,   set_6_xnn_a,
    set_7_xnn_b, set_7_xnn_c, set_7_xnn_d, set_7_xnn_e, set_7_xnn_h, set_7_xnn_l, set_7_xnn,   set_7_xnn_a,
};

static void dd_cb(void) {
	UInt16 addr = z80.regs.IX.W + (Int8)readOpcode(z80.regs.PC.W++);
    IntN opcode = readOpcode(z80.regs.PC.W++);
	DELAY_M1;
    opcodeNnCb[opcode](addr);
}

static void fd_cb(void) {
	UInt16 addr = z80.regs.IY.W + (Int8)readOpcode(z80.regs.PC.W++);
    IntN opcode = readOpcode(z80.regs.PC.W++);
	DELAY_M1;
    opcodeNnCb[opcode](addr);
}

static void cb(void) {
    IntN opcode = readOpcode(z80.regs.PC.W++);
    M1();
    opcodeCb[opcode]();
}

static void dd(void) {
    IntN opcode = readOpcode(z80.regs.PC.W++);
    M1();
    opcodeDd[opcode]();
}

static void ed(void) {
    IntN opcode = readOpcode(z80.regs.PC.W++);
    M1();
    opcodeEd[opcode]();
}

static void fd(void) {
    IntN opcode = readOpcode(z80.regs.PC.W++);
    M1();
    opcodeFd[opcode]();
}

static void executeInstruction(IntN opcode) {
    M1();
    opcodeMain[opcode]();
}


static void z80InitTables(void) {
    Int16 i;

	for (i = 0; i < 256; ++i) {
        UInt8 flags = i ^ 1;
        flags = flags ^ (flags >> 4);
        flags = flags ^ (flags << 2);
        flags = flags ^ (flags >> 1);
        flags = (flags & V_FLAG) | H_FLAG | (i & (S_FLAG | X_FLAG | Y_FLAG)) |
                (i ? 0 : Z_FLAG);

        ZSXYTable[i]  = flags & (Z_FLAG | S_FLAG | X_FLAG | Y_FLAG);
		ZSPXYTable[i] = flags & (Z_FLAG | S_FLAG | X_FLAG | Y_FLAG | V_FLAG);
		ZSPHTable[i]  = flags & (Z_FLAG | S_FLAG | V_FLAG | H_FLAG);
	}

    for (i = 0; i < 0x800; ++i) {
		Int16 flagC = i & 0x100;
		Int16 flagN = i & 0x200;
		Int16 flagH = i & 0x400;
		UInt8 a = i & 0xff;
		UInt8 hi = a / 16;
		UInt8 lo = a & 15;
		UInt8 diff;
        UInt8 regA;

		if (flagC) {
			diff = ((lo <= 9) && !flagH) ? 0x60 : 0x66;
		} 
        else {
			if (lo >= 10) {
				diff = (hi <= 8) ? 0x06 : 0x66;
			} 
            else {
				if (hi >= 10) {
					diff = flagH ? 0x66 : 0x60;
				} 
                else {
					diff = flagH ? 0x06 : 0x00;
				}
			}
		}
		regA = flagN ? a - diff : a + diff;
		DAATable[i] = (regA << 8) |
                      ZSPXYTable[regA] | 
                      (flagN ? N_FLAG : 0) |
                      (flagC || (lo <= 9 ? hi >= 10 : hi >= 9) ? C_FLAG : 0) |
                      ((flagN ? (flagH && lo <= 5) : lo >= 10) ? H_FLAG : 0);
	}
}

void z80Init(void)
{
    z80InitTables();

    z80.terminate   = 0;
    z80.systemTime  = 0;

    z80Reset(0);
}

UInt32 z80GetSystemTime(void) {
    return z80.systemTime;
}

void z80Reset(UInt32 cpuTime) {

    z80.regs.AF.W       = 0xffff;
	z80.regs.BC.W       = 0xffff;
	z80.regs.DE.W       = 0xffff;
	z80.regs.HL.W       = 0xffff;
	z80.regs.IX.W       = 0xffff;
	z80.regs.IY.W       = 0xffff;
	z80.regs.SP.W       = 0xffff;
	z80.regs.AF1.W      = 0xffff;
	z80.regs.BC1.W      = 0xffff;
	z80.regs.DE1.W      = 0xffff;
	z80.regs.HL1.W      = 0xffff;
    z80.regs.SH.W       = 0xffff;
	z80.regs.I          = 0x00;
	z80.regs.R          = 0x00;
	z80.regs.R2         = 0;
	z80.regs.PC.W       = 0x0000;

    z80.regs.iff1       = 0;
    z80.regs.iff2       = 0;
    z80.regs.im         = 0;
    z80.regs.halt       = 0;
    z80.regs.ei_mode    = 0;
    z80.dataBus         = 0xff;
    z80.defaultDatabus  = 0xff;
    z80.intState        = INT_HIGH;
    z80.nmiState        = INT_HIGH;
    
    updateFastLoop();
}

void z80SetDataBus(UInt8 value, UInt8 defaultValue, Int8 setDefault) {
    z80.dataBus = value;
    if (setDefault) {
        z80.defaultDatabus = defaultValue;
    }
}

void z80SetInt(void) {
    z80.intState = INT_LOW;
    updateFastLoop();
}

void z80ClearInt(void) {
    z80.intState = INT_HIGH;
    updateFastLoop();
}

void z80SetNmi(void) {
    if (z80.nmiState == INT_HIGH) {
        z80.nmiState = INT_EDGE;
        updateFastLoop();
    }
}

void z80ClearNmi(void) {
    z80.nmiState = INT_HIGH;
    updateFastLoop();
}

void z80StopExecution(void) {
    z80.terminate = 1;
}

void z80SetTimeout(SystemTime time)
{
    z80.timeout = time;
    updateFastLoop();
}

void z80Execute(void) {
    while (!z80.terminate) {
        UInt16 address;

        if ((Int32)(z80.timeout - z80.systemTime) <= 0) {
            timeout();
        }

        while ((Int32)(z80.fastTimeout - z80.systemTime) > 0) {
            executeInstruction(readOpcode(z80.regs.PC.W++));
        }

        if (z80.regs.halt) {
			continue;
        }

		if (z80.regs.ei_mode) {
			z80.regs.ei_mode=0;
            updateFastLoop();
			continue;
		}

        if (! ((z80.intState==INT_LOW && z80.regs.iff1)||(z80.nmiState==INT_EDGE)) ) {
			continue;
        }

        /* If it is NMI... */

        if (z80.nmiState == INT_EDGE) {
            z80.nmiState = INT_LOW;
	        writeMemory(--z80.regs.SP.W, z80.regs.PC.B.h);
	        writeMemory(--z80.regs.SP.W, z80.regs.PC.B.l);
            z80.regs.iff1 = 0;
            z80.regs.PC.W = 0x0066;
            M1();
            DELAY_NMI;
        }
        else {
            z80.regs.iff1 = 0;
            z80.regs.iff2 = 0;

            switch (z80.regs.im) {

            case 0:
                DELAY_IM;
                address = z80.dataBus;
                z80.dataBus = z80.defaultDatabus;
                executeInstruction(address);
                break;

            case 1:
                DELAY_IM;
                executeInstruction(0xff);
                break;

            case 2:
                address = z80.dataBus | ((Int16)z80.regs.I << 8);
                z80.dataBus = z80.defaultDatabus;
	            writeMemory(--z80.regs.SP.W, z80.regs.PC.B.h);
	            writeMemory(--z80.regs.SP.W, z80.regs.PC.B.l);
                z80.regs.PC.B.l = readMemory(address++);
                z80.regs.PC.B.h = readMemory(address);
                M1();
                DELAY_IM2;
                break;
            }
        }
        updateFastLoop();
    }
}
