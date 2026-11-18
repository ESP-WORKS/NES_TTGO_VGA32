/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General 
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful, 
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU 
** Library General Public License for more details.  To obtain a 
** copy of the GNU Library General Public License, write to the Free 
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** map4.c
**
** mapper 4 interface
** $Id: map004.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/

#include <string.h>
#include "noftypes.h"
#include "nes6502.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"
/* MMC3 0.1.1: proper 8 bank registers, PRG/CHR inversion, scanline IRQ.
 * IRQ is an approximate hblank implementation, not filtered PPU A12 edge. */
static uint8 select_reg,banks[8],irq_latch,irq_counter;
static bool irq_enable,irq_reload,irq_raised;
static void mmc3_chr1(int address,int bank) {
    rominfo_t *info=mmc_getinfo();
    if ((info->mapper_number == 74 && (bank == 8 || bank == 9)) ||
        (info->mapper_number == 192 && bank >= 8 && bank <= 11)) {
        // CHR ROM / CHR RAM selectable only in these bank ranges.
        mmc_bankchrram(1,address,bank-8);
    } else if (info->mapper_number == 119 && (bank & 0x40)) {
        /* TQROM: bit 6 selects the independent CHR RAM, 8KiB. */
        mmc_bankchrram(1,address,bank&7);
    } else {
        mmc_bankchr(1,address,bank);
    }
}
static void mmc3_mapall(void) {
    int n,b0=banks[0]&0xFE,b1=banks[1]&0xFE;
    int inv=(select_reg&0x80)!=0;
    for(n=0;n<2;++n) {
        mmc3_chr1((inv?0x1000:0)+n*0x400,b0+n);
        mmc3_chr1((inv?0x1800:0x800)+n*0x400,b1+n);
    }
    for(n=0;n<4;++n) mmc3_chr1((inv?0:0x1000)+n*0x400,banks[2+n]);
    if (mmc_getinfo()->mapper_number == 118) {
        /* TxSROM: CHR A17 controls which CIRAM page each name table sees. */
        if(inv) ppu_mirror((banks[2]>>7)&1,(banks[3]>>7)&1,
                          (banks[4]>>7)&1,(banks[5]>>7)&1);
        else {
            int a=(banks[0]>>7)&1,b=(banks[1]>>7)&1;
            ppu_mirror(a,a,b,b);
        }
    }
    if(select_reg&0x40) {
        mmc_bankrom(8,0x8000,mmc_getinfo()->rom_banks*2-2);
        mmc_bankrom(8,0xC000,banks[6]);
    } else {
        mmc_bankrom(8,0x8000,banks[6]);
        mmc_bankrom(8,0xC000,mmc_getinfo()->rom_banks*2-2);
    }
    mmc_bankrom(8,0xA000,banks[7]);
    mmc_bankrom(8,0xE000,MMC_LASTBANK);
}
static void map4_init(void) {
    select_reg=0;memset(banks,0,sizeof(banks));
    banks[7]=1;
    irq_latch=irq_counter=0;irq_enable=irq_reload=irq_raised=false;
    mmc3_mapall();
}
static void map4_write(uint32 address,uint8 value) {
    switch(address & 0xE001) {
    case 0x8000:select_reg=value;mmc3_mapall();break;
    case 0x8001:banks[select_reg&7]=value;mmc3_mapall();break;
    case 0xA000:
       if(mmc_getinfo()->mapper_number!=118 && !(mmc_getinfo()->flags&ROM_FLAG_FOURSCREEN)) {
           if(value&1) ppu_mirror(0,0,1,1);
           else ppu_mirror(0,1,0,1);
       }
       break;
    case 0xA001:break; // RAM protection: ignored for MMC6 compatibility
    case 0xC000:irq_latch=value;break;
    case 0xC001:irq_counter=0;irq_reload=true;break;
    case 0xE000:
       irq_enable=false;
       /* v1.02: acknowledge. A request still latched behind the I flag must
        * not fire after the next CLI/RTI (it would be a spurious IRQ). */
       if(irq_raised){irq_raised=false;nes6502_clearirq_src(NES6502_IRQ_MAPPER);}
       break;
    case 0xE001:irq_enable=true;break;
    }
}
static void map4_hblank(int vblank) {
    if(vblank || !ppu_enabled()) return;
    if(irq_reload || irq_counter==0) irq_counter=irq_latch;
    else --irq_counter;
    irq_reload=false;
    if(irq_counter==0 && irq_enable){irq_raised=true;nes6502_irq_src(NES6502_IRQ_MAPPER);}
}
static void map4_getstate(SnssMapperBlock *state) {
    state->extraData.mapper4.irqCounter=irq_counter;
    state->extraData.mapper4.irqLatchCounter=irq_latch;
    state->extraData.mapper4.irqCounterEnabled=irq_enable;
    state->extraData.mapper4.last8000Write=select_reg;
}
static void map4_setstate(SnssMapperBlock *state) {
    irq_counter=state->extraData.mapper4.irqCounter;
    irq_latch=state->extraData.mapper4.irqLatchCounter;
    irq_enable=state->extraData.mapper4.irqCounterEnabled;
    select_reg=state->extraData.mapper4.last8000Write;
    irq_reload=false;irq_raised=false;
    mmc3_mapall();
}
static const map_memwrite wr[]={{0x8000,0xFFFF,map4_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map4_intf={4,"MMC3",map4_init,NULL,map4_hblank,map4_getstate,map4_setstate,NULL,wr,NULL};
const mapintf_t map118_intf={118,"TxSROM",map4_init,NULL,map4_hblank,map4_getstate,map4_setstate,NULL,wr,NULL};
const mapintf_t map119_intf={119,"TQROM",map4_init,NULL,map4_hblank,map4_getstate,map4_setstate,NULL,wr,NULL};

const mapintf_t map74_intf={74,"MMC3 pirate (2KB CHR-RAM)",map4_init,NULL,
    map4_hblank,map4_getstate,map4_setstate,NULL,wr,NULL};
const mapintf_t map192_intf={192,"MMC3 pirate (4KB CHR-RAM)",map4_init,NULL,
    map4_hblank,map4_getstate,map4_setstate,NULL,wr,NULL};
