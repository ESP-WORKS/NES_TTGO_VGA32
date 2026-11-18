/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* Camerica Quattro: 64K outer, 16K inner PRG bank. */
static uint8 outer,inner;
static void map232_bank(void) {
    mmc_bankrom(16,0x8000,(outer<<2)|inner);
    mmc_bankrom(16,0xC000,(outer<<2)|3);
}
static void map232_init(void) {outer=3;inner=0;map232_bank();}
static void map232_write(uint32 addr,uint8 v) {
    if (addr<0xC000) outer=(v>>3)&3;
    else inner=v&3;
    map232_bank();
}
static const map_memwrite wr[]={{0x8000,0xFFFF,map232_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map232_intf={232,"Quattro",map232_init,NULL,NULL,NULL,NULL,NULL,wr,NULL};
