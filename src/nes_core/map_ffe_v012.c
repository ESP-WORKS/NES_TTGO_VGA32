/* NES TTGO VGA32 v0.1.2: original lightweight mapper adaptations.
 * Hardware behavior cross-checked with the user-supplied Mesen GPL-3 source;
 * this implementation contains no copied Mesen code. */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"
#define END_WR {(uint32)-1,(uint32)-1,NULL}

static uint16 irq_counter;
static bool irq_on, alt_mode;
static void ffe_irq(int vblank) {
  UNUSED(vblank);
  if(!irq_on) return;
  /* Nofrendo supplies scanline callbacks only: approximate 113 CPU cycles. */
  const uint16 before=irq_counter;
  irq_counter=(uint16)(irq_counter+113);
  if(irq_counter<before) { irq_on=false; nes_irq(); }
}
static void ffe_control(uint32 addr,uint8 v) {
  switch(addr) {
  case 0x42fe: {alt_mode=(v&0x80)==0;int nt=(v>>4)&1;ppu_mirror(nt,nt,nt,nt);break;}
  case 0x42ff: if(v&0x10) ppu_mirror(0,0,1,1);else ppu_mirror(0,1,0,1);break;
  case 0x4501: irq_on=false;break;
  case 0x4502: irq_counter=(irq_counter&0xff00)|v;break;
  case 0x4503: irq_counter=(irq_counter&0xff)|(v<<8);irq_on=true;break;
  }
}
static void ffe_reset(void) {irq_counter=0;irq_on=false;alt_mode=true;}
/* Mapper 6: 16K PRG and 8K switchable CHR-RAM (total 32K). */
static void ffe6_init(void) {ffe_reset();mmc_bankrom(16,0x8000,0);mmc_bankrom(16,0xC000,MMC_LASTBANK);mmc_bankchrram(8,0,0);}
static void ffe6_write(uint32 a,uint8 v) {
 if(a<0x8000) {ffe_control(a,v);return;}
 if(alt_mode || mmc_getinfo()->vrom_banks==0) {
   mmc_bankrom(16,0x8000,(v>>2)&0x3f);
   mmc_bankchrram(8,0,v&3);
 } else mmc_bankchr(8,0,v&3);
}
static const map_memwrite ffe6_wr[]={ {0x42fe,0x42ff,ffe6_write},
  {0x4501,0x4503,ffe6_write},{0x8000,0xffff,ffe6_write},END_WR};
const mapintf_t map6_intf={6,"FFE F4xxx",ffe6_init,NULL,ffe_irq,NULL,NULL,NULL,ffe6_wr,NULL};

/* Mapper 17: individual 8K PRG and 1K CHR banks via expansion registers. */
static void ffe17_init(void) {
 int i;ffe_reset();for(i=0;i<4;i++)mmc_bankrom(8,0x8000+i*0x2000,mmc_getinfo()->rom_banks*2-4+i);
 mmc_bankchr(8,0,0);
}
static void ffe17_write(uint32 a,uint8 v) {
 if(a==0x42fe||a==0x42ff||(a>=0x4501&&a<=0x4503)) {ffe_control(a,v);return;}
 if(a>=0x4504&&a<=0x4507) mmc_bankrom(8,0x8000+(a-0x4504)*0x2000,v);
 else if(a>=0x4510&&a<=0x4517) mmc_bankchr(1,(a-0x4510)*0x400,v);
}
static const map_memwrite ffe17_wr[]={ {0x42fe,0x42ff,ffe17_write},
  {0x4501,0x4507,ffe17_write},{0x4510,0x4517,ffe17_write},END_WR};
const mapintf_t map17_intf={17,"FFE F8xxx",ffe17_init,NULL,ffe_irq,NULL,NULL,NULL,ffe17_wr,NULL};
