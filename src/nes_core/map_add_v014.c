
/* NES mapper expansion v0.1.4: Sunsoft 4, Taito ASIC families, Namco
 * 175/340, HES, SUBOR and CNROM protection. Implementations are original.
 * Some boards need exact EEPROM, DIP-switch timing, extended audio or per-
 * fetch PPU behavior; see AUDITORIA_ESTABILIDADE_MAPPERS_v0.1.4.md.
 */
#include <string.h>
#include "noftypes.h"
#include "nes.h"
#include "nes_mmc.h"
#include "nes6502.h"
#include "nes_ppu.h"
#define ENDW {(uint32)-1,(uint32)-1,NULL}
#define ENDR {(uint32)-1,(uint32)-1,NULL}
#define DECLW(n,lo,hi,fn) static const map_memwrite w##n[]={ {lo,hi,fn},ENDW};
#define DECLR(n,lo,hi,fn) static const map_memread r##n[]={ {lo,hi,fn},ENDR};
#define INTF(n,label,init,hblank,read,write) \
  const mapintf_t map##n##_intf={n,label,init,NULL,hblank,NULL,NULL,read,write,NULL};
static void mir(uint8 mode) {
  switch(mode&3) {
  case 0:ppu_mirror(0,1,0,1);break;
  case 1:ppu_mirror(0,0,1,1);break;
  case 2:ppu_mirror(0,0,0,0);break;
  case 3:ppu_mirror(1,1,1,1);break;
  }
}

/* Sunsoft-4 / After Burner mapper 68. $C/D000 select CHR nametables
 * (when $E000 bit 4 is asserted) instead of internal CIRAM. */
static uint8 s68_nt[2],s68_ctrl;
static void s68_nt_apply(void) {
    rominfo_t *r=mmc_getinfo();
    if(!(s68_ctrl&16) || !r || !r->vrom || !r->vrom_banks) {
        mir(s68_ctrl);return;
    }
    int slots[4];
    switch(s68_ctrl&3) {
    case 0:slots[0]=0;slots[1]=1;slots[2]=0;slots[3]=1;break;
    case 1:slots[0]=0;slots[1]=0;slots[2]=1;slots[3]=1;break;
    case 2:slots[0]=slots[1]=slots[2]=slots[3]=0;break;
    default:slots[0]=slots[1]=slots[2]=slots[3]=1;break;
    }
    for(int i=0;i<4;i++) {
        unsigned num=(s68_nt[slots[i]] | 0x80u) % (unsigned)(r->vrom_banks*8);
        ppu_setpage(1,8+i,r->vrom+num*1024-(0x2000+i*1024));
    }
    ppu_mirrorhipages();
}
static void s68_init(void){s68_ctrl=0;s68_nt[0]=s68_nt[1]=0;mmc_bankrom(16,0xC000,MMC_LASTBANK);}
static void s68_write(uint32 a,uint8 v){
    switch(a&0xF000){
    case 0x8000:case 0x9000:case 0xA000:case 0xB000:
        mmc_bankchr(2,((a>>12)-8)*2048,v);break;
    case 0xC000:s68_nt[0]=v;s68_nt_apply();break;
    case 0xD000:s68_nt[1]=v;s68_nt_apply();break;
    case 0xE000:s68_ctrl=v;s68_nt_apply();break;
    case 0xF000:mmc_bankrom(16,0x8000,v&15);break;
    }
}
DECLW(68,0x8000,0xFFFF,s68_write)
INTF(68,"Sunsoft 4",s68_init,NULL,NULL,w68)

/* Taito X1-005 (80/207) and X1-017 (82/552).
 * Registers $7EF0-F mirrored at $7E70-F. 128-byte internal SRAM modeled
 * within the ordinary 8KiB battery-backed array (sram[0..127]). */
static uint8 taito_chr[6],taito_ctrl,taito_ram_enabled;
static int taito_type;
static void taito_apply(void){
    int inv=(taito_type==82||taito_type==552)?((taito_ctrl>>1)&1):0;
    int target=inv?0x1000:0;
    mmc_bankchr(2,target,taito_chr[0]>>1);
    mmc_bankchr(2,target+0x800,taito_chr[1]>>1);
    target=inv?0:0x1000;
    for(int i=0;i<4;i++)mmc_bankchr(1,target+i*1024,taito_chr[i+2]);
    if(taito_type==207||taito_type==552) {
        // PCB variants with per-table mirroring from CHR bank high bit.
        ppu_mirror((taito_chr[0]>>7)&1,(taito_chr[1]>>7)&1,
                   (taito_chr[2]>>7)&1,(taito_chr[3]>>7)&1);
    }else mir((taito_ctrl&1)?1:0);
}
static void taito_init(void){
    taito_type=mmc_getinfo()->mapper_number;
    memset(taito_chr,0,sizeof taito_chr);taito_ctrl=0;
    taito_ram_enabled=0;mmc_bankrom(8,0xE000,MMC_LASTBANK);taito_apply();
}
static uint8 taito_read(uint32 a){
    rominfo_t *r=mmc_getinfo();
    if(a>=0x7F00 && r && r->sram && taito_ram_enabled)return r->sram[a&127];
    return 0xff;
}
static void taito_write(uint32 a,uint8 v){
    if(a>=0x7F00){
        rominfo_t *r=mmc_getinfo();
        if(r && r->sram && taito_ram_enabled)r->sram[a&127]=v;
        return;
    }
    a=(a&~0x80u); // $7E7F and $7EFF aliases
    if(a>=0x7E70 && a<=0x7E75) {taito_chr[a-0x7E70]=v;taito_apply();return;}
    if(a==0x7E76){taito_ctrl=v;taito_apply();return;}
    if(a==0x7E78||a==0x7E79){taito_ram_enabled=(v==0xA3);return;}
    if(a>=0x7E7A && a<=0x7E7F){
        int reg=(a-0x7E7A)>>1;
        mmc_bankrom(8,0x8000+reg*0x2000,v&0x3f);
    }
}
static const map_memwrite wtaito[]={
    {0x7E70,0x7EFF,taito_write},{0x7F00,0x7FFF,taito_write},ENDW};
static const map_memread rtaito[]={ {0x7F00,0x7FFF,taito_read},ENDR};
INTF(80,"Taito X1-005",taito_init,NULL,rtaito,wtaito)
INTF(82,"Taito X1-017 (partial RAM)",taito_init,NULL,rtaito,wtaito)
INTF(207,"Taito X1-005 NT",taito_init,NULL,rtaito,wtaito)
INTF(552,"Taito X1-017 NT NES2",taito_init,NULL,rtaito,wtaito)

/* NES-EVENT 105: serial MMC1-style unlock, bank switching, timer IRQ.
 * IRQ timing approximate 60Hz, not actual M2-clock/DIP-switch counter. */
static uint8 ev_shift,ev_bits,ev_reg[4],ev_unlocked,ev_low_seen;
static uint32 ev_frames;
static void ev_apply(void){
    if(!ev_unlocked){mmc_bankrom(32,0x8000,0);return;}
    mir((ev_reg[0]&3)^2);
    if(!(ev_reg[1]&8))mmc_bankrom(32,0x8000,(ev_reg[1]>>1)&3);
    else {
       int n=(ev_reg[3]&15)|8;
       if((ev_reg[0]&12)<8)mmc_bankrom(32,0x8000,n>>1);
       else if((ev_reg[0]&12)==8){mmc_bankrom(16,0x8000,8);mmc_bankrom(16,0xC000,n);}
       else{mmc_bankrom(16,0x8000,n);mmc_bankrom(16,0xC000,15);}
    }
}
static void ev_init(void){memset(ev_reg,0,sizeof ev_reg);ev_reg[0]=12;
    ev_shift=ev_bits=ev_unlocked=ev_low_seen=0;ev_frames=0;ev_apply();}
static void ev_write(uint32 a,uint8 v){
    if(v&0x80){ev_shift=ev_bits=0;ev_reg[0]|=12;ev_apply();return;}
    ev_shift|=(v&1)<<ev_bits;
    if(++ev_bits==5){unsigned i=(a>>13)&3;ev_reg[i]=ev_shift;
        ev_shift=ev_bits=0;
        if(i==1) {if(!(ev_reg[1]&16))ev_low_seen=1;
          else if(ev_low_seen)ev_unlocked=1;
          if(ev_reg[1]&16)ev_frames=0;}
        ev_apply();}
}
static void ev_hblank(int vb){if(!vb && nes_getcontextptr()->scanline==240 &&
    ev_unlocked && !(ev_reg[1]&16) && ++ev_frames>18000u)nes_irq();}
DECLW(105,0x8000,0xFFFF,ev_write)
INTF(105,"NES-EVENT (partial timer)",ev_init,ev_hblank,NULL,w105)

/* HES NTD-8 (113) multicart latch. */
static void n113_write(uint32 a,uint8 v){(void)a;
    mmc_bankrom(32,0x8000,(v>>3)&7);
    mmc_bankchr(8,0,(v&7)|((v>>3)&8));
    if(v&0x80)ppu_mirror(0,0,1,1);else ppu_mirror(0,1,0,1);
}
static const map_memwrite w113[]={ {0x4100,0x5FFF,n113_write},
    {0x8000,0xFFFF,n113_write},ENDW};
INTF(113,"HES NTD-8",NULL,NULL,NULL,w113)

/* Bandai LZ93D50 / 159 with serial 128-byte X24C01 EEPROM.
 * State diagram follows the user-supplied Mesen Eeprom24C01 and BandaiFcg.
 * EEPROM bits are stored in cart SRAM first 128 bytes, which our existing
 * 8KiB .sav pipeline persists; avoid writes outside this region. */
static uint16 band_irq,band_latch;
static uint8 band_enabled;
enum {EEP_IDLE,EEP_ADDRESS,EEP_READ,EEP_WRITE,EEP_ACK,EEP_WAIT_ACK};
static uint8 ee_mode,ee_next,ee_address,ee_data,ee_count,ee_output;
static uint8 ee_prev_scl,ee_prev_sda;
static void ee_bit_write(uint8 *reg,uint8 bit){
    if(ee_count<8){*reg=(uint8)((*reg&~(1u<<ee_count))|((bit&1u)<<ee_count));ee_count++;}
}
static void ee_clock(uint8 scl,uint8 sda){
    // START/STOP have precedence over clock transitions.
    if(ee_prev_scl && scl && ee_prev_sda && !sda){
        ee_mode=EEP_ADDRESS;ee_address=ee_count=0;ee_output=1;
    }else if(ee_prev_scl && scl && !ee_prev_sda && sda){
        ee_mode=EEP_IDLE;ee_output=1;
    }else if(scl && !ee_prev_scl){
        switch(ee_mode){
        case EEP_ADDRESS:
            if(ee_count<7)ee_bit_write(&ee_address,sda);
            else if(ee_count==7){
                ee_count=8;
                ee_next=sda?EEP_READ:EEP_WRITE;
                if(ee_next==EEP_READ && mmc_getinfo()->sram)
                    ee_data=mmc_getinfo()->sram[ee_address&127];
                else ee_data=0;
            }
            break;
        case EEP_ACK:ee_output=0;break;
        case EEP_READ:
            if(ee_count<8){ee_output=(ee_data>>ee_count)&1;ee_count++;}
            break;
        case EEP_WRITE:ee_bit_write(&ee_data,sda);break;
        case EEP_WAIT_ACK:
            if(!sda && mmc_getinfo()->sram){
                ee_mode=EEP_READ;ee_data=mmc_getinfo()->sram[ee_address&127];ee_count=0;
            }else ee_mode=EEP_IDLE;
            break;
        default:break;
        }
    }else if(!scl && ee_prev_scl){
        switch(ee_mode){
        case EEP_ADDRESS:
            if(ee_count==8){ee_mode=EEP_ACK;ee_output=1;}
            break;
        case EEP_ACK:
            ee_mode=ee_next;ee_count=0;ee_output=1;
            break;
        case EEP_READ:
            if(ee_count==8){ee_mode=EEP_WAIT_ACK;ee_address=(ee_address+1)&127;}
            break;
        case EEP_WRITE:
            if(ee_count==8){
                if(mmc_getinfo()->sram)mmc_getinfo()->sram[ee_address&127]=ee_data;
                ee_address=(ee_address+1)&127;
                ee_next=EEP_IDLE;ee_mode=EEP_ACK;ee_output=1;
            }
            break;
        }
    }
    ee_prev_scl=scl;ee_prev_sda=sda;
}
static void n159_init(void){
    band_irq=band_latch=band_enabled=0;
    ee_mode=EEP_IDLE;ee_address=ee_data=ee_count=ee_output=0;
    ee_next=EEP_IDLE;ee_prev_scl=ee_prev_sda=0;
    mmc_bankrom(16,0xC000,MMC_LASTBANK);
}
static void n159_write(uint32 a,uint8 v){int r=a&15;
    if(r<8)mmc_bankchr(1,r*1024,v);
    else if(r==8)mmc_bankrom(16,0x8000,v);
    else if(r==9)mir(v&3);
    else if(r==10){band_enabled=v&1;band_irq=band_latch;}
    else if(r==11)band_latch=(band_latch&0xff00)|v;
    else if(r==12)band_latch=(band_latch&0x00ff)|((uint16)v<<8);
    else if(r==13)ee_clock((v>>5)&1,(v>>6)&1);
}
static uint8 n159_read(uint32 a){(void)a;return (ee_output&1)<<4;}
static void n159_hblank(int vb){(void)vb;if(band_enabled){
    if(band_irq<=114){band_irq=0;nes_irq();}else band_irq-=114;}}
DECLW(159,0x8000,0xFFFF,n159_write)
DECLR(159,0x6000,0x7FFF,n159_read)
INTF(159,"Bandai X24C01 EEPROM",n159_init,n159_hblank,r159,w159)

/* SUBOR (166/167): complementary bank bits from $8000/E000 and $A000/C000 */
static uint8 subor[4];
static void subor_apply(void){
    int combined=((subor[0]^subor[1])&0x20) |
                  ((subor[2]^subor[3])&0x1f);
    int mode=(subor[1]>>2)&3;
    int which=mmc_getinfo()->mapper_number;
    if(mode==0){mmc_bankrom(16,0x8000,combined);mmc_bankrom(16,0xC000,which==166?7:32);}
    else if(mode==1){mmc_bankrom(16,0x8000,31);mmc_bankrom(16,0xC000,combined);}
    else {mmc_bankrom(16,0x8000,combined|1);mmc_bankrom(16,0xC000,combined&~1);}
}
static void subor_init(void){memset(subor,0,sizeof subor);subor_apply();}
static void subor_write(uint32 a,uint8 v){subor[(a>>13)&3]=v;subor_apply();}
DECLW(166,0x8000,0xFFFF,subor_write)
INTF(166,"SUBOR early banking",subor_init,NULL,NULL,w166)
INTF(167,"SUBOR",subor_init,NULL,NULL,w166)

/* CNROM 185 - CHR ROM chip select protection; heuristic for iNES 1.0. */
static uint8 n185_open[8192];
static void n185_init(void){memset(n185_open,0xff,sizeof n185_open);
    mmc_bankvrom(8,0,0);}
static void n185_write(uint32 a,uint8 v){(void)a;
    if((v&3)==0)mmc_bankvrom(8,0,0);
    else ppu_setpage(8,0,n185_open);
}
DECLW(185,0x8000,0xFFFF,n185_write)
INTF(185,"CNROM security diodes (heuristic)",n185_init,NULL,NULL,w185)

/* Namco 175/340 (210) cost reduced Namco 163. No IRQ/extension audio. */
static void n210_init(void){mmc_bankrom(8,0xE000,MMC_LASTBANK);}
static void n210_write(uint32 a,uint8 v){
    if(a<0xC000)mmc_bankchr(1,(((a-0x8000)>>11)&7)*1024,v);
    else if(a>=0xE000){
       switch((a>>11)&3){
       case 0:mmc_bankrom(8,0x8000,v&0x3f);
          switch(v>>6){case 0:ppu_mirror(0,0,0,0);break;
          case 1:ppu_mirror(0,1,0,1);break;
          case 2:ppu_mirror(1,1,1,1);break;
          case 3:ppu_mirror(0,0,1,1);break;}break;
       case 1:mmc_bankrom(8,0xA000,v&0x3f);break;
       case 2:mmc_bankrom(8,0xC000,v&0x3f);break;
       }
    }
}
DECLW(210,0x8000,0xFFFF,n210_write)
INTF(210,"Namco 175/340",n210_init,NULL,NULL,w210)
