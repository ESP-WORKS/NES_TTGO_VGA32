/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* UNROM512: PRG/CHR RAM banking; flash programming is NOT emulated. */
static void map30_init(void) { mmc_bankrom(16,0x8000,0); mmc_bankrom(16,0xC000,MMC_LASTBANK); }
static void map30_write(uint32 address,uint8 value) {
    rominfo_t *cart=mmc_getinfo();
    UNUSED(address);
    mmc_bankrom(16,0x8000,value&31);
    if (cart && cart->vram) ppu_setpage(8,0,cart->vram+(((value>>5)&3)*8192));
    /* iNES mapper 30: bit 3 + !bit 0 = selectable 1-screen CIRAM. */
    if (cart && (cart->flags & ROM_FLAG_FOURSCREEN) && cart->mirror==MIRROR_HORIZ) {
        int nt=(value>>7)&1;
        ppu_mirror(nt,nt,nt,nt);
    }
}
static const map_memwrite wr[]={{0x8000,0xFFFF,map30_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map30_intf={30,"UNROM512 (no flash)",map30_init,NULL,NULL,NULL,NULL,NULL,wr,NULL};
