/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* MMC4: latch-based CHR banking similar to MMC2, with 16K PRG @ 8000. */
static uint8 latch[2], regs[4];
static void map10_latch(uint32 address, uint8 value) {
    if (value != 0xFD && value != 0xFE) return;
    unsigned half = address ? 1 : 0;
    latch[half] = value;
    mmc_bankvrom(4, half ? 0x1000 : 0x0000,
                 regs[half * 2 + (value == 0xFE)]);
}
static void map10_init(void) {
    unsigned i; for (i=0;i<4;++i) regs[i]=0;
    latch[0]=latch[1]=0xFE;
    mmc_bankrom(16,0x8000,0);
    mmc_bankrom(16,0xC000,MMC_LASTBANK);
    ppu_setlatchfunc(map10_latch);
}
static void map10_write(uint32 address, uint8 value) {
    switch (address & 0xF000) {
    case 0xA000: mmc_bankrom(16,0x8000,value); break;
    case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
        int n=(int)((address>>12)-0xB);
        regs[n]=value;
        if (latch[n>>1] == ((n&1) ? 0xFE : 0xFD))
            mmc_bankvrom(4,(n>>1)*0x1000,value);
        break;
    }
    case 0xF000:
        if (!(mmc_getinfo()->flags & ROM_FLAG_FOURSCREEN)) {
            if (value & 1) ppu_mirror(0,0,1,1);
            else ppu_mirror(0,1,0,1);
        }
        break;
    }
}
static const map_memwrite wr[]={{0x8000,0xFFFF,map10_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map10_intf={10,"MMC4",map10_init,NULL,NULL,NULL,NULL,NULL,wr,NULL};
