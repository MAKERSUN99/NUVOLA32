/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
** map118.c
**
** Mapper 118 - TxSROM (MMC3 variant)
**
** Identical to MMC3 (mapper 4) with one difference:
** mirroring is NOT controlled by register $A000.
** Instead, bit 7 of each CHR bank register (R2-R5) selects
** which VRAM page (0 or 1) that 1KB CHR bank maps to.
**
** This implements single-screen mirroring switchable per CHR bank,
** used by: Strange Tales of the Dragon Kings, Goal! Two, etc.
**
** Reference: https://www.nesdev.org/wiki/MMC3
**            https://www.nesdev.org/wiki/TxSROM
*/

#include "../noftypes.h"
#include "../nes/nes_mmc.h"
#include "../nes/nes.h"
#include "../libsnss/libsnss.h"

static struct
{
   int counter, latch;
   bool enabled, reset;
} irq;

static uint8 reg;
static uint8 command;
static uint16 vrombase;

/* CHR bank registers R2-R5 (1KB each), stored to recompute mirroring */
static uint8 chr_reg[6]; /* R0..R5 */

/*
** map118_syncmirror
**
** Mapper 118 mirroring is derived from bit 7 of CHR registers R2-R5.
** The PPU nametable layout is determined by which VRAM page each
** CHR bank selects:
**
**   Nametable A (PPU $2000) = page selected by CHR bank at $0000
**   Nametable B (PPU $2400) = page selected by CHR bank at $0400
**   Nametable C (PPU $2800) = page selected by CHR bank at $0800
**   Nametable D (PPU $2C00) = page selected by CHR bank at $0C00
**
** With vrombase=0x0000 (default):
**   $0000-$07FF -> R0/R1 (2KB, bit7 ignored for mirroring in these)
**   $0800-$0BFF -> R2    bit7 -> nametable page for this region
**   $0C00-$0FFF -> R3    bit7
**   $1000-$13FF -> R4    bit7
**   $1400-$17FF -> R5    bit7
**
** In practice the commonly observed behaviour maps as:
**   mirror page A = (chr_reg[2] >> 7) & 1
**   mirror page B = (chr_reg[4] >> 7) & 1
** giving either horizontal or vertical mirroring depending on game.
**
** ppu_mirror(nt0, nt1, nt2, nt3) sets the four nametable pages.
*/
static void map118_syncmirror(void)
{
   uint8 nt0 = (chr_reg[2] >> 7) & 1;
   uint8 nt1 = (chr_reg[3] >> 7) & 1;
   uint8 nt2 = (chr_reg[4] >> 7) & 1;
   uint8 nt3 = (chr_reg[5] >> 7) & 1;

   ppu_mirror(nt0, nt1, nt2, nt3);
}

static void map118_write(uint32 address, uint8 value)
{
   switch (address & 0xE001)
   {
   case 0x8000:
      command = value;
      vrombase = (command & 0x80) ? 0x1000 : 0x0000;

      if (reg != (value & 0x40))
      {
         if (value & 0x40)
            mmc_bankrom(8, 0x8000, (mmc_getinfo()->rom_banks * 2) - 2);
         else
            mmc_bankrom(8, 0xC000, (mmc_getinfo()->rom_banks * 2) - 2);
      }
      reg = value & 0x40;
      /* vrombase changed: recompute mirroring */
      map118_syncmirror();
      break;

   case 0x8001:
      switch (command & 0x07)
      {
      case 0:
         chr_reg[0] = value;
         value &= 0xFE;
         mmc_bankvrom(1, vrombase ^ 0x0000, value);
         mmc_bankvrom(1, vrombase ^ 0x0400, value + 1);
         break;

      case 1:
         chr_reg[1] = value;
         value &= 0xFE;
         mmc_bankvrom(1, vrombase ^ 0x0800, value);
         mmc_bankvrom(1, vrombase ^ 0x0C00, value + 1);
         break;

      case 2:
         chr_reg[2] = value;
         mmc_bankvrom(1, vrombase ^ 0x1000, value & 0x7F); /* mask bit7: not a CHR address bit */
         map118_syncmirror();
         break;

      case 3:
         chr_reg[3] = value;
         mmc_bankvrom(1, vrombase ^ 0x1400, value & 0x7F);
         map118_syncmirror();
         break;

      case 4:
         chr_reg[4] = value;
         mmc_bankvrom(1, vrombase ^ 0x1800, value & 0x7F);
         map118_syncmirror();
         break;

      case 5:
         chr_reg[5] = value;
         mmc_bankvrom(1, vrombase ^ 0x1C00, value & 0x7F);
         map118_syncmirror();
         break;

      case 6:
         mmc_bankrom(8, (command & 0x40) ? 0xC000 : 0x8000, value);
         break;

      case 7:
         mmc_bankrom(8, 0xA000, value);
         break;
      }
      break;

   case 0xA000:
      /* Mapper 118: registro mirroring MMC3 IGNORATO.
         Il mirroring è interamente controllato dal bit 7 dei registri CHR. */
      break;

   case 0xA001:
      /* Save RAM enable/disable - non implementato (come in map004) */
      break;

   case 0xC000:
      irq.latch = value;
      break;

   case 0xC001:
      irq.reset = true;
      irq.counter = irq.latch;
      break;

   case 0xE000:
      irq.enabled = false;
      break;

   case 0xE001:
      irq.enabled = true;
      break;

   default:
      __asm__("nop");
      break;
   }

   if (true == irq.reset)
      irq.counter = irq.latch;
}

static void map118_hblank(int vblank)
{
   if (vblank)
      return;

   if (ppu_enabled())
   {
      if (irq.counter >= 0)
      {
         irq.reset = false;
         irq.counter--;

         if (irq.counter < 0)
         {
            if (irq.enabled)
            {
               irq.reset = true;
               nes_irq();
            }
         }
      }
   }
}

static void map118_getstate(SnssMapperBlock *state)
{
   state->extraData.mapper4.irqCounter        = irq.counter;
   state->extraData.mapper4.irqLatchCounter   = irq.latch;
   state->extraData.mapper4.irqCounterEnabled = irq.enabled;
   state->extraData.mapper4.last8000Write     = command;
}

static void map118_setstate(SnssMapperBlock *state)
{
   irq.counter = state->extraData.mapper4.irqCounter;
   irq.latch   = state->extraData.mapper4.irqLatchCounter;
   irq.enabled = state->extraData.mapper4.irqCounterEnabled;
   command     = state->extraData.mapper4.last8000Write;
   map118_syncmirror();
}

static void map118_init(void)
{
   int i;

   irq.counter = irq.latch = 0;
   irq.enabled = irq.reset = false;
   reg = command = 0;
   vrombase = 0x0000;

   for (i = 0; i < 6; i++)
      chr_reg[i] = 0;
}

static map_memwrite map118_memwrite[] =
{
   {0x8000, 0xFFFF, map118_write},
   {-1, -1, NULL}
};

mapintf_t map118_intf =
{
   118,              /* mapper number */
   "TxSROM",         /* mapper name */
   map118_init,      /* init routine */
   NULL,             /* vblank callback */
   map118_hblank,    /* hblank callback */
   map118_getstate,  /* get state (snss) */
   map118_setstate,  /* set state (snss) */
   NULL,             /* memory read structure */
   map118_memwrite,  /* memory write structure */
   NULL              /* external sound device */
};