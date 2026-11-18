/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* Jaleco JF-13: sampled speech (uPD7756) requires separate external dump. */
static void map86_init(void) {mmc_bankrom(32,0x8000,0);mmc_bankvrom(8,0,0);}
static void map86_write(uint32 addr,uint8 v) {
    if ((addr&0xF000)!=0x6000 && (addr&0xF000)!=0xE000) return;
    mmc_bankrom(32,0x8000,(v>>4)&3);
    mmc_bankvrom(8,0,(v&3)|((v>>4)&4));
}
static const map_memwrite wr[]={{0x6000,0x7FFF,map86_write},{0xE000,0xFFFF,map86_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map86_intf={86,"Jaleco JF13 (no speech)",map86_init,NULL,NULL,NULL,NULL,NULL,wr,NULL};
