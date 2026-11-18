/* NES TTGO VGA32 v0.1.2: original lightweight mapper adaptations.
 * Hardware behavior cross-checked with the user-supplied Mesen GPL-3 source;
 * this implementation contains no copied Mesen code. */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"
#define END_WR {(uint32)-1,(uint32)-1,NULL}

/* Mapper 76: Namco 109.  PRG banks 6/7, 4 x 2KiB CHR banks (registers 2..5). */
static uint8 n76_select;
static void n76_write(uint32 a,uint8 v) {
 if((a&0xE001)==0x8000) n76_select=v&7;
 else if((a&0xE001)==0x8001) {
   if(n76_select>=2 && n76_select<=5) mmc_bankchr(2, (n76_select-2)*0x800,v);
   else if(n76_select==6) mmc_bankrom(8,0x8000,v&0x3f);
   else if(n76_select==7) mmc_bankrom(8,0xA000,v&0x3f);
 }
}
static void n76_init(void) {n76_select=0;mmc_bankrom(8,0x8000,0);mmc_bankrom(8,0xA000,1);mmc_bankrom(8,0xC000,mmc_getinfo()->rom_banks*2-2);mmc_bankrom(8,0xE000,MMC_LASTBANK);}
static const map_memwrite n76_wr[]={{0x8000,0xffff,n76_write},END_WR};
const mapintf_t map76_intf={76,"Namco 109",n76_init,NULL,NULL,NULL,NULL,NULL,n76_wr,NULL};

/* Mapper 88: Namco 108 with fixed CHR high bits on registers 2..5. */
static uint8 n88_select;
static void n88_write(uint32 a,uint8 v) {
 if((a&0xE001)==0x8000) n88_select=v&7;
 else if((a&0xE001)==0x8001) {
   if(n88_select<2) mmc_bankchr(2,n88_select*0x800,(v&0x3e)>>1);
   else if(n88_select<6) mmc_bankchr(1,0x1000+(n88_select-2)*0x400,(v&0x3f)|0x40);
   else if(n88_select==6) mmc_bankrom(8,0x8000,v&0x3f);
   else mmc_bankrom(8,0xA000,v&0x3f);
 }
}
static void n88_init(void) {n88_select=0;mmc_bankrom(8,0x8000,0);mmc_bankrom(8,0xA000,1);mmc_bankrom(8,0xC000,mmc_getinfo()->rom_banks*2-2);mmc_bankrom(8,0xE000,MMC_LASTBANK);}
static const map_memwrite n88_wr[]={{0x8000,0xffff,n88_write},END_WR};
const mapintf_t map88_intf={88,"Namco 108/88",n88_init,NULL,NULL,NULL,NULL,NULL,n88_wr,NULL};

/* Mapper 89: Sunsoft 2 variant with 16K PRG and 8K CHR. */
static void n89_write(uint32 a,uint8 v) {
 UNUSED(a);
 mmc_bankrom(16,0x8000,(v>>4)&7);
 mmc_bankchr(8,0,(v&7)|((v>>4)&8));
 int nt=(v>>3)&1;ppu_mirror(nt,nt,nt,nt);
}
static void n89_init(void) {mmc_bankrom(16,0x8000,0);mmc_bankrom(16,0xC000,MMC_LASTBANK);mmc_bankchr(8,0,0);}
static const map_memwrite n89_wr[]={{0x8000,0xffff,n89_write},END_WR};
const mapintf_t map89_intf={89,"Sunsoft 2",n89_init,NULL,NULL,NULL,NULL,NULL,n89_wr,NULL};

/* Mapper 152: Bandai 74161/7432 - unlike mapper 70, bit7 always controls one-screen mirroring. */
static void n152_write(uint32 a,uint8 v) {
 UNUSED(a);mmc_bankrom(16,0x8000,(v>>4)&7);mmc_bankchr(8,0,v&15);
 int nt=(v>>7)&1;ppu_mirror(nt,nt,nt,nt);
}
static void n152_init(void) {mmc_bankrom(16,0xC000,MMC_LASTBANK);n152_write(0,0);}
static const map_memwrite n152_wr[]={{0x8000,0xffff,n152_write},END_WR};
const mapintf_t map152_intf={152,"Bandai 74161",n152_init,NULL,NULL,NULL,NULL,NULL,n152_wr,NULL};

/* Mapper 184: Sunsoft 1: writes at $6000-$7FFF select two 4K CHR banks. */
static void n184_write(uint32 a,uint8 v) {
 UNUSED(a);mmc_bankchr(4,0,v&7);
 mmc_bankchr(4,0x1000,0x80|((v>>4)&7));
}
static void n184_init(void) {mmc_bankrom(32,0x8000,0);n184_write(0,0);}
static const map_memwrite n184_wr[]={{0x6000,0x7fff,n184_write},END_WR};
const mapintf_t map184_intf={184,"Sunsoft 1",n184_init,NULL,NULL,NULL,NULL,NULL,n184_wr,NULL};

/* Mapper 228: Action Enterprises multicart, banking register encoded in CPU address. */
static void n228_write(uint32 a,uint8 v) {
 unsigned chip=(a>>11)&3;
 if(chip==3) chip=2;
 unsigned prg=((a>>6)&31)|(chip<<5);
 if(a&0x20) {mmc_bankrom(16,0x8000,prg);mmc_bankrom(16,0xC000,prg);}
 else {mmc_bankrom(16,0x8000,prg&~1U);mmc_bankrom(16,0xC000,(prg&~1U)+1);}
 mmc_bankchr(8,0,((a&15)<<2)|(v&3));
 if(a&0x2000) ppu_mirror(0,0,1,1);
 else ppu_mirror(0,1,0,1);
}
static void n228_init(void) {n228_write(0x8000,0);}
static const map_memwrite n228_wr[]={{0x8000,0xffff,n228_write},END_WR};
const mapintf_t map228_intf={228,"Action Enterprises",n228_init,NULL,NULL,NULL,NULL,NULL,n228_wr,NULL};

/* Mapper 241: 32K PRG banking, CHR ROM fixed. */
static void n241_write(uint32 a,uint8 v) {UNUSED(a);mmc_bankrom(32,0x8000,v);}
static void n241_init(void) {mmc_bankrom(32,0x8000,0);mmc_bankchr(8,0,0);}
static const map_memwrite n241_wr[]={{0x8000,0xffff,n241_write},END_WR};
const mapintf_t map241_intf={241,"Mapper 241",n241_init,NULL,NULL,NULL,NULL,NULL,n241_wr,NULL};

/* Mapper 95: Namco 108 variant with A15-driven per-half nametable select. */
static uint8 n95_select,n95_banks[8];
static void n95_write(uint32 a,uint8 v) {
 if((a&0xE001)==0x8000) n95_select=v&7;
 else if((a&0xE001)==0x8001) {
   n95_banks[n95_select]=v;
   switch(n95_select) {
    case 0:case 1:mmc_bankchr(2,n95_select*0x800,(v&0x3e)>>1);break;
    case 2:case 3:case 4:case 5:mmc_bankchr(1,0x1000+(n95_select-2)*0x400,v&0x3f);break;
    case 6:mmc_bankrom(8,0x8000,v&0x3f);break;
    case 7:mmc_bankrom(8,0xA000,v&0x3f);break;
   }
   int lo=(n95_banks[0]>>5)&1,hi=(n95_banks[1]>>5)&1;
   ppu_mirror(lo,lo,hi,hi);
 }
}
static void n95_init(void) {
 n95_select=0;int i;for(i=0;i<8;i++)n95_banks[i]=0;
 mmc_bankrom(8,0x8000,0);mmc_bankrom(8,0xA000,1);
 mmc_bankrom(8,0xC000,mmc_getinfo()->rom_banks*2-2);mmc_bankrom(8,0xE000,MMC_LASTBANK);
}
static const map_memwrite n95_wr[]={{0x8000,0xffff,n95_write},END_WR};
const mapintf_t map95_intf={95,"Namco 108-95",n95_init,NULL,NULL,NULL,NULL,NULL,n95_wr,NULL};
