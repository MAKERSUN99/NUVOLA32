/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
** map163.c
**
** Mapper 163 - Nanjing FC-001 (南晶)
**
** Hardware reference: NewRisingSun's notes on the FC-001 PCB
** (https://forums.nesdev.org/viewtopic.php?t=18000)
** and NESdev wiki INES_Mapper_163.
**
** FC-001 pinout key signals:
**   CPU A8  -> pin 17  (selects $5100 vs $5200 in address decode)
**   CPU A9  -> pin 18
**   PPU A9  -> pin 07  (CHR auto-switch source)
**   PPU A12 -> pin 09
**   PPU A13 -> pin 06  (CHR auto-switch latch trigger)
**   CHR A12 <- pin 10  (output: controlled by auto-switch)
**
** Register map (mask $FF00 unless noted):
**
**   $5000 write  [C... PPPP]
**     PPPP = PRG A18..A15
**     C    = CHR auto-switch enable (1 = CHR A12 follows latched PPU A9)
**     bits 0-1 subject to $5300 B-bit swap
**
**   $5100 write  [..P. PpPP]   <- "output latch" on mapper 163
**     p    = inner 32KB bank bit (ORed with bank when $5300=4)
**     P... = PRG A20..A19 / other upper bits
**     bits 0-1 subject to $5300 B-bit swap
**
**   $5101 write  edge-triggered security
**     If last written value to $5101 != 0x00: flip trigger bit
**     Then latch the new value
**
**   $5200 write  (unused on mapper 163, CPU A8/A9 swapped vs 164)
**
**   $5300 write  [.... .A.B]  NOT subject to bit-swap
**     B=1: swap D0,D1 on writes to $5000/$5100
**     A=1: PRG A15/A16 from $5000 bits 0-1
**     A=0: PRG A15/A16 = 11b  → boots in last bank
**
**   $5100/$5500 read  [.... .T..]
**     D2 = inverted security trigger bit T
**
** Boot state: all regs=0, boots in bank 3 (A=0 → A15/A16=11b)
**
** Copy protection (FF7 specific):
**   Game writes sequence to $5101 to flip T, then reads $5500.
**   If D2 = ~T the game continues; otherwise it loops forever.
**   The trigger flips only when the *previous* value was nonzero.
*/

#include "../noftypes.h"
#include "../nes/nes_mmc.h"
#include "../nes/nes.h"
#include "../libsnss/libsnss.h"

static uint8 reg5000;    /* [C... PPPP] PRG low + CHR auto flag */
static uint8 reg5100;    /* [..P. PpPP] PRG high / output latch */
static uint8 reg5300;    /* [.... .A.B] mode */
static uint8 sec_latch;  /* last value written to $5101 */
static uint8 sec_trig;   /* security trigger bit (flipped on nonzero->any) */
static uint8 chr_auto;   /* CHR auto-switch enabled */

/* Apply D0/D1 swap if Mode B=1 */
static uint8 map163_swap(uint8 v)
{
   if (reg5300 & 0x01)
      v = (v & 0xFC) | ((v & 0x01) << 1) | ((v & 0x02) >> 1);
   return v;
}

static void map163_setprg(void)
{
   uint8 low  = map163_swap(reg5000) & 0x0F;  /* PRG A18..A15 */
   uint8 hi   = map163_swap(reg5100);
   /* p bit: bit 0 of reg5100 after swap, ORed with bank when $5300=4 */
   uint8 p    = hi & 0x01;
   /* upper bits PRG A20..A19: bits 4-5 of reg5100 (hardware pins 3,33) */
   uint8 top  = (hi >> 4) & 0x03;
   uint8 bank;

   if (reg5300 & 0x02) /* A bit: use $5000 bits 0-1 as A15/A16 */
   {
      bank = (top << 4) | low;
      /* OR the p bit when mode $5300 = $04 */
      if ((reg5300 & 0x07) == 0x04)
         bank |= p;
   }
   else /* A=0: A15/A16 = 11b */
   {
      bank = (top << 4) | (low & 0x0C) | 0x03;
   }

   mmc_bankrom(16, 0x8000, bank * 2);
   mmc_bankrom(16, 0xC000, bank * 2 + 1);
}

static void map163_setchr(void)
{
   if (!chr_auto)
      mmc_bankvrom(8, 0x0000, 0);
   else
   {
      /* Auto-switch: 4KB bank 0 for top nametable, bank 1 for bottom */
      mmc_bankvrom(4, 0x0000, 0);
      mmc_bankvrom(4, 0x1000, 1);
   }
}

static void map163_write(uint32 address, uint8 value)
{
   nofrendo_log_printf("M163 W %04X = %02X [5000=%02X 5100=%02X 5300=%02X trig=%d latch=%02X]\n",
      address, value, reg5000, reg5100, reg5300, sec_trig, sec_latch);
   switch (address & 0xFF00)
   {
   case 0x5000:
      reg5000  = value;
      chr_auto = (value >> 7) & 1;
      map163_setprg();
      map163_setchr();
      break;

   case 0x5100:
      /* $5100 = output latch / PRG high on mapper 163 */
      reg5100 = value;
      map163_setprg();
      break;

   case 0x5101:
      /* Edge-triggered security:
         if the previously latched value was nonzero → flip trigger */
      if (sec_latch != 0x00)
         sec_trig ^= 1;
      sec_latch = value;
      break;

   case 0x5200:
      /* Not connected on mapper 163 (A8/A9 swapped vs 164) — ignore */
      break;

   case 0x5300:
      /* Mode register — NOT subject to bit-swap */
      reg5300 = value;
      map163_setprg();
      map163_setchr();
      break;

   default:
      break;
   }
}

static uint8 map163_read(uint32 address)
{
   uint8 ret = 0x00;
   if ((address & 0xF300) == 0x5100)
      ret = (~sec_trig & 1) << 2;
   nofrendo_log_printf("M163 R %04X = %02X [trig=%d]\n", address, ret, sec_trig);
   return ret;
}

static void map163_getstate(SnssMapperBlock *state)
{
   state->extraData.mapper4.irqCounter        = (int)reg5000;
   state->extraData.mapper4.irqLatchCounter   = (int)reg5100;
   state->extraData.mapper4.irqCounterEnabled = (bool)sec_trig;
   state->extraData.mapper4.last8000Write     = reg5300 | (sec_latch << 4);
}

static void map163_setstate(SnssMapperBlock *state)
{
   reg5000   = (uint8)state->extraData.mapper4.irqCounter;
   reg5100   = (uint8)state->extraData.mapper4.irqLatchCounter;
   sec_trig  = (uint8)state->extraData.mapper4.irqCounterEnabled;
   reg5300   = state->extraData.mapper4.last8000Write & 0x0F;
   sec_latch = (state->extraData.mapper4.last8000Write >> 4) & 0x0F;
   chr_auto  = (reg5000 >> 7) & 1;
   map163_setprg();
   map163_setchr();
}

static void map163_init(void)
{
   reg5000   = 0;
   reg5100   = 0;
   reg5300   = 0; /* A=0: boots in last bank */
   sec_latch = 0;
   sec_trig  = 0;
   chr_auto  = 0;

   map163_setprg();
   mmc_bankvrom(8, 0x0000, 0);
   ppu_mirror(0, 0, 1, 1); /* horizontal, hard-wired */
}

static map_memread map163_memread[] =
{
   {0x5100, 0x5101, map163_read},  /* solo gli indirizzi di feedback */
   {0x5500, 0x5501, map163_read},
   {-1, -1, NULL}
};

static map_memwrite map163_memwrite[] =
{
   {0x5000, 0x5301, map163_write},  /* solo $5000-$5301 */
   {-1, -1, NULL}
};

mapintf_t map163_intf =
{
   163,
   "NANJING",
   map163_init,
   NULL,
   NULL,
   map163_getstate,
   map163_setstate,
   map163_memread,
   map163_memwrite,
   NULL
};