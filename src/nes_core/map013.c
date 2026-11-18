/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* CPROM: 4K fixed CHR RAM and switchable 4K CHR RAM, total 16K. */
static void map13_setchr(unsigned bank) {
    rominfo_t *cart=mmc_getinfo();
    if (!cart || !cart->vram) return;
    ppu_setpage(4,0,cart->vram);
    ppu_setpage(4,4,cart->vram+((bank&3)*4096)-0x1000);
}
static void map13_init(void) { mmc_bankrom(32,0x8000,0); map13_setchr(0); }
static void map13_write(uint32 address,uint8 value) { UNUSED(address); map13_setchr(value); }
static const map_memwrite wr[]={{0x8000,0xFFFF,map13_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map13_intf={13,"CPROM",map13_init,NULL,NULL,NULL,NULL,NULL,wr,NULL};
