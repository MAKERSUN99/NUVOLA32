/*
** Nofrendo MMC5 Mapper (map005.c)
** Minimal, stable implementation for ESP32 targets
*/

#include "../noftypes.h"
#include "../nes/nes_mmc.h"
#include "../nes/nes.h"
#include "../log.h"
#include "../sndhrdw/mmc5_snd.h"

static struct
{
   int counter;
   int latch;
   int enabled;
   int reset;
} irq;

/* -------------------------------------------------
 * HBlank callback (IRQ scanline)
 * ------------------------------------------------- */
static void map5_hblank(int vblank)
{
   UNUSED(vblank);

   if (!irq.enabled)
      return;

   if (nes_getcontextptr()->scanline == irq.counter)
   {
      nes_irq();
      irq.reset = 1;
      irq.counter = irq.latch;
   }
}

/* -------------------------------------------------
 * Write handler
 * ------------------------------------------------- */
static void map5_write(uint32 address, uint8 value)
{
   static int prg_page_size = 8;

   /* ExRAM area (ignored) */
   if (address >= 0x5C00 && address <= 0x5FFF)
      return;

   switch (address)
   {
      /* PRG page size */
      case 0x5100:
         switch (value & 3)
         {
            case 0: prg_page_size = 32; break;
            case 1: prg_page_size = 16; break;
            default: prg_page_size = 8; break;
         }
         return;

      /* Mirroring */
      case 0x5105:
         ppu_mirror(value & 3,
                    (value >> 2) & 3,
                    (value >> 4) & 3,
                    value >> 6);
         return;

      /* PRG banks */
      case 0x5114:
         mmc_bankrom(8, 0x8000, value);
         return;

      case 0x5115:
         mmc_bankrom(8, 0x8000, value);
         mmc_bankrom(8, 0xA000, value + 1);
         return;

      case 0x5116:
         mmc_bankrom(8, 0xC000, value);
         return;

      case 0x5117:
         mmc_bankrom(8, 0xE000, value);
         return;

      /* CHR banks */
      case 0x5120: mmc_bankvrom(1, 0x0000, value); return;
      case 0x5121: mmc_bankvrom(1, 0x0400, value); return;
      case 0x5122: mmc_bankvrom(1, 0x0800, value); return;
      case 0x5123: mmc_bankvrom(1, 0x0C00, value); return;
      case 0x5128: mmc_bankvrom(1, 0x1000, value); return;
      case 0x5129: mmc_bankvrom(1, 0x1400, value); return;
      case 0x512A: mmc_bankvrom(1, 0x1800, value); return;
      case 0x512B: mmc_bankvrom(1, 0x1C00, value); return;

      /* IRQ latch */
      case 0x5203:
         irq.latch   = value;
         irq.counter = value;
         irq.reset   = 0;
         return;

      /* IRQ enable */
      case 0x5204:
         irq.enabled = (value & 0x80) ? 1 : 0;
         irq.reset   = 0;
         return;

      /* IRQ status write (ACK) */
      case 0x5200:
         irq.reset = 0;
         return;

      /* Ignored registers */
      case 0x5101:
      case 0x5104:
      case 0x5106:
      case 0x5107:
      case 0x5113:
      case 0x5124:
      case 0x5125:
      case 0x5126:
      case 0x5127:
         return;

      default:
         return;
   }
}

/* -------------------------------------------------
 * Read handler
 * ------------------------------------------------- */
static uint8 map5_read(uint32 address)
{
   if (address == 0x5204)
      return irq.reset ? 0x40 : 0x00;

   return 0xFF;
}

/* -------------------------------------------------
 * Init
 * ------------------------------------------------- */
static void map5_init(void)
{
   mmc_bankrom(8, 0x8000, MMC_LASTBANK);
   mmc_bankrom(8, 0xA000, MMC_LASTBANK);
   mmc_bankrom(8, 0xC000, MMC_LASTBANK);
   mmc_bankrom(8, 0xE000, MMC_LASTBANK);

   irq.counter = 0;
   irq.latch   = 0;
   irq.enabled = 0;
   irq.reset   = 0;
}

/* -------------------------------------------------
 * Save state (stub)
 * ------------------------------------------------- */
static void map5_getstate(SnssMapperBlock *state)
{
   state->extraData.mapper5.dummy = 0;
}

static void map5_setstate(SnssMapperBlock *state)
{
   UNUSED(state);
}

/* -------------------------------------------------
 * Memory maps
 * ------------------------------------------------- */
static map_memwrite map5_memwrite[] =
{
   {0x5016, 0x5FFF, map5_write},
   {0x8000, 0xFFFF, map5_write},
   {-1, -1, NULL}
};

static map_memread map5_memread[] =
{
   {0x5204, 0x5204, map5_read},
   {-1, -1, NULL}
};

/* -------------------------------------------------
 * Mapper interface
 * ------------------------------------------------- */
mapintf_t map5_intf =
{
   5,
   "MMC5",
   map5_init,
   NULL,
   map5_hblank,
   map5_getstate,
   map5_setstate,
   map5_memread,
   map5_memwrite,
   &mmc5_ext
};
