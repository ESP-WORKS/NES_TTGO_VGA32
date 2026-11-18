/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* Camerica BF9093/BF9097: bank switching and Fire Hawk mirroring. */
static void map71_init(void) { mmc_bankrom(16,0x8000,0);mmc_bankrom(16,0xC000,MMC_LASTBANK); }
static void map71_write(uint32 addr,uint8 v) {
    if (addr>=0xC000) mmc_bankrom(16,0x8000,v&15);
    else if (addr>=0x9000 && addr<0xA000) {
        int nt=(v>>4)&1; ppu_mirror(nt,nt,nt,nt);
    }
}
static const map_memwrite wr[]={{0x8000,0xFFFF,map71_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map71_intf={71,"Camerica",map71_init,NULL,NULL,NULL,NULL,NULL,wr,NULL};
