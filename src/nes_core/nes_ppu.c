#include "emuapi.h"
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
** nes_ppu.c
**
** NES PPU emulation
** $Id: nes_ppu.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "noftypes.h"
#include "nes_ppu.h"
#include "nes.h"
#include "nes6502.h"
#include "log.h"
#include "nes_mmc.h"

#include "bitmap.h"
#include "vid_drv.h"
#include "nes_pal.h"
#include "nesinput.h"
#include "nes_hot.h"
#include "nes_bg_lut.h"


/* PPU access */
#define  PPU_MEM(x)           ppu.page[(x) >> 10][(x)]

/* A CHR ROM page is immutable, even on mixed CHR-ROM/CHR-RAM TQROM.
 * Legacy Nofrendo wrote PPUDATA directly to the mapped CHR ROM pointer,
 * corrupting the loaded cartridge in PSRAM when games probe pattern data. */
static bool ppu_pattern_page_readonly(uint32 addr);

/* Background (color 0) and solid sprite pixel flags */
#define  BG_TRANS             0x80
#define  SP_PIXEL             0x40
#define  BG_CLEAR(V)          ((V) & BG_TRANS)
#define  BG_SOLID(V)          (0 == BG_CLEAR(V))
#define  SP_CLEAR(V)          (0 == ((V) & SP_PIXEL))

/* Full BG color */
#define  FULLBG               (ppu.palette[0] | BG_TRANS)

/* the NES PPU */
static ppu_t ppu;
static void ppu_invalidate_oam(void);
void ppu_async_drain(void);
/* Four palette groups x 256 packed four-pixel patterns, in internal RAM. */
static nes_bg_lut_t bg_lut;


static bool ppu_pattern_page_readonly(uint32 addr)
{
   // Also protects CHR ROM temporarily mapped into nametable space by
   // Sunsoft-4 mapper 68: CPU writes must never corrupt the loaded ROM.
   if (addr >= 0x3F00) return false;
   const rominfo_t *cart=mmc_getinfo();
   if (!cart || !cart->vrom || cart->vrom_banks <= 0) return false;
   uintptr_t physical = (uintptr_t)(ppu.page[addr >> 10] + addr);
   uintptr_t start = (uintptr_t)cart->vrom;
   uintptr_t end = start + ((uintptr_t)cart->vrom_banks * 8192u);
   return physical >= start && physical < end;
}

void ppu_displaysprites(bool display)
{
   ppu.drawsprites = display;
}

/* Nofrendo's pages contain address-adjusted pointers. Some mappers point
 * nametable pages into ExRAM / CHR-ROM (MMC5, Sunsoft 4, Taito, etc.). Copy
 * only INTERNAL nametable pointers by rebasing their physical addresses;
 * preserve external pointers untouched. Otherwise the old ptr subtraction
 * assumed unrelated arrays were one allocation and corrupted page tables. */
static uint8 *ppu_rebase_page(const ppu_t *from, const ppu_t *to,
                             uint8 *page, unsigned slot)
{
   uintptr_t physical=(uintptr_t)page+(uintptr_t)slot*1024u;
   uintptr_t old_base=(uintptr_t)from->nametab;
   if (physical >= old_base && physical-old_base < sizeof(from->nametab)) {
      uintptr_t new_physical=(uintptr_t)to->nametab+(physical-old_base);
      return (uint8 *)(new_physical-(uintptr_t)slot*1024u);
   }
   return page;
}
void ppu_setcontext(ppu_t *src_ppu)
{
   ASSERT(src_ppu);
   ppu_async_drain();
   ppu = *src_ppu;
   ppu_invalidate_oam();
   for (unsigned slot=8;slot<16;++slot)
      ppu.page[slot]=ppu_rebase_page(src_ppu,&ppu,src_ppu->page[slot],slot);
}
void ppu_getcontext(ppu_t *dest_ppu)
{
   ASSERT(dest_ppu);
   *dest_ppu = ppu;
   for (unsigned slot=8;slot<16;++slot)
      dest_ppu->page[slot]=ppu_rebase_page(&ppu,dest_ppu,ppu.page[slot],slot);
}

ppu_t *ppu_create(void)
{
   static bool pal_generated = false;
   ppu_t *temp;

   temp = malloc(sizeof(ppu_t));
   if (NULL == temp)
      return NULL;

   memset(temp, 0, sizeof(ppu_t));

   temp->latchfunc = NULL;
   temp->vromswitch = NULL;
   temp->vram_present = false;
   temp->drawsprites = true;

   /* TODO: probably a better way to do this... */
   if (false == pal_generated)
   {
      pal_generate();
      pal_generated = true;
   }

   ppu_setdefaultpal(temp);

   return temp;
}

void ppu_destroy(ppu_t **src_ppu)
{
   if (*src_ppu)
   {
      free(*src_ppu);
      *src_ppu = NULL;
   }
}

void ppu_setpage(int size, int page_num, uint8 *location)
{
   /* Guard the page table against malformed mapper register values. */
   if ((size != 1 && size != 2 && size != 4 && size != 8) ||
       page_num < 0 || page_num + size > 16 || !location)
      return;
   /* deliberately fall through */
   switch (size)
   {
   case 8:  
      ppu.page[page_num++] = location;
      ppu.page[page_num++] = location;
      ppu.page[page_num++] = location;
      ppu.page[page_num++] = location;
   case 4:  
      ppu.page[page_num++] = location;
      ppu.page[page_num++] = location;
   case 2:
      ppu.page[page_num++] = location;
   case 1:
      ppu.page[page_num++] = location;
      break;
   }
}

/* make sure $3000-$3F00 mirrors $2000-$2F00 */
void ppu_mirrorhipages(void)
{
   ppu.page[12] = ppu.page[8] - 0x1000;
   ppu.page[13] = ppu.page[9] - 0x1000;
   ppu.page[14] = ppu.page[10] - 0x1000;
   ppu.page[15] = ppu.page[11] - 0x1000;
}

void ppu_mirror(int nt1, int nt2, int nt3, int nt4)
{
   /* CIRAM or four-screen VRAM has four 1KiB tables at most. */
   nt1 &= 3; nt2 &= 3; nt3 &= 3; nt4 &= 3;
   ppu.page[8] = ppu.nametab + (nt1 << 10) - 0x2000;
   ppu.page[9] = ppu.nametab + (nt2 << 10) - 0x2400;
   ppu.page[10] = ppu.nametab + (nt3 << 10) - 0x2800;
   ppu.page[11] = ppu.nametab + (nt4 << 10) - 0x2C00;
   ppu.page[12] = ppu.page[8] - 0x1000;
   ppu.page[13] = ppu.page[9] - 0x1000;
   ppu.page[14] = ppu.page[10] - 0x1000;
   ppu.page[15] = ppu.page[11] - 0x1000;
}

/* bleh, for snss */
uint8 *ppu_getpage(int page)
{
   return ppu.page[page];
}

static void mem_trash(uint8 *buffer, int length)
{
   int i;

   for (i = 0; i < length; i++)
      buffer[i] = (uint8) rand();
}

/* reset state of ppu */
void ppu_reset(int reset_type)
{
   ppu_async_drain();
   if (HARD_RESET == reset_type)
      mem_trash(ppu.oam, 256);
   ppu_invalidate_oam();

   ppu.ctrl0 = 0;
   ppu.ctrl1 = PPU_CTRL1F_OBJON | PPU_CTRL1F_BGON;
   ppu.stat = 0;
   ppu.flipflop = 0;
   ppu.vaddr = ppu.vaddr_latch = 0x2000;
   ppu.oam_addr = 0;
   ppu.tile_xofs = 0;

   ppu.latch = 0;
   ppu.vram_accessible = true;
}

/* we render a scanline of graphics first so we know exactly
** where the sprite 0 strike is going to occur (in terms of
** cpu cycles), using the relation that 3 pixels == 1 cpu cycle
*/
static void ppu_setstrike(int x_loc)
{
   if (false == ppu.strikeflag)
   {
      ppu.strikeflag = true;

      /* 3 pixels per cpu cycle */
      ppu.strike_cycle = nes6502_getcycles(false) + (x_loc / 3);
   }
}

static void ppu_oamdma(uint8 value)
{
   uint32 cpu_address;
   uint8 oam_loc;

   cpu_address = (uint32) (value << 8);

   /* Sprite DMA starts at the current SPRRAM address */
   oam_loc = ppu.oam_addr;
   ppu_invalidate_oam();
   do
   {
      ppu.oam[oam_loc++] = nes6502_getbyte(cpu_address++);
   }
   while (oam_loc != ppu.oam_addr);

   /* TODO: enough with houdini */
   cpu_address -= 256;
   /* Odd address in $2003 */
   if ((ppu.oam_addr >> 2) & 1)
   {
      for (oam_loc = 4; oam_loc < 8; oam_loc++)
         ppu.oam[oam_loc] = nes6502_getbyte(cpu_address++);
      cpu_address += 248;
      for (oam_loc = 0; oam_loc < 4; oam_loc++)
         ppu.oam[oam_loc] = nes6502_getbyte(cpu_address++);
   }
   /* Even address in $2003 */
   else
   {
      for (oam_loc = 0; oam_loc < 8; oam_loc++)
         ppu.oam[oam_loc] = nes6502_getbyte(cpu_address++);
   }

   /* make the CPU spin for DMA cycles */
   nes6502_burn(513);
   nes6502_release();
}

/* TODO: this isn't the PPU! */
void ppu_writehigh(uint32 address, uint8 value)
{
   switch (address)
   {
   case PPU_OAMDMA:
      ppu_oamdma(value);
      break;

   case PPU_JOY0:
      /* VS system VROM switching - bleh!*/
      if (ppu.vromswitch)
         ppu.vromswitch(value);

      /* see if we need to strobe them joypads */
      value &= 1;
      
      if (0 == value && ppu.strobe)
         input_strobe();

      ppu.strobe = value;
      break;

   case PPU_JOY1: /* frame IRQ control */
      nes_setfiq(value);
      break;

   default:
      break;
   }
}

/* TODO: this isn't the PPU! */
uint8 ppu_readhigh(uint32 address)
{
   uint8 value;

   switch (address)
   {
   case PPU_JOY0:
      value = input_get(INP_JOYPAD0);
      break;

   case PPU_JOY1:
      /* TODO: better input handling */
      value = input_get(INP_ZAPPER | INP_JOYPAD1 
                        /*| INP_ARKANOID*/ 
                        /*| INP_POWERPAD*/);
      break;

   default:
      value = 0xFF;
      break;
   }

   return value;
}

/* Read from $2000-$2007 */
uint8 ppu_read(uint32 address)
{
   uint8 value;
   
   /* handle mirrored reads up to $3FFF */
   switch (address & 0x2007)
   {
   case PPU_STAT:
      value = (ppu.stat & 0xE0) | (ppu.latch & 0x1F);

      if (ppu.strikeflag)
      {
         if (nes6502_getcycles(false) >= ppu.strike_cycle)
            value |= PPU_STATF_STRIKE;
      }

      /* clear both vblank flag and vram address flipflop */
      ppu.stat &= ~PPU_STATF_VBLANK;
      ppu.flipflop = 0;
      break;

   case PPU_VDATA:
      /* buffered VRAM reads */
      value = ppu.latch = ppu.vdata_latch;

      /* VRAM only accessible during VBL */
      if ((ppu.bg_on || ppu.obj_on) && !ppu.vram_accessible)
      {
         ppu.vdata_latch = 0xFF;
         nofrendo_log_printf("VRAM read at $%04X, scanline %d\n", 
                    ppu.vaddr, nes_getcontextptr()->scanline);
      }
      else
      {
         uint32 addr = ppu.vaddr;
         if (addr >= 0x3000)
            addr -= 0x1000;
         if (ppu.chr_phase && addr < 0x2000)
            ppu.chr_phase(PPU_CHRPHASE_CPU);
         ppu.vdata_latch = PPU_MEM(addr);
      }

      /* v1.02: reads of $3F00-$3FFF return the palette immediately; the
       * buffer (above) still receives the name table byte underneath. */
      if (0x3F00 == (ppu.vaddr & 0x3F00))
         value = ppu.latch = (uint8) (ppu.palette[ppu.vaddr & 0x1F] & 0x3F);

      ppu.vaddr += ppu.vaddr_inc;
      ppu.vaddr &= 0x3FFF;
      break;

   case PPU_OAMDATA:
      /* v1.02: $2004 returns the OAM byte; attribute bits 2-4 read as 0. */
      value = ppu.oam[ppu.oam_addr];
      if (2 == (ppu.oam_addr & 3))
         value &= 0xE3;
      ppu.latch = value;
      break;

   case PPU_CTRL0:
   case PPU_CTRL1:
   case PPU_OAMADDR:
   case PPU_SCROLL:
   case PPU_VADDR:
   default:
      value = ppu.latch;
      break;
   }

   return value;
}

/* Write to $2000-$2007 */
void ppu_write(uint32 address, uint8 value)
{
   /* write goes into ppu latch... */
   ppu.latch = value;
   
   switch (address & 0x2007)
   {
   case PPU_CTRL0:
      ppu.ctrl0 = value;

      if (ppu.obj_height != ((value & PPU_CTRL0F_OBJ16) ? 16 : 8))
         ppu_invalidate_oam();
      ppu.obj_height = (value & PPU_CTRL0F_OBJ16) ? 16 : 8;
      ppu.bg_base = (value & PPU_CTRL0F_BGADDR) ? 0x1000 : 0;
      ppu.obj_base = (value & PPU_CTRL0F_OBJADDR) ? 0x1000 : 0;
      ppu.vaddr_inc = (value & PPU_CTRL0F_ADDRINC) ? 32 : 1;
      ppu.tile_nametab = value & PPU_CTRL0F_NAMETAB;      

      /* Mask out bits 10 & 11 in the ppu latch */
      ppu.vaddr_latch &= ~0x0C00;
      ppu.vaddr_latch |= ((value & 3) << 10);
      break;

   case PPU_CTRL1:
      ppu.ctrl1 = value;

      ppu.obj_on = (value & PPU_CTRL1F_OBJON) ? true : false;
      ppu.bg_on = (value & PPU_CTRL1F_BGON) ? true : false;
      ppu.obj_mask = (value & PPU_CTRL1F_OBJMASK) ? false : true;
      ppu.bg_mask = (value & PPU_CTRL1F_BGMASK) ? false : true;
      break;

   case PPU_OAMADDR:
      ppu.oam_addr = value;
      break;

   case PPU_OAMDATA:
      ppu.oam[ppu.oam_addr++] = value;
      ppu_invalidate_oam();
      break;

   case PPU_SCROLL:
      if (0 == ppu.flipflop)
      {
         /* Mask out bits 4 - 0 in the ppu latch */
         ppu.vaddr_latch &= ~0x001F;
         ppu.vaddr_latch |= (value >> 3);    /* Tile number */
         ppu.tile_xofs = (value & 7);  /* Tile offset (0-7 pix) */
      }
      else
      {
         /* Mask out bits 14-12 and 9-5 in the ppu latch */
         ppu.vaddr_latch &= ~0x73E0;
         ppu.vaddr_latch |= ((value & 0xF8) << 2);   /* Tile number */
         ppu.vaddr_latch |= ((value & 7) << 12);     /* Tile offset (0-7 pix) */
      }

      ppu.flipflop ^= 1;

      break;

   case PPU_VADDR:
      if (0 == ppu.flipflop)
      {
         /* Mask out bits 15-8 in ppu latch */
         ppu.vaddr_latch &= ~0xFF00;
         ppu.vaddr_latch |= ((value & 0x3F) << 8);
      }
      else
      {
         /* Mask out bits 7-0 in ppu latch */
         ppu.vaddr_latch &= ~0x00FF;
         ppu.vaddr_latch |= value;
         ppu.vaddr = ppu.vaddr_latch;
      }
      
      ppu.flipflop ^= 1;

      break;

   case PPU_VDATA:
      if (ppu.vaddr < 0x3F00)
      {
         /* queued lines still read this memory through page pointers */
         ppu_async_drain();
         /* VRAM only accessible during scanlines 241-260 */
         if ((ppu.bg_on || ppu.obj_on) && !ppu.vram_accessible)
         {
            nofrendo_log_printf("VRAM write to $%04X, scanline %d\n", 
                       ppu.vaddr, nes_getcontextptr()->scanline);
            if (!ppu_pattern_page_readonly(ppu.vaddr))
               PPU_MEM(ppu.vaddr) = 0xFF; /* corrupt */
         }
         else 
         {
            uint32 addr = ppu.vaddr;

            /* v1.02: $3000-$3EFF mirrors the name tables through page[12..15];
             * the old code also moved ppu.vaddr itself down by $1000, which
             * corrupted the address of every following $2007 access. */
            if (ppu.chr_phase && addr < 0x2000)
               ppu.chr_phase(PPU_CHRPHASE_CPU);
            if (!ppu_pattern_page_readonly(addr))
               PPU_MEM(addr) = value;
         }
      }
      else
      {
         if (0 == (ppu.vaddr & 0x0F))
         {
            int i;

            for (i = 0; i < 8; i ++)
               ppu.palette[i << 2] = (value & 0x3F) | BG_TRANS;
         }
         else if (ppu.vaddr & 3)
         {
            ppu.palette[ppu.vaddr & 0x1F] = value & 0x3F;
         }
      }

      ppu.vaddr += ppu.vaddr_inc;
      ppu.vaddr &= 0x3FFF;
      break;

   default:
      break;
   }
}

/* Builds a 256 color 8-bit palette based on a 64-color NES palette
** Note that we set it up 3 times so that we flip bits on the primary
** NES buffer for priorities
*/
static void ppu_buildpalette(ppu_t *src_ppu, rgb_t *pal)
{
   int i;

   /* Set it up 3 times, for sprite priority/BG transparency trickery */
   for (i = 0; i < 64; i++)
   {
      src_ppu->curpal[i].r = src_ppu->curpal[i + 64].r 
                           = src_ppu->curpal[i + 128].r = pal[i].r;
      src_ppu->curpal[i].g = src_ppu->curpal[i + 64].g
                           = src_ppu->curpal[i + 128].g = pal[i].g;
      src_ppu->curpal[i].b = src_ppu->curpal[i + 64].b
                           = src_ppu->curpal[i + 128].b = pal[i].b;
   } 
}

/* build the emulator specific palette based on a 64-entry palette
** input palette can be either nes_palette or a 64-entry RGB palette
** read in from disk (i.e. for VS games)
*/
void ppu_setpal(ppu_t *src_ppu, rgb_t *pal)
{
   ppu_buildpalette(src_ppu, pal);
   vid_setpalette(src_ppu->curpal);
}

void ppu_setdefaultpal(ppu_t *src_ppu)
{
   ppu_setpal(src_ppu, nes_palette);
}

void ppu_setlatchfunc(ppulatchfunc_t func)
{
   ppu.latchfunc = func;
}

void ppu_setvromswitch(ppuvromswitch_t func)
{
   ppu.vromswitch = func;
}

void ppu_setchrphasefunc(ppuchrphase_t func)
{
   ppu.chr_phase = func;
}

/* Copy eight 1 KiB CHR page pointers ($0000-$1FFF) in one go. */
NES_HOT void ppu_setchrpages(uint8 *const pages[8])
{
   ppu.page[0] = pages[0]; ppu.page[1] = pages[1];
   ppu.page[2] = pages[2]; ppu.page[3] = pages[3];
   ppu.page[4] = pages[4]; ppu.page[5] = pages[5];
   ppu.page[6] = pages[6]; ppu.page[7] = pages[7];
}

void ppu_setexattr(const uint8 *exram, const uint8 *(*chr4k)(uint32 bank4k))
{
   ppu.exattr = (exram && chr4k) ? exram : NULL;
   ppu.exchr = chr4k;
}

/* The live nametable RAM is the static PPU's, not the heap ppu_t's. */
uint8 *ppu_getnametab(void)
{
   return ppu.nametab;
}

bool ppu_obj16(void)
{
   return 16 == ppu.obj_height;
}

/* rendering routines */
INLINE void draw_bgtile(uint8 *surface, uint8 pat1, uint8 pat2, 
                        const uint8 *colors)
{
   uint32 pattern = ((pat2 & 0xAA) << 8) | ((pat2 & 0x55) << 1)
                    | ((pat1 & 0xAA) << 7) | (pat1 & 0x55);
   
   *surface++ = colors[(pattern >> 14) & 3];
   *surface++ = colors[(pattern >> 6) & 3];
   *surface++ = colors[(pattern >> 12) & 3];
   *surface++ = colors[(pattern >> 4) & 3];
   *surface++ = colors[(pattern >> 10) & 3];
   *surface++ = colors[(pattern >> 2) & 3];
   *surface++ = colors[(pattern >> 8) & 3];
   *surface = colors[pattern & 3];
}

INLINE int draw_oamtile(uint8 *surface, uint8 attrib, uint8 pat1, 
                        uint8 pat2, const uint8 *col_tbl, bool check_strike)
{
   int strike_pixel = -1;
   uint32 color = ((pat2 & 0xAA) << 8) | ((pat2 & 0x55) << 1)
                  | ((pat1 & 0xAA) << 7) | (pat1 & 0x55);

   /* sprite is not 100% transparent */
   if (color)
   {
      uint8 colors[8];

      /* swap pixels around if our tile is flipped */
      if (0 == (attrib & OAMF_HFLIP))
      {
         colors[0] = (color >> 14) & 3;
         colors[1] = (color >> 6) & 3;
         colors[2] = (color >> 12) & 3;
         colors[3] = (color >> 4) & 3;
         colors[4] = (color >> 10) & 3;
         colors[5] = (color >> 2) & 3;
         colors[6] = (color >> 8) & 3;
         colors[7] = color & 3;
      }
      else
      {
         colors[7] = (color >> 14) & 3;
         colors[6] = (color >> 6) & 3;
         colors[5] = (color >> 12) & 3;
         colors[4] = (color >> 4) & 3;
         colors[3] = (color >> 10) & 3;
         colors[2] = (color >> 2) & 3;
         colors[1] = (color >> 8) & 3;
         colors[0] = color & 3;
      }

      /* check for solid sprite pixel overlapping solid bg pixel */
      if (check_strike)
      {
         if (colors[0] && BG_SOLID(surface[0]))
            strike_pixel = 0;
         else if (colors[1] && BG_SOLID(surface[1]))
            strike_pixel = 1;
         else if (colors[2] && BG_SOLID(surface[2]))
            strike_pixel = 2;
         else if (colors[3] && BG_SOLID(surface[3]))
            strike_pixel = 3;
         else if (colors[4] && BG_SOLID(surface[4]))
            strike_pixel = 4;
         else if (colors[5] && BG_SOLID(surface[5]))
            strike_pixel = 5;
         else if (colors[6] && BG_SOLID(surface[6]))
            strike_pixel = 6;
         else if (colors[7] && BG_SOLID(surface[7]))
            strike_pixel = 7;
      }

      /* draw the character */
      if (attrib & OAMF_BEHIND)
      {
         if (colors[0])
            surface[0] = SP_PIXEL | (BG_CLEAR(surface[0]) ? col_tbl[colors[0]] : surface[0]);
         if (colors[1])
            surface[1] = SP_PIXEL | (BG_CLEAR(surface[1]) ? col_tbl[colors[1]] : surface[1]);
         if (colors[2])
            surface[2] = SP_PIXEL | (BG_CLEAR(surface[2]) ? col_tbl[colors[2]] : surface[2]);
         if (colors[3])
            surface[3] = SP_PIXEL | (BG_CLEAR(surface[3]) ? col_tbl[colors[3]] : surface[3]);
         if (colors[4])
            surface[4] = SP_PIXEL | (BG_CLEAR(surface[4]) ? col_tbl[colors[4]] : surface[4]);
         if (colors[5])
            surface[5] = SP_PIXEL | (BG_CLEAR(surface[5]) ? col_tbl[colors[5]] : surface[5]);
         if (colors[6])
            surface[6] = SP_PIXEL | (BG_CLEAR(surface[6]) ? col_tbl[colors[6]] : surface[6]);
         if (colors[7])
            surface[7] = SP_PIXEL | (BG_CLEAR(surface[7]) ? col_tbl[colors[7]] : surface[7]);
      }
      else
      {
         if (colors[0] && SP_CLEAR(surface[0]))
            surface[0] = SP_PIXEL | col_tbl[colors[0]];
         if (colors[1] && SP_CLEAR(surface[1]))
            surface[1] = SP_PIXEL | col_tbl[colors[1]];
         if (colors[2] && SP_CLEAR(surface[2]))
            surface[2] = SP_PIXEL | col_tbl[colors[2]];
         if (colors[3] && SP_CLEAR(surface[3]))
            surface[3] = SP_PIXEL | col_tbl[colors[3]];
         if (colors[4] && SP_CLEAR(surface[4]))
            surface[4] = SP_PIXEL | col_tbl[colors[4]];
         if (colors[5] && SP_CLEAR(surface[5]))
            surface[5] = SP_PIXEL | col_tbl[colors[5]];
         if (colors[6] && SP_CLEAR(surface[6]))
            surface[6] = SP_PIXEL | col_tbl[colors[6]];
         if (colors[7] && SP_CLEAR(surface[7]))
            surface[7] = SP_PIXEL | col_tbl[colors[7]];
      }
   }

   return strike_pixel;
}

/* OAM entry */
typedef struct obj_s
{
   uint8 y_loc;
   uint8 tile;
   uint8 atr;
   uint8 x_loc;
} obj_t;

/* ------------------------------------------------------------------------
 * v0.1.10: per-scanline render state.
 * Everything the renderer needs for one line is gathered in a ppu_line_t, so
 * a line can be drawn later and on the other CPU core (see the async queue
 * below).  Registers, palette and the line's sprites are copied; pattern and
 * nametable memory is reached through page pointers.  In synchronous mode
 * the pointers are the live ppu.page[] (mapper latches such as MMC2 change
 * them in the middle of a line); in asynchronous mode they are copies.
 * ------------------------------------------------------------------------ */
#define  PPUL_BG_ON     0x01
#define  PPUL_OBJ_ON    0x02
#define  PPUL_BG_MASK   0x04     /* blank the left 8 background pixels */
#define  PPUL_OBJ_MASK  0x08     /* hide sprites in the left 8 pixels */
#define  PPUL_SYNC      0x10     /* drawn by the CPU core, legacy semantics */

typedef struct ppu_line_s
{
   uint8 *const *chr_bg;      /* 8 pages: pattern data for background fetches */
   uint8 *const *chr_obj;     /* 8 pages: pattern data for sprite fetches */
   uint8 *const *nt;          /* 4 pages: $2000-$2FFF */
   const uint8 *palette;      /* 32 entries */
   uint32 vaddr, bg_base, obj_base;
   int scanline;
   uint8 tile_xofs, obj_height, nsprites, flags;
   uint8 sprite_num[PPU_MAXSPRITE];
   obj_t sprite[PPU_MAXSPRITE];
} ppu_line_t;

typedef struct ppu_job_s
{
   ppu_line_t l;
   uint8 *pg_bg[8], *pg_obj[8], *pg_nt[4];
   uint8 pal[32];
} ppu_job_t;

#define  L_BG(s, x)     ((s)->chr_bg[(x) >> 10][(x)])
#define  L_OBJ(s, x)    ((s)->chr_obj[(x) >> 10][(x)])
#define  L_NT(s, x)     ((s)->nt[((x) >> 10) & 3][(x)])
#define  L_FULLBG(s)    ((s)->palette[0] | BG_TRANS)

/* Blank the left-hand 8 pixels when $2001 bit 1 is clear. */
static NES_HOT void ppu_bgmask_leftcolumn(const ppu_line_t *s, uint8 *vidbuf)
{
   uint32 *buf_ptr = (uint32 *) vidbuf;
   const uint32 fullbg = L_FULLBG(s);
   uint32 bg_clear = fullbg | fullbg << 8 | fullbg << 16 | fullbg << 24;

   buf_ptr[0] = bg_clear;
   buf_ptr[1] = bg_clear;
}

/* MMC5 extended attribute mode ($5104 = 1): every background tile takes its
 * palette (bits 6-7) and a 4 KiB CHR bank (bits 0-5, plus $5130) from the
 * ExRAM byte at the same position of the nametable, so $2000 bit 4 and the
 * attribute table are ignored.  Synchronous lines only. */
static NES_HOT void ppu_renderbg_exattr(const ppu_line_t *s, uint8 *bmp_ptr)
{
   const uint8 *ex = ppu.exattr;
   uint32 refresh_vaddr = 0x2000 + (s->vaddr & 0x0FE0);
   uint32 x_tile = s->vaddr & 0x1F;
   const uint32 fine_y = (s->vaddr >> 12) & 7;
   int tile_count = 33;

   while (tile_count--)
   {
      const uint32 off = (refresh_vaddr & 0x03E0) + x_tile;
      const uint8 tile_index = L_NT(s, refresh_vaddr + x_tile);
      const uint8 e = ex[off];
      const uint8 *data = ppu.exchr((uint32) (e & 0x3F)) + ((uint32) tile_index << 4) + fine_y;

      nes_bg_lut_draw(&bg_lut, bmp_ptr, data[0], data[8], e >> 6);
      bmp_ptr += 8;

      if (32 == ++x_tile)
      {
         x_tile = 0;
         refresh_vaddr ^= (1 << 10);   /* switch nametable */
      }
   }
}

static NES_HOT void ppu_renderbg(const ppu_line_t *s, uint8 *vidbuf)
{
   uint8 *bmp_ptr;
   const uint8 *data_ptr, *tile_ptr, *attrib_ptr;
   uint32 refresh_vaddr, bg_offset, attrib_base;
   int tile_count;
   uint8 tile_index, x_tile, y_tile;
   uint8 col_high, attrib, attrib_shift;
   const ppulatchfunc_t latchfunc = (s->flags & PPUL_SYNC) ? ppu.latchfunc : NULL;

   /* draw a line of transparent background color if bg is disabled */
   if (0 == (s->flags & PPUL_BG_ON))
   {
      memset(vidbuf, L_FULLBG(s), NES_SCREEN_WIDTH);
      return;
   }

   nes_bg_lut_update(&bg_lut, s->palette);
   bmp_ptr = vidbuf - s->tile_xofs; /* scroll x; left safety margin provided */

   if ((s->flags & PPUL_SYNC) && ppu.exattr)
   {
      ppu_renderbg_exattr(s, bmp_ptr);
      if (s->flags & PPUL_BG_MASK)
         ppu_bgmask_leftcolumn(s, vidbuf);
      return;
   }

   refresh_vaddr = 0x2000 + (s->vaddr & 0x0FE0); /* mask out x tile */
   x_tile = s->vaddr & 0x1F;
   y_tile = (s->vaddr >> 5) & 0x1F; /* to simplify calculations */
   bg_offset = ((s->vaddr >> 12) & 7) + s->bg_base; /* offset in y tile */

   /* calculate initial values */
   tile_ptr = &L_NT(s, refresh_vaddr + x_tile); /* pointer to tile index */
   attrib_base = (refresh_vaddr & 0x2C00) + 0x3C0 + ((y_tile & 0x1C) << 1);
   attrib_ptr = &L_NT(s, attrib_base + (x_tile >> 2));
   attrib = *attrib_ptr++;
   attrib_shift = (x_tile & 2) + ((y_tile & 2) << 1);
   col_high = ((attrib >> attrib_shift) & 3) << 2;

   /* ppu fetches 33 tiles */
   tile_count = 33;
   while (tile_count--)
   {
      /* Tile number from nametable */
      tile_index = *tile_ptr++;
      data_ptr = &L_BG(s, bg_offset + (tile_index << 4));

      /* Handle $FD/$FE tile VROM switching (PunchOut) */
      if (latchfunc)
         latchfunc(s->bg_base, tile_index);

      nes_bg_lut_draw(&bg_lut, bmp_ptr, data_ptr[0], data_ptr[8], col_high >> 2);
      bmp_ptr += 8;

      x_tile++;

      if (0 == (x_tile & 1))     /* check every 2 tiles */
      {
         if (0 == (x_tile & 3))  /* check every 4 tiles */
         {
            if (32 == x_tile)    /* check every 32 tiles */
            {
               x_tile = 0;
               refresh_vaddr ^= (1 << 10); /* switch nametable */
               attrib_base ^= (1 << 10);

               /* recalculate pointers */
               tile_ptr = &L_NT(s, refresh_vaddr);
               attrib_ptr = &L_NT(s, attrib_base);
            }

            /* Get the attribute byte */
            attrib = *attrib_ptr++;
         }

         attrib_shift ^= 2;
         col_high = ((attrib >> attrib_shift) & 3) << 2;
      }
   }

   /* Blank left hand column if need be */
   if (s->flags & PPUL_BG_MASK)
      ppu_bgmask_leftcolumn(s, vidbuf);
}

/* v0.1.9: per-scanline sprite lists.
 * The old renderer scanned all 64 OAM entries on each of the 240 lines (15k
 * range checks per frame).  OAM normally changes once per frame (DMA), so the
 * lists are rebuilt only when OAM or the sprite height changed and each line
 * then visits just its own sprites, in OAM order and with the same 8-per-line
 * cut-off as the scan.  If a game rewrites OAM during rendering the lists are
 * rebuilt at most a few times per frame; after that this frame falls back to
 * the legacy scan, so the worst case is never worse than before. */
static uint8 oam_line_cnt[240];
static uint8 oam_line_over[240];   /* v1.02: a 9th sprite exists on this line */
static uint8 oam_line_list[240][PPU_MAXSPRITE];
static bool oam_lists_valid = false;
static uint8 oam_rebuilds = 0;

#ifndef PPU_OAM_REBUILDS_PER_FRAME
#define  PPU_OAM_REBUILDS_PER_FRAME  4
#endif

static void ppu_invalidate_oam(void)
{
   oam_lists_valid = false;
}

static NES_HOT void ppu_build_oam_lists(void)
{
   const obj_t *spr = (const obj_t *) ppu.oam;
   const int height = ppu.obj_height;
   int sprite_num;

   memset(oam_line_cnt, 0, sizeof(oam_line_cnt));
   memset(oam_line_over, 0, sizeof(oam_line_over));

   for (sprite_num = 0; sprite_num < 64; sprite_num++, spr++)
   {
      const uint8 sprite_y = spr->y_loc + 1;     /* wraps like the scanline test */
      int line, last;

      if (0 == sprite_y || sprite_y >= 240)
         continue;

      last = sprite_y + height - 1;
      if (last > 239)
         last = 239;

      for (line = sprite_y; line <= last; line++)
      {
         if (oam_line_cnt[line] < PPU_MAXSPRITE)
            oam_line_list[line][oam_line_cnt[line]++] = (uint8) sprite_num;
         else
            oam_line_over[line] = 1;
      }
   }

   oam_lists_valid = true;
}

/* Copy this line's sprites (at most 8, in OAM order) into the line state and
 * raise the sprite-overflow flag exactly like the old renderer.  CPU core. */
static NES_HOT void ppu_collect_sprites(ppu_line_t *l, int scanline)
{
   const obj_t *oam = (const obj_t *) ppu.oam;
   int count = 0;
   bool overflow = false;

   l->nsprites = 0;
   if (false == ppu.obj_on)
      return;

   if (!oam_lists_valid && oam_rebuilds < PPU_OAM_REBUILDS_PER_FRAME)
   {
      ppu_build_oam_lists();
      oam_rebuilds++;
   }

   if (oam_lists_valid)
   {
      const uint8 *list = oam_line_list[scanline];
      int i;

      count = oam_line_cnt[scanline];
      overflow = (0 != oam_line_over[scanline]);
      for (i = 0; i < count; i++)
      {
         l->sprite_num[i] = list[i];
         l->sprite[i] = oam[list[i]];
      }
   }
   else
   {
      /* legacy full scan (OAM rewritten many times in one frame) */
      const uint8 sprite_height = ppu.obj_height;
      int sprite_num;

      for (sprite_num = 0; sprite_num < 64; sprite_num++)
      {
         const uint8 sprite_y = oam[sprite_num].y_loc + 1;

         /* Check to see if sprite is out of range */
         if ((sprite_y > scanline) || (sprite_y <= (scanline - sprite_height))
             || (0 == sprite_y) || (sprite_y >= 240))
            continue;

         if (PPU_MAXSPRITE == count)
         {
            overflow = true;             /* a 9th sprite on this line */
            break;
         }
         l->sprite_num[count] = (uint8) sprite_num;
         l->sprite[count] = oam[sprite_num];
         count++;
      }
   }

   l->nsprites = (uint8) count;

   /* v1.02: the flag needs a 9th sprite (not just 8) and, like the real
    * PPU, stays set until the pre-render line (see ppu_scanline). */
   if (overflow)
      ppu.stat |= PPU_STATF_MAXSPRITE;
}

/* Pattern row of a sprite on the line (shared by drawing and strike test). */
static inline __attribute__((always_inline)) const uint8 *ppu_sprite_row(const ppu_line_t *s, const obj_t *sprite_ptr)
{
   const uint8 *data_ptr;
   uint32 vram_adr;
   int y_offset;
   const uint8 tile_index = sprite_ptr->tile;
   const uint8 sprite_y = sprite_ptr->y_loc + 1;

   /* 8x16 even sprites use $0000, odd use $1000 */
   if (16 == s->obj_height)
      vram_adr = ((tile_index & 1) << 12) | ((tile_index & 0xFE) << 4);
   else
      vram_adr = s->obj_base + (tile_index << 4);

   /* Get the address of the tile */
   data_ptr = &L_OBJ(s, vram_adr);

   /* Calculate offset (line within the sprite) */
   y_offset = s->scanline - sprite_y;
   if (y_offset > 7)
      y_offset += 8;

   /* Account for vertical flippage */
   if (sprite_ptr->atr & OAMF_VFLIP)
   {
      if (16 == s->obj_height)
         y_offset -= 23;
      else
         y_offset -= 7;

      data_ptr -= y_offset;
   }
   else
   {
      data_ptr += y_offset;
   }
   return data_ptr;
}

/* Sprite-0 strike computed on the CPU core straight from the line state, for
 * lines that are drawn asynchronously.  Same rule as the drawing code: the
 * first opaque sprite pixel (left to right) that lies over a background
 * pixel whose palette entry is not flagged transparent.  Returns the pixel
 * index inside the sprite, or -1. */
static NES_HOT int ppu_strike_pixel(const ppu_line_t *s)
{
   const obj_t *sp;
   const uint8 *row;
   uint32 pat, refresh_vaddr, bg_offset;
   int i;

   if (0 == s->nsprites || 0 != s->sprite_num[0] || 0 == (s->flags & PPUL_BG_ON))
      return -1;

   sp = &s->sprite[0];
   row = ppu_sprite_row(s, sp);
   pat = (uint32) row[0] | (uint32) row[8];
   if (0 == pat)
      return -1;

   refresh_vaddr = 0x2000 + (s->vaddr & 0x0FE0);
   bg_offset = ((s->vaddr >> 12) & 7) + s->bg_base;

   for (i = 0; i < 8; i++)
   {
      const uint32 bit = (sp->atr & OAMF_HFLIP) ? (uint32) i : (uint32) (7 - i);
      uint32 x, px, col, nt_addr, c;
      const uint8 *data;
      uint8 tile;

      if (0 == ((pat >> bit) & 1))
         continue;

      x = (uint32) sp->x_loc + (uint32) i;
      if (x >= 255)
         break;                          /* x=255 and beyond never hit */
      if (x < 8 && (s->flags & (PPUL_BG_MASK | PPUL_OBJ_MASK)))
         continue;                       /* clipped left column: no hit */

      px = x + s->tile_xofs;
      col = (s->vaddr & 0x1F) + (px >> 3);
      nt_addr = refresh_vaddr;
      if (col >= 32)
      {
         col -= 32;
         nt_addr ^= (1 << 10);
      }
      tile = L_NT(s, nt_addr + col);
      data = &L_BG(s, bg_offset + ((uint32) tile << 4));
      c = ((data[0] >> (7 - (px & 7))) & 1) | (((data[8] >> (7 - (px & 7))) & 1) << 1);
      if (c)
         return i;
      else
      {
         /* color 0: solid only while its palette entry lacks the flag */
         const uint32 y_tile = (s->vaddr >> 5) & 0x1F;
         const uint32 attrib_addr = (nt_addr & 0x2C00) + 0x3C0 + ((y_tile & 0x1C) << 1) + (col >> 2);
         const uint32 shift = (col & 2) + ((y_tile & 2) << 1);
         const uint32 group = (L_NT(s, attrib_addr) >> shift) & 3;
         if (0 == (s->palette[group << 2] & BG_TRANS))
            return i;
      }
   }
   return -1;
}

/* Draw one sprite of the line. */
static NES_HOT void ppu_draw_sprite_line(const ppu_line_t *s, uint8 *buf_ptr, int index)
{
   const obj_t *sprite_ptr = &s->sprite[index];
   const uint8 *data_ptr;
   uint8 *bmp_ptr;
   const uint8 attrib = sprite_ptr->atr;
   uint8 col_high;
   bool check_strike;
   int strike_pixel;

   bmp_ptr = buf_ptr + sprite_ptr->x_loc;

   /* Handle $FD/$FE tile VROM switching (PunchOut) */
   if ((s->flags & PPUL_SYNC) && ppu.latchfunc)
      ppu.latchfunc(s->obj_base, sprite_ptr->tile);

   /* Get upper two bits of color */
   col_high = ((attrib & 3) << 2);

   data_ptr = ppu_sprite_row(s, sprite_ptr);

   /* Synchronous lines keep the legacy strike test inside the blitter;
   ** asynchronous lines were tested by ppu_strike_pixel() on the CPU core.
   */
   check_strike = (s->flags & PPUL_SYNC) && (0 == s->sprite_num[index]) && (false == ppu.strikeflag);
   // v0.1.17: sprite pixels beyond x=255 are outside the NES viewport.
   // Draw into a small local buffer when the sprite crosses the right edge,
   // then copy back only its visible pixels. This also blocks offscreen
   // sprite-0 strikes and prevents stale right-edge bytes being reused.
   // Normal sprites (x<=248) keep the original direct fast path.
   if (sprite_ptr->x_loc > 248)
   {
      uint8 edge[8];
      const unsigned visible = 256u - (unsigned)sprite_ptr->x_loc;
      memcpy(edge, bmp_ptr, 8);
      strike_pixel = draw_oamtile(edge, attrib, data_ptr[0], data_ptr[8],
                                  s->palette + 16 + col_high, check_strike);
      memcpy(bmp_ptr, edge, visible);
      if (strike_pixel >= (int)visible) strike_pixel = -1;
   }
   else
   {
      strike_pixel = draw_oamtile(bmp_ptr, attrib, data_ptr[0], data_ptr[8],
                                  s->palette + 16 + col_high, check_strike);
   }
#ifdef PPU_STRIKE_SELFTEST
   if (check_strike && sprite_ptr->x_loc <= 248 && strike_pixel != ppu_strike_pixel(s))
   {
      extern void ppu_selftest_fail(int line, int a, int b);
      ppu_selftest_fail(s->scanline, strike_pixel, ppu_strike_pixel(s));
   }
#endif
   if (strike_pixel >= 0)
      ppu_setstrike(strike_pixel);
}

static NES_HOT void ppu_renderoam(const ppu_line_t *s, uint8 *vidbuf)
{
   uint32 savecol[2];
   int i;

   if (0 == (s->flags & PPUL_OBJ_ON))
      return;

   /* Save left hand column? */
   if (s->flags & PPUL_OBJ_MASK)
   {
      savecol[0] = ((uint32 *) vidbuf)[0];
      savecol[1] = ((uint32 *) vidbuf)[1];
   }

   for (i = 0; i < s->nsprites; i++)
      ppu_draw_sprite_line(s, vidbuf, i);

   /* Restore lefthand column */
   if (s->flags & PPUL_OBJ_MASK)
   {
      ((uint32 *) vidbuf)[0] = savecol[0];
      ((uint32 *) vidbuf)[1] = savecol[1];
   }
}

/* Fake rendering a line */
/* This is needed for sprite 0 hits when we're skipping drawing a frame */
static void ppu_fakeoam(int scanline)
{
   uint8 *data_ptr;
   obj_t *sprite_ptr;
   uint32 vram_adr, color;
   int y_offset;
   uint8 pat1, pat2;
   uint8 tile_index, attrib;
   uint8 sprite_height, sprite_y, sprite_x;

   /* we don't need to be here if strike flag is set */

   if (false == ppu.obj_on || ppu.strikeflag)
      return;

   sprite_height = ppu.obj_height;
   sprite_ptr = (obj_t *) ppu.oam;
   sprite_y = sprite_ptr->y_loc + 1;

   /* Check to see if sprite is out of range */
   if ((sprite_y > scanline) || (sprite_y <= (scanline - sprite_height)) 
       || (0 == sprite_y) || (sprite_y > 240))
      return;

   sprite_x = sprite_ptr->x_loc;
   tile_index = sprite_ptr->tile;
   attrib = sprite_ptr->atr;

   /* 8x16 even sprites use $0000, odd use $1000 */
   if (16 == ppu.obj_height)
      vram_adr = ((tile_index & 1) << 12) | ((tile_index & 0xFE) << 4);
   else
      vram_adr = ppu.obj_base + (tile_index << 4);

   data_ptr = &PPU_MEM(vram_adr);

   /* Calculate offset (line within the sprite) */
   y_offset = scanline - sprite_y;
   if (y_offset > 7)
      y_offset += 8;

   /* Account for vertical flippage */
   if (attrib & OAMF_VFLIP)
   {
      if (16 == ppu.obj_height)
         y_offset -= 23;
      else
         y_offset -= 7;
      data_ptr -= y_offset;
   }
   else
   {
      data_ptr += y_offset;
   }

   /* check for a solid sprite 0 pixel */
   pat1 = data_ptr[0];
   pat2 = data_ptr[8];
   color = ((pat2 & 0xAA) << 8) | ((pat2 & 0x55) << 1)
                  | ((pat1 & 0xAA) << 7) | (pat1 & 0x55);

   if (color)
   {
      uint8 colors[8];

      /* buckle up, it's going to get ugly... */
      if (0 == (attrib & OAMF_HFLIP))
      {
         colors[0] = (color >> 14) & 3;
         colors[1] = (color >> 6) & 3;
         colors[2] = (color >> 12) & 3;
         colors[3] = (color >> 4) & 3;
         colors[4] = (color >> 10) & 3;
         colors[5] = (color >> 2) & 3;
         colors[6] = (color >> 8) & 3;
         colors[7] = color & 3;
      }
      else
      {
         colors[7] = (color >> 14) & 3;
         colors[6] = (color >> 6) & 3;
         colors[5] = (color >> 12) & 3;
         colors[4] = (color >> 4) & 3;
         colors[3] = (color >> 10) & 3;
         colors[2] = (color >> 2) & 3;
         colors[1] = (color >> 8) & 3;
         colors[0] = color & 3;
      }

      if (colors[0])
         ppu_setstrike(sprite_x + 0);
      else if (colors[1])
         ppu_setstrike(sprite_x + 1);
      else if (colors[2])
         ppu_setstrike(sprite_x + 2);
      else if (colors[3])
         ppu_setstrike(sprite_x + 3);
      else if (colors[4])
         ppu_setstrike(sprite_x + 4);
      else if (colors[5])
         ppu_setstrike(sprite_x + 5);
      else if (colors[6])
         ppu_setstrike(sprite_x + 6);
      else if (colors[7])
         ppu_setstrike(sprite_x + 7);
   }
}

bool ppu_enabled(void)
{
   return (ppu.bg_on || ppu.obj_on);
}

/* 8-pixel left guard: horizontal scrolling writes at buf-tile_xofs.
 * The legacy code used &line[0], corrupting unrelated data for xofs=1..7.
 * 320 bytes hold guard + 256 visible + 8 fetched pixels + spare. */
static uint8 line[320] __attribute__((aligned(4)));

/* ------------------------------------------------------------------------
 * v0.1.10: asynchronous line renderer.
 * The CPU core (producer) pushes one ppu_job_t per visible scanline into a
 * single-producer/single-consumer ring; a task on the other core (consumer)
 * draws background and sprites, converts and stores the line.  The producer
 * waits for the ring to empty ("drain") before anything that changes memory
 * the consumer reads through pointers: $2007 writes below $3F00, mapper RAM
 * used as nametables, reset, and the end of the frame.  Lines that need
 * mid-line mapper callbacks (MMC2/MMC4 latch, MMC5 extended attributes) are
 * drawn synchronously after a drain.  With the queue off, every line takes
 * the synchronous path, which is the v0.1.9 renderer unchanged.
 * ------------------------------------------------------------------------ */
static ppu_job_t *aq_jobs = NULL;
static uint32 aq_mask = 0, aq_count = 0;
static volatile uint32 aq_head = 0, aq_tail = 0;
static uint32 aq_notified = 0;
static void (*aq_wake)(void) = NULL;
static void (*aq_idle)(void) = NULL;
static volatile bool aq_on = false;
static volatile int aq_consuming = 0;

#ifndef PPU_ASYNC_BATCH
#define  PPU_ASYNC_BATCH   4        /* lines per consumer wake-up */
#endif
#define  PPU_ASYNC_SPINS   400000u  /* busy-wait before yielding a tick */

/* ------------------------------------------------------------------------
 * v0.1.11: CHR-ROM page cache in internal RAM (asynchronous lines only).
 * Hardware traces showed ~55 us per line on the drawing core: the cartridge
 * lives in PSRAM and that core's cache is flushed every frame by the line
 * stores and the stage->DMA copy, so almost every tile fetch stalled.  The
 * CPU core now translates each 1 KiB pattern page of a line to a copy held in
 * internal RAM (LRU, filled on first use).  Only CHR-ROM is cached: it never
 * changes.  A slot is overwritten only after the queue is drained, so no
 * queued line can still point at it.
 * ------------------------------------------------------------------------ */
#define  PPU_CHRCACHE_MAX   64
static uint8 *cc_mem = NULL;
static int cc_pages = 0;
static const uint8 *cc_key[PPU_CHRCACHE_MAX];
static uint32 cc_age[PPU_CHRCACHE_MAX];
static uint32 cc_clock = 0;
static const uint8 *cc_rom_lo = NULL, *cc_rom_hi = NULL;
static uint8 *cc_last_src[2][8], *cc_last_dst[2][8];
static uint8 cc_last_idx[2][8];
static uint32 cc_misses = 0;

void ppu_chrcache_init(void *memory, int pages)
{
   const rominfo_t *cart = mmc_getinfo();

   ppu_async_drain();
   cc_mem = NULL;
   cc_pages = 0;
   memset(cc_key, 0, sizeof(cc_key));
   memset(cc_age, 0, sizeof(cc_age));
   memset(cc_last_src, 0, sizeof(cc_last_src));
   memset(cc_last_dst, 0, sizeof(cc_last_dst));
   cc_clock = 0;
   cc_misses = 0;
   cc_rom_lo = cc_rom_hi = NULL;
   /* a line touches up to 16 distinct pages: never run with fewer slots */
   if (!memory || pages < 16 || !cart || !cart->vrom || cart->vrom_banks <= 0)
      return;
   if (pages > PPU_CHRCACHE_MAX)
      pages = PPU_CHRCACHE_MAX;
   cc_rom_lo = cart->vrom;
   cc_rom_hi = cart->vrom + (uint32) cart->vrom_banks * 8192u;
   cc_mem = (uint8 *) memory;
   cc_pages = pages;
}

uint32 ppu_chrcache_take_misses(void)
{
   const uint32 m = cc_misses;
   cc_misses = 0;
   return m;
}

/* Page pointer `page` serves pattern addresses i*1024..i*1024+1023. */
static NES_HOT uint8 *ppu_chrcache_translate(int set, int i, uint8 *page)
{
   const uint8 *phys;
   uint8 *out = page;

   if (page == cc_last_src[set][i])
   {
      if (cc_last_dst[set][i] != page)
         cc_age[cc_last_idx[set][i]] = cc_clock;
      return cc_last_dst[set][i];
   }

   phys = page + i * 1024;
   if (phys >= cc_rom_lo && phys + 1024 <= cc_rom_hi)
   {
      int k, slot = -1, victim = 0;

      for (k = 0; k < cc_pages; k++)
      {
         if (cc_key[k] == phys)
         {
            slot = k;
            break;
         }
         if (cc_age[k] < cc_age[victim])
            victim = k;
      }
      if (slot < 0)
      {
         slot = victim;
         ppu_async_drain();         /* nobody may still read this slot */
         memset(cc_last_src, 0, sizeof(cc_last_src));
         cc_key[slot] = phys;
         memcpy(cc_mem + slot * 1024, phys, 1024);
         cc_misses++;
      }
      cc_age[slot] = cc_clock;
      cc_last_idx[set][i] = (uint8) slot;
      out = cc_mem + slot * 1024 - i * 1024;
   }
   cc_last_src[set][i] = page;
   cc_last_dst[set][i] = out;
   return out;
}

/* Optional microsecond clock: measures how long the CPU core is held back by
 * a full queue (reported in the serial diagnostics). */
static uint32 (*aq_clock)(void) = NULL;
static uint32 aq_wait_us = 0;

void ppu_async_set_clock(uint32 (*now_us)(void))
{
   aq_clock = now_us;
}

uint32 ppu_async_take_wait_us(void)
{
   const uint32 w = aq_wait_us;
   aq_wait_us = 0;
   return w;
}

int ppu_async_jobsize(void)
{
   return (int) sizeof(ppu_job_t);
}

bool ppu_async_active(void)
{
   return aq_on;
}

/* true while the consumer is inside its drawing loop (see emu_DrawLine) */
int ppu_async_in_consumer(void)
{
   return aq_consuming;
}

NES_HOT void ppu_async_drain(void)
{
   uint32 h, spins = 0;

   if (!aq_on)
      return;
   h = aq_head;
   if (aq_notified != h)
   {
      aq_notified = h;
      aq_wake();
   }
   while (__atomic_load_n(&aq_tail, __ATOMIC_ACQUIRE) != h)
   {
      if (++spins > PPU_ASYNC_SPINS)
      {
         spins = 0;
         aq_wake();
         aq_idle();
      }
   }
}

bool ppu_async_start(void *memory, int slots, void (*wake)(void), void (*idle)(void))
{
   if (aq_on)
      return true;
   if (!memory || slots < 2 || (slots & (slots - 1)) || !wake || !idle)
      return false;
   aq_jobs = (ppu_job_t *) memory;
   aq_count = (uint32) slots;
   aq_mask = (uint32) slots - 1u;
   __atomic_store_n(&aq_head, 0u, __ATOMIC_RELAXED);
   __atomic_store_n(&aq_tail, 0u, __ATOMIC_RELAXED);
   aq_notified = 0;
   aq_wake = wake;
   aq_idle = idle;
   aq_on = true;
   return true;
}

void ppu_async_stop(void)
{
   if (!aq_on)
      return;
   ppu_async_drain();
   aq_on = false;
}

/* Draw one fully described line (either core). */
static NES_HOT void ppu_render_line(const ppu_line_t *s)
{
   uint8 *buf = &line[8]; // Keep eight pixels before the visible area.

   ppu_renderbg(s, buf);
   ppu_renderoam(s, buf);
   emu_DrawLine(buf, 256, 240, s->scanline);
}

/* Consumer side: draw everything that has been queued. */
NES_HOT void ppu_async_consume(void)
{
   uint32 t;

   if (!aq_jobs)
      return;
   aq_consuming = 1;
   t = __atomic_load_n(&aq_tail, __ATOMIC_RELAXED);
   for (;;)
   {
      const uint32 h = __atomic_load_n(&aq_head, __ATOMIC_ACQUIRE);
      if (t == h)
         break;
      while (t != h)
      {
         ppu_render_line(&aq_jobs[t & aq_mask].l);
         ++t;
         __atomic_store_n(&aq_tail, t, __ATOMIC_RELEASE);
      }
   }
   aq_consuming = 0;
}

static inline __attribute__((always_inline)) uint8 ppu_line_flags(void)
{
   return (uint8) ((ppu.bg_on ? PPUL_BG_ON : 0) | (ppu.obj_on ? PPUL_OBJ_ON : 0)
                 | (ppu.bg_mask ? PPUL_BG_MASK : 0) | (ppu.obj_mask ? PPUL_OBJ_MASK : 0));
}

static NES_HOT void ppu_renderscanline(bitmap_t *bmp, int scanline, bool draw_flag)
{
   /* start scanline - transfer ppu latch into vaddr */
   if (ppu.bg_on || ppu.obj_on)
   {
      if (0 == scanline)
      {
         ppu.vaddr = ppu.vaddr_latch;
      }
      else
      {
         ppu.vaddr &= ~0x041F;
         ppu.vaddr |= (ppu.vaddr_latch & 0x041F);
      }
   }

   if (draw_flag && ppu.drawsprites && aq_on && NULL == ppu.latchfunc && NULL == ppu.exattr)
   {
      /* ---- asynchronous line: describe it and hand it to the other core */
      const uint32 h = aq_head;
      ppu_job_t *job;
      ppu_line_t *l;
      uint32 spins = 0;
      int i;

      if ((uint32) (h - __atomic_load_n(&aq_tail, __ATOMIC_ACQUIRE)) >= aq_count)
      {
         const uint32 t0 = aq_clock ? aq_clock() : 0;

         while ((uint32) (h - __atomic_load_n(&aq_tail, __ATOMIC_ACQUIRE)) >= aq_count)
         {
            if (aq_notified != h)
            {
               aq_notified = h;
               aq_wake();
            }
            if (++spins > PPU_ASYNC_SPINS)
            {
               spins = 0;
               aq_wake();
               aq_idle();
            }
         }
         if (aq_clock)
            aq_wait_us += (uint32) (aq_clock() - t0);
      }

      job = &aq_jobs[h & aq_mask];
      l = &job->l;

      if (ppu.chr_phase)
         ppu.chr_phase(PPU_CHRPHASE_BG);
      if (cc_pages)
      {
         cc_clock++;
         for (i = 0; i < 8; i++)
            job->pg_bg[i] = ppu_chrcache_translate(0, i, ppu.page[i]);
      }
      else
      {
         for (i = 0; i < 8; i++)
            job->pg_bg[i] = ppu.page[i];
      }
      if (ppu.chr_phase)
      {
         ppu.chr_phase(PPU_CHRPHASE_OBJ);
         for (i = 0; i < 8; i++)
            job->pg_obj[i] = cc_pages ? ppu_chrcache_translate(1, i, ppu.page[i]) : ppu.page[i];
      }
      else
      {
         for (i = 0; i < 8; i++)
            job->pg_obj[i] = job->pg_bg[i];
      }
      for (i = 0; i < 4; i++)
         job->pg_nt[i] = ppu.page[8 + i];
      memcpy(job->pal, ppu.palette, 32);

      l->chr_bg = job->pg_bg;
      l->chr_obj = job->pg_obj;
      l->nt = job->pg_nt;
      l->palette = job->pal;
      l->vaddr = ppu.vaddr;
      l->bg_base = ppu.bg_base;
      l->obj_base = ppu.obj_base;
      l->scanline = scanline;
      l->tile_xofs = (uint8) ppu.tile_xofs;
      l->obj_height = ppu.obj_height;
      l->flags = ppu_line_flags();
      ppu_collect_sprites(l, scanline);

      /* the CPU can read the strike flag during this very scanline */
      if (ppu.obj_on && false == ppu.strikeflag)
      {
         const int strike_pixel = ppu_strike_pixel(l);
         if (strike_pixel >= 0)
            ppu_setstrike(strike_pixel);
      }

      if (ppu.chr_phase)
         ppu.chr_phase(PPU_CHRPHASE_CPU);

      __atomic_store_n(&aq_head, h + 1u, __ATOMIC_RELEASE);
      if ((uint32) (h + 1u - aq_notified) >= PPU_ASYNC_BATCH)
      {
         aq_notified = h + 1u;
         aq_wake();
      }
      return;
   }

   /* ---- synchronous line (v0.1.9 path) */
   {
      static ppu_line_t sl;
      uint8 *buf = &line[8]; // Keep eight pixels before the visible area.

      ppu_async_drain();   /* line[] and bg_lut are shared with the consumer */

      sl.chr_bg = ppu.page;
      sl.chr_obj = ppu.page;
      sl.nt = &ppu.page[8];
      sl.palette = ppu.palette;
      sl.vaddr = ppu.vaddr;
      sl.bg_base = ppu.bg_base;
      sl.obj_base = ppu.obj_base;
      sl.scanline = scanline;
      sl.tile_xofs = (uint8) ppu.tile_xofs;
      sl.obj_height = ppu.obj_height;
      sl.flags = (uint8) (ppu_line_flags() | PPUL_SYNC);
      sl.nsprites = 0;

      if (draw_flag)
      {
         if (ppu.chr_phase)
            ppu.chr_phase(PPU_CHRPHASE_BG);
         ppu_renderbg(&sl, buf);
      }

      /* TODO: fetch obj data 1 scanline before */
      if (ppu.chr_phase)
         ppu.chr_phase(PPU_CHRPHASE_OBJ);
      if (true == ppu.drawsprites && true == draw_flag)
      {
         ppu_collect_sprites(&sl, scanline);
         ppu_renderoam(&sl, buf);
      }
      else
         ppu_fakeoam(scanline);
      if (ppu.chr_phase)
         ppu.chr_phase(PPU_CHRPHASE_CPU);

      if (draw_flag)
         emu_DrawLine(buf, 256, 240, scanline);
   }
}


void ppu_endscanline(int scanline)
{
   /* modify vram address at end of scanline */
   if (scanline < 240 && (ppu.bg_on || ppu.obj_on))
   {
      int ytile;

      /* check for max 3 bit y tile offset */
      if (7 == (ppu.vaddr >> 12))
      {
         ppu.vaddr &= ~0x7000;      /* clear y tile offset */
         ytile = (ppu.vaddr >> 5) & 0x1F;

         if (29 == ytile)
         {
            ppu.vaddr &= ~0x03E0;   /* clear y tile */
            ppu.vaddr ^= 0x0800;    /* toggle nametable */
         }
         else if (31 == ytile)
         {
            ppu.vaddr &= ~0x03E0;   /* clear y tile */
         }
         else
         {
            ppu.vaddr += 0x20;      /* increment y tile */
         }
      }
      else
      {
         ppu.vaddr += 0x1000;       /* increment tile y offset */
      }
   }
}

void ppu_checknmi(void)
{
   if (ppu.ctrl0 & PPU_CTRL0F_NMI)
      nes_nmi();
}

NES_HOT void ppu_scanline(bitmap_t *bmp, int scanline, bool draw_flag)
{
   if (scanline < 240)
   {     
      if (0 == scanline)
         oam_rebuilds = 0;
      ppu_renderscanline(bmp, scanline, draw_flag);
   }
   else if (241 == scanline)
   {
      ppu.stat |= PPU_STATF_VBLANK;
      ppu.vram_accessible = true;
   }
   else if (261 == scanline)
   {
      ppu.stat &= ~(PPU_STATF_VBLANK | PPU_STATF_MAXSPRITE);
      ppu.strikeflag = false;
      ppu.strike_cycle = (uint32) -1;

      ppu.vram_accessible = false;
   }
}



/* Stuff for the OAM viewer */
static void draw_sprite(bitmap_t *bmp, int x, int y, uint8 tile_num, uint8 attrib)
{
   int line, height;
   int col_high, vram_adr;
   uint8 *vid, *data_ptr;

   vid = bmp->line[y] + x;

   /* Get upper two bits of color */
   col_high = ((attrib & 3) << 2);

   /* 8x16 even sprites use $0000, odd use $1000 */
   height = ppu.obj_height;
   if (16 == height)
      vram_adr = ((tile_num & 1) << 12) | ((tile_num & 0xFE) << 4);
   /* else just use the offset from $2000 */
   else
      vram_adr = ppu.obj_base + (tile_num << 4);

   data_ptr = &PPU_MEM(vram_adr);

   for (line = 0; line < height; line++)
   {
      if (line == 8)
         data_ptr += 8;

      draw_bgtile(vid, data_ptr[0], data_ptr[8], ppu.palette + 16 + col_high);
      //draw_oamtile(vid, attrib, data_ptr[0], data_ptr[8], ppu.palette + 16 + col_high);

      data_ptr++;
      vid += bmp->pitch;
   }  
}

/* The MCUME fork retained debugger calls to these helpers without the
 * helpers themselves.  Provide bounded bitmap drawing so that this source
 * links even if a future front end enables the OAM diagnostic overlay. */
static void draw_box(bitmap_t *bmp, int x, int y, int height)
{
   if (!bmp) return;
   for (int j = 0; j < height + 2; ++j)
   {
      int yy = y + j;
      if (yy < 0 || yy >= bmp->height || !bmp->line[yy]) continue;
      for (int i = 0; i < 10; ++i)
      {
         int xx = x + i;
         if (xx >= 0 && xx < bmp->width &&
             (i == 0 || i == 9 || j == 0 || j == height + 1))
            bmp->line[yy][xx] = 0x3F;
      }
   }
}
static void draw_deadsprite(bitmap_t *bmp, int x, int y, int height)
{
   if (!bmp) return;
   for (int j = 0; j < height; ++j)
   {
      int yy = y + j;
      if (yy < 0 || yy >= bmp->height || !bmp->line[yy]) continue;
      int xa = x + (j % 8), xb = x + 7 - (j % 8);
      if (xa >= 0 && xa < bmp->width) bmp->line[yy][xa] = 0x3F;
      if (xb >= 0 && xb < bmp->width) bmp->line[yy][xb] = 0x3F;
   }
}

void ppu_dumpoam(bitmap_t *bmp, int x_loc, int y_loc)
{
   int sprite, x_pos, y_pos, height;
   obj_t *spr_ptr;

   spr_ptr = (obj_t *) ppu.oam;
   height = ppu.obj_height;

   for (sprite = 0; sprite < 64; sprite++)
   {
      x_pos = ((sprite & 0x0F) << 3) + (sprite & 0x0F) + x_loc;
      if (height == 16)
         y_pos = (sprite & 0xF0) + (sprite >> 4) + y_loc;
      else
         y_pos = ((sprite & 0xF0) >> 1) + (sprite >> 4) + y_loc;

      draw_box(bmp, x_pos, y_pos, height);

      if (spr_ptr->y_loc && spr_ptr->y_loc < 240)
         draw_sprite(bmp, x_pos + 1, y_pos + 1, spr_ptr->tile, spr_ptr->atr);
      else
         draw_deadsprite(bmp, x_pos + 1, y_pos + 1, height);

      spr_ptr++;
   }
}

/* More of a debugging thing than anything else */
void ppu_dumppattern(bitmap_t *bmp, int table_num, int x_loc, int y_loc, int col)
{
   int x_tile, y_tile;
   uint8 *bmp_ptr, *data_ptr, *ptr;
   int tile_num, line;
   uint8 col_high;

   tile_num = 0;
   col_high = col << 2;

   for (y_tile = 0; y_tile < 16; y_tile++)
   {
      /* Get our pointer to the bitmap */
      bmp_ptr = bmp->line[y_loc] + x_loc;

      for (x_tile = 0; x_tile < 16; x_tile++)
      {
         data_ptr = &PPU_MEM((table_num << 12) + (tile_num << 4));
         ptr = bmp_ptr;

         for (line = 0; line < 8; line ++)
         {
            draw_bgtile(ptr, data_ptr[0], data_ptr[8], ppu.palette + col_high);
            data_ptr++;
            ptr += bmp->pitch;
         }

         bmp_ptr += 8;
         tile_num++;
      }
      y_loc += 8;
   }
}

/*
** $Log: nes_ppu.c,v $
** Revision 1.2  2001/04/27 14:37:11  neil
** wheeee
**
** Revision 1.1.1.1  2001/04/27 07:03:54  neil
** initial
**
** Revision 1.14  2000/11/29 12:58:23  matt
** timing/fiq fixes
**
** Revision 1.13  2000/11/27 19:36:15  matt
** more timing fixes
**
** Revision 1.12  2000/11/26 15:51:13  matt
** frame IRQ emulation
**
** Revision 1.11  2000/11/25 20:30:39  matt
** scanline emulation simplifications/timing fixes
**
** Revision 1.10  2000/11/24 14:56:02  matt
** fixed a long-standing sprite 0 strike bug
**
** Revision 1.9  2000/11/20 13:23:17  matt
** PPU fixes
**
** Revision 1.8  2000/11/19 13:47:30  matt
** problem with frame irqs fixed
**
** Revision 1.7  2000/11/19 13:40:19  matt
** more accurate ppu behavior
**
** Revision 1.6  2000/11/14 12:09:37  matt
** only generate the palette once, please
**
** Revision 1.5  2000/11/11 14:51:43  matt
** context get/set fixed
**
** Revision 1.4  2000/11/09 12:35:50  matt
** fixed timing problem with VRAM reads/writes
**
** Revision 1.3  2000/11/05 16:35:41  matt
** rolled rgb.h into bitmap.h
**
** Revision 1.2  2000/10/27 12:55:03  matt
** palette generating functions now take *this pointers
**
** Revision 1.1  2000/10/24 12:20:28  matt
** changed directory structure
**
** Revision 1.33  2000/10/23 15:53:08  matt
** better system handling
**
** Revision 1.32  2000/10/22 15:02:32  matt
** simplified mirroring
**
** Revision 1.31  2000/10/21 21:36:04  matt
** ppu cleanups / fixes
**
** Revision 1.30  2000/10/21 19:26:59  matt
** many more cleanups
**
** Revision 1.29  2000/10/10 13:58:15  matt
** stroustrup squeezing his way in the door
**
** Revision 1.28  2000/10/08 17:54:32  matt
** reject VRAM access out of VINT period
**
** Revision 1.27  2000/09/15 04:58:07  matt
** simplifying and optimizing APU core
**
** Revision 1.26  2000/09/08 11:57:29  matt
** no more nes_fiq
**
** Revision 1.25  2000/09/07 21:57:31  matt
** api change
**
** Revision 1.24  2000/07/31 04:27:59  matt
** one million cleanups
**
** Revision 1.23  2000/07/30 06:13:12  matt
** default to no FIQs on startup
**
** Revision 1.22  2000/07/30 04:32:32  matt
** emulation of the NES frame IRQ
**
** Revision 1.21  2000/07/25 02:25:53  matt
** safer xxx_destroy calls
**
** Revision 1.20  2000/07/23 15:12:43  matt
** removed unused variables, changed INLINE
**
** Revision 1.19  2000/07/21 04:50:39  matt
** moved palette calls out of nofrendo.c and into ppu_create
**
** Revision 1.18  2000/07/17 05:12:55  matt
** nes_ppu.c is no longer a scary place to be-- cleaner & faster
**
** Revision 1.17  2000/07/17 01:52:28  matt
** made sure last line of all source files is a newline
**
** Revision 1.16  2000/07/11 04:42:39  matt
** updated for new screen dimension defines
**
** Revision 1.15  2000/07/10 19:10:16  matt
** should bomb out now if a game tries to write to VROM
**
** Revision 1.14  2000/07/10 05:28:30  matt
** moved joypad/oam dma from apu to ppu
**
** Revision 1.13  2000/07/10 03:03:16  matt
** added ppu_getcontext() routine
**
** Revision 1.12  2000/07/09 03:46:05  matt
** using pitch instead of width...
**
** Revision 1.11  2000/07/06 16:42:40  matt
** better palette setting interface
**
** Revision 1.10  2000/07/05 22:49:25  matt
** changed mmc2 (punchout) tile-access switching
**
** Revision 1.9  2000/07/04 23:13:26  matt
** added an irq line drawing debug feature hack
**
** Revision 1.8  2000/06/26 04:58:08  matt
** accuracy changes
**
** Revision 1.7  2000/06/22 02:13:49  matt
** more accurate emulation of $2002
**
** Revision 1.6  2000/06/20 20:42:47  matt
** accuracy changes
**
** Revision 1.5  2000/06/20 00:05:12  matt
** tested and verified STAT quirk, added code
**
** Revision 1.4  2000/06/09 15:12:26  matt
** initial revision
**
*/
