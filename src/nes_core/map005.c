/*
 * MMC5/ExROM mapper - v0.1.9 (rewrite of the v0.1.4 implementation).
 *
 * What was wrong before (Castlevania III showed scrambled graphics):
 *  - $5120-$512B hold bank numbers in units of the *selected CHR mode*
 *    (8K/4K/2K/1K).  The old code treated them as 1K numbers and divided
 *    them (>>3, >>2, >>1), so every CHR mode except 1K picked wrong tiles.
 *  - Only the "A" register set ($5120-$5127) was ever mapped.  With 8x16
 *    sprites the background must use the "B" set ($5128-$512B) while the
 *    sprites use set A.  Both views now exist and the PPU announces the
 *    kind of fetch through ppu_setchrphasefunc().
 *  - ExRAM: nametable source 2 returned the ExRAM even when $5104 made it
 *    unreadable, CPU writes ignored the "only while rendering" rule and
 *    extended attributes ($5104 = 1) did not exist.
 *  - PRG: bit 7 of $5114-$5116 (ROM/RAM) was ignored, so RAM windows were
 *    mapped to ROM; every bank write also copied the whole 6502 context.
 *  - IRQ: the scanline counter fired one line late.
 *
 * Still not emulated (documented limitation): vertical split screen
 * ($5200-$5202, used by very few games) and the exact PPU-cycle timing of the
 * scanline detector.  PRG-RAM is the 8 KiB Nofrendo SRAM (no 32/64/128 KiB
 * banking).  Nofrendo LGPL v2 - retain upstream LGPL copyright notices.
 */
#include <string.h>
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes.h"
#include "nes_ppu.h"
#include "nes6502.h"
#include "nes_hot.h"
#include "mmc5_snd.h"
#include "log.h"

/* Scanline on which $5203 == N raises the IRQ.  The scanline renderer draws
 * line L and only then runs the CPU for it.  1 (default): the IRQ is raised
 * right after line T is drawn, so the handler's CHR/PRG writes apply from line
 * T+1.  Verified on Castlevania III: the HUD/playfield border (line 47) is
 * clean.  0 raises it one line earlier (after line T-1) and leaves a stray row
 * of HUD pixels on that border in the same game. */
#ifndef MMC5_IRQ_AFTER_LINE_T
#define MMC5_IRQ_AFTER_LINE_T 1
#endif

static uint8 prg_mode, chr_mode, prg[5], chr[12], chr_high;
static uint8 nt_mode, fill_tile, fill_attr, exram_mode;
static uint8 exram[1024], fillram[1024], zeroram[1024];
static uint8 prg_protect1, prg_protect2;
static uint8 irq_target, irq_enable, irq_pending, in_frame, frame_active;
static uint8 scanline_counter;
static uint8 mul_a, mul_b;
static uint8 split_ctrl, split_scroll, split_bank;   /* stored, not emulated */
static uint8 last_chr_b;        /* last CHR register written was in set B */

/* The four 8 KiB CPU windows at $8000/$A000/$C000/$E000. */
static uint8 *win_ptr[4];
static uint8 win_is_ram[4];

/* CHR views: one 8 KiB pointer set per fetch kind. */
static uint8 *pg_a[8], *pg_b[8];
static uint8 chr_dirty;

/* ------------------------------------------------------------------ */
/* Nametables                                                          */
/* ------------------------------------------------------------------ */
static void nt_apply(void)
{
    uint8 *nametab = ppu_getnametab();
    if (!nametab) return;
    for (int i = 0; i < 4; i++) {
        const int kind = (nt_mode >> (i * 2)) & 3;
        uint8 *src = (kind == 0) ? nametab :
                     (kind == 1) ? nametab + 1024 :
                     (kind == 2) ? (exram_mode < 2 ? exram : zeroram) : fillram;
        ppu_setpage(1, 8 + i, src - ((8 + i) * 1024));
    }
    ppu_mirrorhipages();
}

static void refresh_fill(void)
{
    ppu_async_drain();          /* queued lines may still read fillram */
    memset(fillram, fill_tile, 960);
    memset(fillram + 960, (fill_attr & 3) * 0x55, 64);
}

/* ------------------------------------------------------------------ */
/* PRG                                                                 */
/* ------------------------------------------------------------------ */
static void map_window(unsigned win, int is_ram, unsigned bank8k)
{
    rominfo_t *cart = mmc_getinfo();
    uint8 *ptr;
    if (is_ram && cart->sram) {
        ptr = cart->sram;           /* only 8 KiB of PRG-RAM exist */
    } else {
        const unsigned total = (unsigned)cart->rom_banks * 2u;
        if (!total) return;
        ptr = cart->rom + (bank8k % total) * 8192u;
        is_ram = 0;
    }
    if (win_ptr[win] == ptr && win_is_ram[win] == (uint8)is_ram) return;
    win_ptr[win] = ptr;
    win_is_ram[win] = (uint8)is_ram;
    nes6502_setpage(8 + 2 * (int)win, ptr);
    nes6502_setpage(9 + 2 * (int)win, ptr + 4096);
}

static void prg_apply(void)
{
    /* Bit 7 of $5114-$5116 selects ROM (1) or PRG-RAM (0); $5117 is ROM. */
    switch (prg_mode) {
    case 0: {
        const unsigned b = prg[4] & 0x7c;
        map_window(0, 0, b);     map_window(1, 0, b + 1);
        map_window(2, 0, b + 2); map_window(3, 0, b + 3);
        break; }
    case 1: {
        const unsigned lo = prg[2] & 0x7e, hi = prg[4] & 0x7e;
        const int ram = !(prg[2] & 0x80);
        map_window(0, ram, lo);  map_window(1, ram, lo + 1);
        map_window(2, 0, hi);    map_window(3, 0, hi + 1);
        break; }
    case 2: {
        const unsigned lo = prg[2] & 0x7e;
        const int ram = !(prg[2] & 0x80);
        map_window(0, ram, lo);  map_window(1, ram, lo + 1);
        map_window(2, !(prg[3] & 0x80), prg[3] & 0x7f);
        map_window(3, 0, prg[4] & 0x7f);
        break; }
    default:
        map_window(0, !(prg[1] & 0x80), prg[1] & 0x7f);
        map_window(1, !(prg[2] & 0x80), prg[2] & 0x7f);
        map_window(2, !(prg[3] & 0x80), prg[3] & 0x7f);
        map_window(3, 0, prg[4] & 0x7f);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* CHR                                                                 */
/* ------------------------------------------------------------------ */
static void chr_snapshot(uint8 *dst[8])
{
    for (int i = 0; i < 8; i++) dst[i] = ppu_getpage(i);
}

/* Register values are in units of the CHR mode (8K/4K/2K/1K); $5130 adds
 * the two upper bits at that same granularity. */
static void chr_rebuild(void)
{
    const int hi = (int)chr_high << 8;
    chr_dirty = 0;

    /* ---- set A: $5120-$5127 (sprites, and everything with 8x8 sprites) */
    if (chr_mode == 0) {
        mmc_bankchr(8, 0, hi | chr[7]);
    } else if (chr_mode == 1) {
        mmc_bankchr(4, 0x0000, hi | chr[3]);
        mmc_bankchr(4, 0x1000, hi | chr[7]);
    } else if (chr_mode == 2) {
        for (int i = 0; i < 4; i++) mmc_bankchr(2, i * 2048, hi | chr[i * 2 + 1]);
    } else {
        for (int i = 0; i < 8; i++) mmc_bankchr(1, i * 1024, hi | chr[i]);
    }
    chr_snapshot(pg_a);

    /* ---- set B: $5128-$512B (background with 8x16 sprites), mirrored into
     *      both pattern tables.  Mode 2 uses $5129/$512B, modes 0/1 $512B. */
    if (chr_mode == 0) {
        mmc_bankchr(8, 0, hi | chr[11]);
    } else if (chr_mode == 1) {
        mmc_bankchr(4, 0x0000, hi | chr[11]);
        mmc_bankchr(4, 0x1000, hi | chr[11]);
    } else if (chr_mode == 2) {
        for (int i = 0; i < 4; i++) mmc_bankchr(2, i * 2048, hi | chr[8 + (i & 1) * 2 + 1]);
    } else {
        for (int i = 0; i < 8; i++) mmc_bankchr(1, i * 1024, hi | chr[8 + (i & 3)]);
    }
    chr_snapshot(pg_b);
}

/* Called by the PPU before background fetches, before sprite fetches and
 * after each scanline / before $2007 access to pattern space. */
static NES_HOT void map5_chrphase(int phase)
{
    if (chr_dirty) chr_rebuild();
    if (ppu_obj16()) {
        /* 8x16: background -> set B, sprites -> set A, CPU -> last written */
        ppu_setchrpages(phase == PPU_CHRPHASE_BG ? pg_b :
                        phase == PPU_CHRPHASE_OBJ ? pg_a :
                        (last_chr_b ? pg_b : pg_a));
    } else {
        /* 8x8: set A drives every fetch */
        ppu_setchrpages(pg_a);
    }
}

/* 4 KiB CHR bank for extended-attribute tiles (ExRAM mode 1). */
static const uint8 *map5_exchr(uint32 bank6)
{
    rominfo_t *cart = mmc_getinfo();
    if (cart->vrom && cart->vrom_banks > 0) {
        const unsigned count = (unsigned)cart->vrom_banks * 2u;   /* 4K banks */
        const unsigned bank = (((unsigned)chr_high << 6) | bank6) % count;
        return cart->vrom + bank * 4096u;
    }
    if (cart->vram && cart->vram_banks > 0) {
        const unsigned count = (unsigned)cart->vram_banks * 2u;
        const unsigned bank = (((unsigned)chr_high << 6) | bank6) % count;
        return cart->vram + bank * 4096u;
    }
    return zeroram;
}

static void exattr_apply(void)
{
    /* Extended attributes only exist while $5104 == 1. */
    ppu_setexattr(exram_mode == 1 ? exram : NULL, map5_exchr);
}

/* ------------------------------------------------------------------ */
/* Init / scanline / registers                                         */
/* ------------------------------------------------------------------ */
static void map5_init(void)
{
    prg_mode = 3; chr_mode = 0; exram_mode = 0;
    memset(prg, 0, sizeof prg); memset(chr, 0, sizeof chr);
    prg[4] = 0xff; chr_high = 0;
    nt_mode = 0; fill_tile = fill_attr = 0;
    memset(exram, 0, sizeof exram); memset(zeroram, 0, sizeof zeroram);
    refresh_fill();
    prg_protect1 = 0; prg_protect2 = 0;
    irq_target = irq_enable = irq_pending = in_frame = frame_active = 0;
    scanline_counter = 0;
    mul_a = mul_b = 0;
    split_ctrl = split_scroll = split_bank = 0;
    last_chr_b = 0;
    memset(win_ptr, 0, sizeof win_ptr); memset(win_is_ram, 0, sizeof win_is_ram);
    prg_apply();
    chr_rebuild();
    ppu_setchrphasefunc(map5_chrphase);
    exattr_apply();
    /* Default H/V mirroring from cartridge header until $5105 is set. */
    if (mmc_getinfo()->mirror == MIRROR_VERT) nt_mode = 0x44;
    else nt_mode = 0x50;
    nt_apply();
}

static void map5_hblank(int vblank)
{
    if (vblank || !ppu_enabled()) {
        in_frame = 0; frame_active = 0;
        return;
    }
    const int line = nes_getcontextptr()->scanline;
    if (!frame_active) {
        /* First rendered line of the frame: the detector resets here. */
        frame_active = 1; scanline_counter = 0; irq_pending = 0;
    }
    in_frame = 1;
#if MMC5_IRQ_AFTER_LINE_T
    scanline_counter = (uint8)line;
    if (irq_target && scanline_counter == irq_target) {
        irq_pending = 1;
        if (irq_enable) nes_irq();
    }
#else
    /* This callback runs after line `line` is drawn: the hardware counter
     * advances to line+1 now, and a match asserts the IRQ immediately. */
    if (line < 239) {
        scanline_counter = (uint8)(line + 1);
        if (irq_target && scanline_counter == irq_target) {
            irq_pending = 1;
            if (irq_enable) nes_irq();
        }
    }
#endif
}

/* Is $8000+ address `addr` currently backed by PRG-RAM, and writable? */
static int prg_ram_writable(uint32 addr)
{
    if (prg_protect1 != 2 || prg_protect2 != 1 || !mmc_getinfo()->sram)
        return 0;
    return win_is_ram[(addr - 0x8000) >> 13];
}

static void map5_write(uint32 addr, uint8 val)
{
    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        if (exram_mode < 3) ppu_async_drain();   /* ExRAM can be a nametable */
        if (exram_mode == 2) exram[addr - 0x5C00] = val;
        else if (exram_mode < 2) exram[addr - 0x5C00] = in_frame ? val : 0;
        return;     /* mode 3: read only */
    }
    if (addr >= 0x8000) {
        if (prg_ram_writable(addr))
            mmc_getinfo()->sram[addr & 0x1fff] = val;
        return;
    }
    switch (addr) {
    case 0x5100: prg_mode = val & 3; prg_apply(); break;
    case 0x5101: chr_mode = val & 3; chr_dirty = 1; break;
    case 0x5102: prg_protect1 = val & 3; break;
    case 0x5103: prg_protect2 = val & 3; break;
    case 0x5104: exram_mode = val & 3; nt_apply(); exattr_apply(); break;
    case 0x5105: nt_mode = val; nt_apply(); break;
    case 0x5106: fill_tile = val; refresh_fill(); break;
    case 0x5107: fill_attr = val & 3; refresh_fill(); break;
    case 0x5113: prg[0] = val; break;
    case 0x5114: case 0x5115: case 0x5116: case 0x5117:
        prg[1 + addr - 0x5114] = val; prg_apply(); break;
    case 0x5130: chr_high = val & 3; chr_dirty = 1; break;
    case 0x5200: split_ctrl = val; break;
    case 0x5201: split_scroll = val; break;
    case 0x5202: split_bank = val; break;
    case 0x5203: irq_target = val; break;
    case 0x5204:
        irq_enable = (val >> 7) & 1;
        if (irq_enable && irq_pending) nes_irq();
        break;
    case 0x5205: mul_a = val; break;
    case 0x5206: mul_b = val; break;
    default:
        if (addr >= 0x5120 && addr <= 0x512B) {
            chr[addr - 0x5120] = val;
            last_chr_b = (addr >= 0x5128);
            chr_dirty = 1;
        }
        break;
    }
}

static uint8 map5_read(uint32 addr)
{
    if (addr >= 0x5C00 && addr <= 0x5FFF)
        return (exram_mode >= 2) ? exram[addr - 0x5C00] : (uint8)(addr >> 8);
    if (addr == 0x5204) {
        const uint8 r = (irq_pending ? 0x80 : 0) | (in_frame ? 0x40 : 0);
        irq_pending = 0;
        return r;
    }
    if (addr == 0x5205) return (uint8)((unsigned)mul_a * mul_b);
    if (addr == 0x5206) return (uint8)(((unsigned)mul_a * mul_b) >> 8);
    return 0xff;
}

static const map_memread rd[] = {
    {0x5204, 0x5206, map5_read}, {0x5C00, 0x5FFF, map5_read},
    {(uint32)-1, (uint32)-1, NULL}
};
static const map_memwrite wr[] = {
    {0x5100, 0x5FFF, map5_write}, {0x8000, 0xFFFF, map5_write},
    {(uint32)-1, (uint32)-1, NULL}
};
const mapintf_t map5_intf = {5, "MMC5/ExROM (v0.1.9)", map5_init, NULL, map5_hblank,
    NULL, NULL, rd, wr, &mmc5_ext};
