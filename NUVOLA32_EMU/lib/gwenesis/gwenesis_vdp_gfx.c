/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"
*/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "m68k.h"
#include "gwenesis_vdp.h"
#include "gwenesis_io.h"
#include "gwenesis_bus.h"
#include "gwenesis_savestate.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
//#include "displayGW.h"

uint64_t t_frame_start;
uint64_t t_render_end;
uint64_t t_display_end;

uint32_t frame_counter = 0;
uint64_t fps_timer = 0;

// Flag dirty CRAM: alzato dal bus quando la palette viene scritta,
// abbassato da gwenesis_vdp_render_config() dopo aver ricalcolato le LUT.
// Evita di ricalcolare CRAM565 ogni frame quando la palette non è cambiata.
bool g_cram_dirty = true;

// extern void GWENESIS_PUSH_SCANLINE(int line, const uint16_t *src, int w);

#if GNW_TARGET_MARIO != 0 || GNW_TARGET_ZELDA != 0
#pragma GCC optimize("Ofast")
#endif

#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#include "stm32h7b0xx.h"
extern unsigned char *VRAM;
#else
#include <stdint.h>
extern unsigned char *VRAM;
#endif

extern unsigned short *CRAM;
extern unsigned char *SAT_CACHE;
extern unsigned char gwenesis_vdp_regs[];
extern unsigned short *CRAM565;
extern unsigned short *VSRAM;

uint16_t *CRAM565_SH = NULL;
uint16_t *CRAM565_HI = NULL;

unsigned char *screen, *scaled_screen;

enum
{
    PIX_OVERFLOW = 32
};

// Buffer hot — allocati in SRAM interna DMA-capable per latenza minima
static uint8_t *render_buffer = NULL;
static uint8_t *sprite_buffer = NULL;
static uint16_t *line565 = NULL;

static int mode_h40;
int mode_pal;

int screen_width;
int screen_height;

int gwenesis_H32upscaler;
int sprite_overflow;
bool sprite_collision;

static int base_w;
static int PlanA_firstcol, PlanA_lastcol;
static int Window_firstcol, Window_lastcol;

static uint16_t ntwidth_x2;
static uint16_t ntw_mask, nth_mask;

void GWENESIS_PUSH_SCANLINE(int line, const uint16_t *__restrict src, int width);

// ─────────────────────────────────────────────────────────────────────────────
// Logging (disabilitato in produzione)
// ─────────────────────────────────────────────────────────────────────────────

#define VDP_GFX_DISABLE_LOGGING 1

#if !VDP_GFX_DISABLE_LOGGING
#include <stdarg.h>
void vdpg_log(const char *subs, const char *fmt, ...)
{
    extern int frame_counter;
    extern int scan_line;
    va_list va;
    printf("%06d:%03d :[%s] vc:%03x hc:%03x",
           frame_counter, scan_line, subs,
           gwenesis_vdp_vcounter(), gwenesis_vdp_hcounter());
    va_start(va, fmt);
    vfprintf(stdout, fmt, va);
    va_end(va);
    printf("\n");
}
#else
#define vdpg_log(...) \
    do                \
    {                 \
    } while (0)
#endif

// ─────────────────────────────────────────────────────────────────────────────
// VRAM access
// ─────────────────────────────────────────────────────────────────────────────

#define FETCH16VRAM(A) (__builtin_bswap16(*(const uint16_t *)((const uint8_t *)VRAM + (A))))

// ─────────────────────────────────────────────────────────────────────────────
// Allocazione buffer — SRAM interna per tutti i buffer hot
// ─────────────────────────────────────────────────────────────────────────────

void gwenesis_vdp_set_buffers(unsigned char *screen_buffer, unsigned char *scaled_buffer)
{
    screen = screen_buffer;
    scaled_screen = scaled_buffer;
}

void gwenesis_vdp_set_buffer(unsigned short *ptr_screen_buffer)
{
    (void)ptr_screen_buffer; // non usato su embedded
}

void gwenesis_vdp_allocate_buffers()
{
    // Dimensione sempre al massimo H40 (320) per evitare overflow a runtime
    const size_t buffer_size = 320 + PIX_OVERFLOW * 2;

    // SAT_CACHE, CRAM, VSRAM: acceduti frequentemente nel renderer
    // → SRAM interna per latenza minima
    SAT_CACHE = (unsigned char *)heap_caps_calloc(
        SAT_CACHE_MAX_SIZE, sizeof(unsigned char),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    CRAM = (unsigned short *)heap_caps_calloc(
        CRAM_MAX_SIZE, sizeof(unsigned short),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // CRAM565 × 4 per shadow/highlight — letti ad ogni pixel nel blit finale
    // → SRAM interna obbligatoria
    CRAM565 = (unsigned short *)heap_caps_calloc(
        CRAM_MAX_SIZE * 4, sizeof(unsigned short),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    VSRAM = (unsigned short *)heap_caps_calloc(
        VSRAM_MAX_SIZE, sizeof(unsigned short),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    CRAM565_SH = (uint16_t *)heap_caps_calloc(
        64, sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    CRAM565_HI = (uint16_t *)heap_caps_calloc(
        64, sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // render_buffer, sprite_buffer, line565: scritti/letti 320 volte per scanline
    // → SRAM interna + DMA per pushPixels successivo
    if (!render_buffer)
        render_buffer = (uint8_t *)heap_caps_calloc(
            buffer_size, sizeof(uint8_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (!sprite_buffer)
        sprite_buffer = (uint8_t *)heap_caps_calloc(
            buffer_size, sizeof(uint8_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (!line565)
        line565 = (uint16_t *)heap_caps_calloc(
            buffer_size, sizeof(uint16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    // OOM check — abort immediato con messaggio diagnostico
    if (!SAT_CACHE || !CRAM || !CRAM565 || !VSRAM ||
        !CRAM565_SH || !CRAM565_HI || !render_buffer ||
        !sprite_buffer || !line565)
    {
        printf("[FATAL] gwenesis_vdp_allocate_buffers: OOM\n");
        abort();
    }

    g_cram_dirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pixel attribute macros
// ─────────────────────────────────────────────────────────────────────────────

#define PIX0(P) (((P) & 0x000000F0) >> 4)
#define PIX1(P) (((P) & 0x0000000F) >> 0)
#define PIX2(P) (((P) & 0x0000F000) >> 12)
#define PIX3(P) (((P) & 0x00000F00) >> 8)
#define PIX4(P) (((P) & 0x00F00000) >> 20)
#define PIX5(P) (((P) & 0x000F0000) >> 16)
#define PIX6(P) (((P) & 0xF0000000) >> 28)
#define PIX7(P) (((P) & 0x0F000000) >> 24)

// ─────────────────────────────────────────────────────────────────────────────
// Pattern draw — sprite
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) void
draw_pattern_nofliph_sprite(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    if (p == 0)
        return;
    if ((PIX0(p)) && ((scr[0] & PIXATTR_SPRITE) == 0))
        scr[0] = attrs | PIX0(p);
    if ((PIX1(p)) && ((scr[1] & PIXATTR_SPRITE) == 0))
        scr[1] = attrs | PIX1(p);
    if ((PIX2(p)) && ((scr[2] & PIXATTR_SPRITE) == 0))
        scr[2] = attrs | PIX2(p);
    if ((PIX3(p)) && ((scr[3] & PIXATTR_SPRITE) == 0))
        scr[3] = attrs | PIX3(p);
    if ((PIX4(p)) && ((scr[4] & PIXATTR_SPRITE) == 0))
        scr[4] = attrs | PIX4(p);
    if ((PIX5(p)) && ((scr[5] & PIXATTR_SPRITE) == 0))
        scr[5] = attrs | PIX5(p);
    if ((PIX6(p)) && ((scr[6] & PIXATTR_SPRITE) == 0))
        scr[6] = attrs | PIX6(p);
    if ((PIX7(p)) && ((scr[7] & PIXATTR_SPRITE) == 0))
        scr[7] = attrs | PIX7(p);
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_sprite(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    if (p == 0)
        return;
    if ((PIX7(p)) && ((scr[0] & PIXATTR_SPRITE) == 0))
        scr[0] = attrs | PIX7(p);
    if ((PIX6(p)) && ((scr[1] & PIXATTR_SPRITE) == 0))
        scr[1] = attrs | PIX6(p);
    if ((PIX5(p)) && ((scr[2] & PIXATTR_SPRITE) == 0))
        scr[2] = attrs | PIX5(p);
    if ((PIX4(p)) && ((scr[3] & PIXATTR_SPRITE) == 0))
        scr[3] = attrs | PIX4(p);
    if ((PIX3(p)) && ((scr[4] & PIXATTR_SPRITE) == 0))
        scr[4] = attrs | PIX3(p);
    if ((PIX2(p)) && ((scr[5] & PIXATTR_SPRITE) == 0))
        scr[5] = attrs | PIX2(p);
    if ((PIX1(p)) && ((scr[6] & PIXATTR_SPRITE) == 0))
        scr[6] = attrs | PIX1(p);
    if ((PIX0(p)) && ((scr[7] & PIXATTR_SPRITE) == 0))
        scr[7] = attrs | PIX0(p);
}

static inline __attribute__((always_inline)) void
draw_pattern_nofliph_sprite_over_planes(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    if (p == 0)
        return;
    if (attrs & PIXATTR_HIPRI)
    {
        if ((PIX0(p)) && ((scr[0] & PIXATTR_SPRITE) == 0))
            scr[0] = attrs | PIX0(p);
        if ((PIX1(p)) && ((scr[1] & PIXATTR_SPRITE) == 0))
            scr[1] = attrs | PIX1(p);
        if ((PIX2(p)) && ((scr[2] & PIXATTR_SPRITE) == 0))
            scr[2] = attrs | PIX2(p);
        if ((PIX3(p)) && ((scr[3] & PIXATTR_SPRITE) == 0))
            scr[3] = attrs | PIX3(p);
        if ((PIX4(p)) && ((scr[4] & PIXATTR_SPRITE) == 0))
            scr[4] = attrs | PIX4(p);
        if ((PIX5(p)) && ((scr[5] & PIXATTR_SPRITE) == 0))
            scr[5] = attrs | PIX5(p);
        if ((PIX6(p)) && ((scr[6] & PIXATTR_SPRITE) == 0))
            scr[6] = attrs | PIX6(p);
        if ((PIX7(p)) && ((scr[7] & PIXATTR_SPRITE) == 0))
            scr[7] = attrs | PIX7(p);
    }
    else
    {
        if ((PIX0(p)) && ((scr[0] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[0] = attrs | PIX0(p);
        if ((PIX1(p)) && ((scr[1] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[1] = attrs | PIX1(p);
        if ((PIX2(p)) && ((scr[2] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[2] = attrs | PIX2(p);
        if ((PIX3(p)) && ((scr[3] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[3] = attrs | PIX3(p);
        if ((PIX4(p)) && ((scr[4] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[4] = attrs | PIX4(p);
        if ((PIX5(p)) && ((scr[5] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[5] = attrs | PIX5(p);
        if ((PIX6(p)) && ((scr[6] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[6] = attrs | PIX6(p);
        if ((PIX7(p)) && ((scr[7] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[7] = attrs | PIX7(p);
    }
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_sprite_over_planes(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    if (p == 0)
        return;
    if (attrs & PIXATTR_HIPRI)
    {
        if ((PIX7(p)) && ((scr[0] & PIXATTR_SPRITE) == 0))
            scr[0] = attrs | PIX7(p);
        if ((PIX6(p)) && ((scr[1] & PIXATTR_SPRITE) == 0))
            scr[1] = attrs | PIX6(p);
        if ((PIX5(p)) && ((scr[2] & PIXATTR_SPRITE) == 0))
            scr[2] = attrs | PIX5(p);
        if ((PIX4(p)) && ((scr[3] & PIXATTR_SPRITE) == 0))
            scr[3] = attrs | PIX4(p);
        if ((PIX3(p)) && ((scr[4] & PIXATTR_SPRITE) == 0))
            scr[4] = attrs | PIX3(p);
        if ((PIX2(p)) && ((scr[5] & PIXATTR_SPRITE) == 0))
            scr[5] = attrs | PIX2(p);
        if ((PIX1(p)) && ((scr[6] & PIXATTR_SPRITE) == 0))
            scr[6] = attrs | PIX1(p);
        if ((PIX0(p)) && ((scr[7] & PIXATTR_SPRITE) == 0))
            scr[7] = attrs | PIX0(p);
    }
    else
    {
        if ((PIX7(p)) && ((scr[0] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[0] = attrs | PIX7(p);
        if ((PIX6(p)) && ((scr[1] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[1] = attrs | PIX6(p);
        if ((PIX5(p)) && ((scr[2] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[2] = attrs | PIX5(p);
        if ((PIX4(p)) && ((scr[3] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[3] = attrs | PIX4(p);
        if ((PIX3(p)) && ((scr[4] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[4] = attrs | PIX3(p);
        if ((PIX2(p)) && ((scr[5] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[5] = attrs | PIX2(p);
        if ((PIX1(p)) && ((scr[6] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[6] = attrs | PIX1(p);
        if ((PIX0(p)) && ((scr[7] & PIXATTR_SPRITE_HIPRI) == 0))
            scr[7] = attrs | PIX0(p);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pattern draw — plane B
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) void
draw_pattern_nofliph_planeB(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    const uint8_t back = gwenesis_vdp_regs[7];
    if (p == 0)
    {
        scr[0] = scr[1] = scr[2] = scr[3] =
            scr[4] = scr[5] = scr[6] = scr[7] = back;
        return;
    }
    scr[0] = PIX0(p) ? attrs | PIX0(p) : back;
    scr[1] = PIX1(p) ? attrs | PIX1(p) : back;
    scr[2] = PIX2(p) ? attrs | PIX2(p) : back;
    scr[3] = PIX3(p) ? attrs | PIX3(p) : back;
    scr[4] = PIX4(p) ? attrs | PIX4(p) : back;
    scr[5] = PIX5(p) ? attrs | PIX5(p) : back;
    scr[6] = PIX6(p) ? attrs | PIX6(p) : back;
    scr[7] = PIX7(p) ? attrs | PIX7(p) : back;
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_planeB(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    const uint8_t back = gwenesis_vdp_regs[7];
    if (p == 0)
    {
        scr[0] = scr[1] = scr[2] = scr[3] =
            scr[4] = scr[5] = scr[6] = scr[7] = back;
        return;
    }
    scr[0] = PIX7(p) ? attrs | PIX7(p) : back;
    scr[1] = PIX6(p) ? attrs | PIX6(p) : back;
    scr[2] = PIX5(p) ? attrs | PIX5(p) : back;
    scr[3] = PIX4(p) ? attrs | PIX4(p) : back;
    scr[4] = PIX3(p) ? attrs | PIX3(p) : back;
    scr[5] = PIX2(p) ? attrs | PIX2(p) : back;
    scr[6] = PIX1(p) ? attrs | PIX1(p) : back;
    scr[7] = PIX0(p) ? attrs | PIX0(p) : back;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pattern draw — plane A over B
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) void
draw_pattern_nofliph_planeAoverB(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    if (p == 0)
        return;
    if (attrs & PIXATTR_HIPRI)
    {
        if (PIX0(p))
            scr[0] = attrs | PIX0(p);
        if (PIX1(p))
            scr[1] = attrs | PIX1(p);
        if (PIX2(p))
            scr[2] = attrs | PIX2(p);
        if (PIX3(p))
            scr[3] = attrs | PIX3(p);
        if (PIX4(p))
            scr[4] = attrs | PIX4(p);
        if (PIX5(p))
            scr[5] = attrs | PIX5(p);
        if (PIX6(p))
            scr[6] = attrs | PIX6(p);
        if (PIX7(p))
            scr[7] = attrs | PIX7(p);
    }
    else
    {
        if (PIX0(p) && ((scr[0] & PIXATTR_HIPRI) == 0))
            scr[0] = attrs | PIX0(p);
        if (PIX1(p) && ((scr[1] & PIXATTR_HIPRI) == 0))
            scr[1] = attrs | PIX1(p);
        if (PIX2(p) && ((scr[2] & PIXATTR_HIPRI) == 0))
            scr[2] = attrs | PIX2(p);
        if (PIX3(p) && ((scr[3] & PIXATTR_HIPRI) == 0))
            scr[3] = attrs | PIX3(p);
        if (PIX4(p) && ((scr[4] & PIXATTR_HIPRI) == 0))
            scr[4] = attrs | PIX4(p);
        if (PIX5(p) && ((scr[5] & PIXATTR_HIPRI) == 0))
            scr[5] = attrs | PIX5(p);
        if (PIX6(p) && ((scr[6] & PIXATTR_HIPRI) == 0))
            scr[6] = attrs | PIX6(p);
        if (PIX7(p) && ((scr[7] & PIXATTR_HIPRI) == 0))
            scr[7] = attrs | PIX7(p);
    }
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_planeAoverB(uint8_t *scr, uint32_t p, uint8_t attrs)
{
    if (p == 0)
        return;
    if (attrs & PIXATTR_HIPRI)
    {
        if (PIX7(p))
            scr[0] = attrs | PIX7(p);
        if (PIX6(p))
            scr[1] = attrs | PIX6(p);
        if (PIX5(p))
            scr[2] = attrs | PIX5(p);
        if (PIX4(p))
            scr[3] = attrs | PIX4(p);
        if (PIX3(p))
            scr[4] = attrs | PIX3(p);
        if (PIX2(p))
            scr[5] = attrs | PIX2(p);
        if (PIX1(p))
            scr[6] = attrs | PIX1(p);
        if (PIX0(p))
            scr[7] = attrs | PIX0(p);
    }
    else
    {
        if (PIX7(p) && ((scr[0] & PIXATTR_HIPRI) == 0))
            scr[0] = attrs | PIX7(p);
        if (PIX6(p) && ((scr[1] & PIXATTR_HIPRI) == 0))
            scr[1] = attrs | PIX6(p);
        if (PIX5(p) && ((scr[2] & PIXATTR_HIPRI) == 0))
            scr[2] = attrs | PIX5(p);
        if (PIX4(p) && ((scr[3] & PIXATTR_HIPRI) == 0))
            scr[3] = attrs | PIX4(p);
        if (PIX3(p) && ((scr[4] & PIXATTR_HIPRI) == 0))
            scr[4] = attrs | PIX3(p);
        if (PIX2(p) && ((scr[5] & PIXATTR_HIPRI) == 0))
            scr[5] = attrs | PIX2(p);
        if (PIX1(p) && ((scr[6] & PIXATTR_HIPRI) == 0))
            scr[6] = attrs | PIX1(p);
        if (PIX0(p) && ((scr[7] & PIXATTR_HIPRI) == 0))
            scr[7] = attrs | PIX0(p);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pattern draw — dispatcher sprite / plane
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) void
draw_pattern_sprite(uint8_t *scr, uint16_t name, int paty)
{
    uint8_t attrs = ((name & 0x6000) >> 9) + ((name & 0x8000) >> 8) + PIXATTR_SPRITE;
    unsigned int pattern = (name & 0x1000)
                               ? *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4))
                               : *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

    if (name & 0x0800)
        draw_pattern_fliph_sprite(scr, pattern, attrs);
    else
        draw_pattern_nofliph_sprite(scr, pattern, attrs);
}

static inline __attribute__((always_inline)) void
draw_pattern_sprite_over_planes(uint8_t *scr, uint16_t name, int paty)
{
    uint8_t attrs = ((name & 0x6000) >> 9) + ((name & 0x8000) >> 8) + PIXATTR_SPRITE;
    unsigned int pattern = (name & 0x1000)
                               ? *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4))
                               : *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

    if (name & 0x0800)
        draw_pattern_fliph_sprite_over_planes(scr, pattern, attrs);
    else
        draw_pattern_nofliph_sprite_over_planes(scr, pattern, attrs);
}

static inline __attribute__((always_inline)) void
draw_pattern_planeB(uint8_t *scr, uint16_t name, int paty)
{
    uint8_t attrs = ((name & 0x6000) >> 9) + ((name & 0x8000) >> 8);
    unsigned int pattern = (name & 0x1000)
                               ? *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4))
                               : *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

    if (name & 0x0800)
        draw_pattern_fliph_planeB(scr, pattern, attrs);
    else
        draw_pattern_nofliph_planeB(scr, pattern, attrs);
}

static inline __attribute__((always_inline)) void
draw_pattern_planeA(uint8_t *scr, uint16_t name, int paty)
{
    uint8_t attrs = ((name & 0x6000) >> 9) + ((name & 0x8000) >> 8);
    unsigned int pattern = (name & 0x1000)
                               ? *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4))
                               : *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

    if (name & 0x0800)
        draw_pattern_fliph_planeAoverB(scr, pattern, attrs);
    else
        draw_pattern_nofliph_planeAoverB(scr, pattern, attrs);
}

// ─────────────────────────────────────────────────────────────────────────────
// Horizontal scroll
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) unsigned int
get_hscroll_vram(int line)
{
    const int mode = REG11_HSCROLL_MODE;
    const unsigned int table = REG13_HSCROLL_ADDRESS;
    int idx;
    switch (mode)
    {
    case 0:
        idx = 0;
        break; // Full screen
    case 1:
        idx = (line & 7);
        break; // First 8 lines
    case 2:
        idx = (line & ~7);
        break; // Every row
    case 3:
        idx = line;
        break; // Every line
    default:
        idx = 0;
        break;
    }
    return table + idx * 4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Plane B render
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) void draw_line_b(int line)
{
    uint8_t *scr = &render_buffer[PIX_OVERFLOW];
    uint8_t *end = scr + screen_width;
    uint16_t scrollx = FETCH16VRAM(get_hscroll_vram(line) + 2) & 0x3FF;
    uint16_t *vsram = &VSRAM[1];

    const unsigned int column_scrolling = gwenesis_vdp_regs[11] & 0x4;

    scrollx = -scrollx;
    uint8_t col = (scrollx >> 3) & ntw_mask;
    uint8_t patx = scrollx & 7;

    unsigned int numcell = 0;
    scr -= patx;
    while (scr < end)
    {
        const uint16_t scrolly = *vsram + line;
        const uint8_t row = (scrolly >> 3) & nth_mask;
        const uint8_t paty = scrolly & 7;
        const unsigned int nt = REG4_NAMETABLE_B + row * ntwidth_x2;

        draw_pattern_planeB(scr, FETCH16VRAM(nt + col * 2), paty);
        col = (col + 1) & ntw_mask;
        scr += 8;
        numcell++;

        if (column_scrolling && (numcell & 1) == 0)
            vsram += 2;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Plane A + Window render
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) void draw_line_aw(int line)
{
    uint8_t *scr = &render_buffer[PIX_OVERFLOW];
    uint16_t scrollx = FETCH16VRAM(get_hscroll_vram(line) + 0) & 0x3FF;
    uint16_t *vsram = &VSRAM[0];

    const int Window_line = REG18_WINDOW_VPOS * 8;
    const int window_down = gwenesis_vdp_regs[18] & 0x80;

    int PlanA_first = PlanA_firstcol;
    int PlanA_last = PlanA_lastcol;
    int Window_last = Window_lastcol;
    int Window_first = Window_firstcol;

    if (window_down)
    {
        if (line > Window_line)
        {
            PlanA_first = PlanA_last = 0;
            Window_last = screen_width;
            Window_first = 0;
        }
    }
    else
    {
        if (line < Window_line)
        {
            PlanA_first = PlanA_last = 0;
            Window_last = screen_width;
            Window_first = 0;
        }
    }

    const unsigned int column_scrolling = gwenesis_vdp_regs[11] & 0x4;

    scrollx = -scrollx;
    uint8_t col = (scrollx >> 3) & ntw_mask;
    uint8_t patx = scrollx & 7;

    uint8_t *pos = scr + PlanA_first;
    uint8_t *end = scr + PlanA_last;

    unsigned int numcell = 0;
    pos -= patx;
    while (pos < end)
    {
        const uint16_t scrolly = *vsram + line;
        const uint8_t row = (scrolly >> 3) & nth_mask;
        const uint8_t paty = scrolly & 7;
        const unsigned int nt = REG2_NAMETABLE_A + row * ntwidth_x2;

        draw_pattern_planeA(pos, FETCH16VRAM(nt + col * 2), paty);
        col = (col + 1) & ntw_mask;
        pos += 8;
        numcell++;

        if (column_scrolling && (numcell & 1) == 0)
            vsram += 2;
    }

    // Window plane
    const int row = line >> 3;
    const int paty = line & 7;
    const int wdwidth_x2 = (screen_width == 320) ? 128 : 64;
    unsigned int nt = base_w + row * wdwidth_x2 + Window_first / 4;

    for (int i = Window_first / 8; i < Window_last / 8; ++i)
    {
        draw_pattern_planeA(end, FETCH16VRAM(nt), paty);
        nt += 2;
        end += 8;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sprite render
// ─────────────────────────────────────────────────────────────────────────────

static inline __attribute__((always_inline)) void
draw_sprites_over_planes(int line)
{
    uint8_t *scr = &render_buffer[PIX_OVERFLOW];
    uint8_t *start_table = VRAM + REG5_SAT_ADDRESS;

    const int SPRITE_TABLE_SIZE = (screen_width == 320) ? 80 : 64;
    const int MAX_SPRITES_PER_LINE = (screen_width == 320) ? 20 : 16;
    const int MAX_PIXELS_PER_LINE = (screen_width == 320) ? 320 : 256;

    bool masking = false, one_sprite_nonzero = false;
    int sidx = 0, num_sprites = 0, num_pixels = 0;

    for (int i = 0; (i < SPRITE_TABLE_SIZE) && (sidx < SPRITE_TABLE_SIZE); ++i)
    {
        uint8_t *table = start_table + sidx * 8;
        uint8_t *cache = SAT_CACHE + sidx * 8;

        int sy = ((cache[0] & 0x3) << 8) | cache[1];
        int sx = ((table[6] & 0x3) << 8) | table[7];
        uint16_t name = (table[4] << 8) | table[5];

        const int sh = BITS(cache[2], 0, 2) + 1;
        const int link = BITS(cache[3], 0, 7);
        const int sw = BITS(table[2], 2, 2) + 1;
        const int isflipv = table[4] & 0x10;
        const int isfliph = table[4] & 0x08;

        sy -= 128;
        if (line >= sy && line < sy + sh * 8)
        {
            if (sx == 0)
            {
                if (one_sprite_nonzero || (sprite_overflow == line - 1))
                    masking = true;
            }
            else
            {
                one_sprite_nonzero = true;
            }

            int row = (line - sy) >> 3;
            int paty = (line - sy) & 7;
            if (isflipv)
                row = sh - row - 1;

            sx -= 128;
            if (sx > (-sw * 8) && sx < screen_width && !masking)
            {
                name += row;
                if (isfliph)
                {
                    name += sh * (sw - 1);
                    for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++)
                    {
                        draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
                        name -= sh;
                        num_pixels += 8;
                    }
                }
                else
                {
                    for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++)
                    {
                        draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
                        name += sh;
                        num_pixels += 8;
                    }
                }
            }
            else
            {
                num_pixels += sw * 8;
            }

            if (num_pixels >= MAX_PIXELS_PER_LINE)
            {
                sprite_overflow = line;
                break;
            }
            if (++num_sprites >= MAX_SPRITES_PER_LINE)
                break;
        }
        if (link == 0)
            break;
        sidx = link;
    }
}

static inline __attribute__((always_inline)) void
draw_sprites(int line)
{
    uint8_t *scr = &sprite_buffer[PIX_OVERFLOW];
    uint8_t *start_table = VRAM + REG5_SAT_ADDRESS;

    const int SPRITE_TABLE_SIZE = (screen_width == 320) ? 80 : 64;
    const int MAX_SPRITES_PER_LINE = (screen_width == 320) ? 20 : 16;
    const int MAX_PIXELS_PER_LINE = (screen_width == 320) ? 320 : 256;

    bool masking = false, one_sprite_nonzero = false;
    int sidx = 0, num_sprites = 0, num_pixels = 0;

    for (int i = 0; i < SPRITE_TABLE_SIZE && sidx < SPRITE_TABLE_SIZE; ++i)
    {
        uint8_t *table = start_table + sidx * 8;
        uint8_t *cache = table; // SHI mode: legge direttamente dalla tabella

        int sy = ((cache[0] & 0x3) << 8) | cache[1];
        int sx = ((table[6] & 0x3) << 8) | table[7];
        uint16_t name = (table[4] << 8) | table[5];

        const int sh = BITS(cache[2], 0, 2) + 1;
        const int link = BITS(cache[3], 0, 7);
        const int sw = BITS(table[2], 2, 2) + 1;
        const int isflipv = table[4] & 0x10;
        const int isfliph = table[4] & 0x08;

        sy -= 128;
        if (line >= sy && line < sy + sh * 8)
        {
            if (sx == 0)
            {
                if (one_sprite_nonzero || sprite_overflow == line - 1)
                    masking = true;
            }
            else
            {
                one_sprite_nonzero = true;
            }

            int row = (line - sy) >> 3;
            int paty = (line - sy) & 7;
            if (isflipv)
                row = sh - row - 1;

            sx -= 128;
            if (sx > -sw * 8 && sx < screen_width && !masking)
            {
                name += row;
                if (isfliph)
                {
                    name += sh * (sw - 1);
                    for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++)
                    {
                        draw_pattern_sprite(scr + sx + p * 8, name, paty);
                        name -= sh;
                        num_pixels += 8;
                    }
                }
                else
                {
                    for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++)
                    {
                        draw_pattern_sprite(scr + sx + p * 8, name, paty);
                        name += sh;
                        num_pixels += 8;
                    }
                }
            }
            else
            {
                num_pixels += sw * 8;
            }

            if (num_pixels >= MAX_PIXELS_PER_LINE)
            {
                sprite_overflow = line;
                break;
            }
            if (++num_sprites >= MAX_SPRITES_PER_LINE)
                break;
        }
        if (link == 0)
            break;
        sidx = link;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Conversione colore CRAM → RGB565
//
// LUT precalcolate per eliminare le divisioni intere da md_cram_to_rgb565,
// dim565 e boost565. Ogni funzione veniva chiamata 64× per frame con 3
// divisioni ciascuna = 192 divisioni/frame → 0 con le LUT.
// ─────────────────────────────────────────────────────────────────────────────

// Espansione lineare 3bit → 5bit  (x * 31 / 7)
static const uint8_t lut_r3_to_r5[8] = {0, 4, 9, 13, 18, 22, 27, 31};
// Espansione lineare 3bit → 6bit  (x * 63 / 7)
static const uint8_t lut_r3_to_g6[8] = {0, 9, 18, 27, 36, 45, 54, 63};

// Dim: dimezza il canale 3bit, poi espande
static const uint8_t lut_dim_r5[8] = {0, 2, 4, 6, 9, 11, 13, 15};
static const uint8_t lut_dim_g6[8] = {0, 4, 9, 13, 18, 22, 27, 31};

// Boost: incrementa il canale 3bit di ~50% con clamp a 7, poi espande
static const uint8_t lut_boost_r5[8] = {0, 6, 13, 22, 27, 31, 31, 31};
static const uint8_t lut_boost_g6[8] = {0, 13, 27, 45, 54, 63, 63, 63};

static inline uint16_t md_cram_to_rgb565(uint16_t c)
{
    const uint16_t r3 = (c >> 1) & 0x7;
    const uint16_t g3 = (c >> 5) & 0x7;
    const uint16_t b3 = (c >> 9) & 0x7;
    return ((uint16_t)lut_r3_to_r5[r3] << 11) | ((uint16_t)lut_r3_to_g6[g3] << 5) | (uint16_t)lut_r3_to_r5[b3];
}

static inline uint16_t dim565(uint16_t c)
{
    const uint16_t r3 = (c >> 1) & 0x7;
    const uint16_t g3 = (c >> 5) & 0x7;
    const uint16_t b3 = (c >> 9) & 0x7;
    return ((uint16_t)lut_dim_r5[r3] << 11) | ((uint16_t)lut_dim_g6[g3] << 5) | (uint16_t)lut_dim_r5[b3];
}

static inline uint16_t boost565(uint16_t c)
{
    const uint16_t r3 = (c >> 1) & 0x7;
    const uint16_t g3 = (c >> 5) & 0x7;
    const uint16_t b3 = (c >> 9) & 0x7;
    return ((uint16_t)lut_boost_r5[r3] << 11) | ((uint16_t)lut_boost_g6[g3] << 5) | (uint16_t)lut_boost_r5[b3];
}

// ─────────────────────────────────────────────────────────────────────────────
// gwenesis_vdp_render_config — chiamato una volta per frame
// Ricalcola CRAM565 solo se la palette è cambiata (g_cram_dirty)
// ─────────────────────────────────────────────────────────────────────────────

void IRAM_ATTR gwenesis_vdp_render_config()
{
    // Ricalcola palette solo se modificata dal bus
    if (g_cram_dirty)
    {
        for (int i = 0; i < 64; ++i)
        {
            CRAM565[i] = md_cram_to_rgb565(CRAM[i]);
            CRAM565_SH[i] = dim565(CRAM[i]); // dim/boost operano sui bit CRAM raw
            CRAM565_HI[i] = boost565(CRAM[i]);
        }
        g_cram_dirty = false;
    }

    // mode_h40 letto qui una volta sola — NON riletto in gwenesis_vdp_render_line
    mode_h40 = REG12_MODE_H40;
    mode_pal = REG1_PAL;
    screen_width = mode_h40 ? 320 : 256;
    screen_height = mode_pal ? 240 : 224;

    int ntwidth = BITS(gwenesis_vdp_regs[16], 0, 2);
    int ntheight = BITS(gwenesis_vdp_regs[16], 4, 2);
    ntwidth = (ntwidth + 1) * 32;
    ntheight = (ntheight + 1) * 32;
    ntw_mask = ntwidth - 1;
    nth_mask = ntheight - 1;
    ntwidth_x2 = ntwidth * 2;

    if (mode_h40)
        base_w = ((REG3_NAMETABLE_W & 0x1e) << 11);
    else
        base_w = ((REG3_NAMETABLE_W & 0x1f) << 11);

    const bool window_right = BIT(gwenesis_vdp_regs[17], 7);

    PlanA_firstcol = 0;
    PlanA_lastcol = screen_width;
    Window_firstcol = 0;
    Window_lastcol = 0;

    if (window_right)
    {
        Window_firstcol = REG17_WINDOW_HPOS * 16;
        Window_lastcol = screen_width;
        if (Window_firstcol > Window_lastcol)
            Window_firstcol = Window_lastcol;
        PlanA_firstcol = 0;
        PlanA_lastcol = Window_firstcol;
    }
    else
    {
        Window_firstcol = 0;
        Window_lastcol = REG17_WINDOW_HPOS * 16;
        if (Window_lastcol > screen_width)
            Window_lastcol = screen_width;
        PlanA_firstcol = Window_lastcol;
        PlanA_lastcol = screen_width;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// gwenesis_vdp_render_line — hot path, chiamata fino a 240× per frame
//
// Modifiche rispetto all'originale:
// - mode_h40 NON riletto (già settato in render_config)
// - blit loop srotolato a x8 con __builtin_prefetch
// - t_render_end aggiornato in ENTRAMBI i path (Normal e SHI)
// - GWENESIS_PUSH_SCANLINE_OPT → GWENESIS_PUSH_SCANLINE (unificato in display.cpp)
// ─────────────────────────────────────────────────────────────────────────────

__attribute__((optimize("unroll-loops"))) void IRAM_ATTR
gwenesis_vdp_render_line(int line)
{
    // Interlace non supportata
    if (BITS(gwenesis_vdp_regs[12], 1, 2) != 0)
        return;

    const int vis_h = REG1_PAL ? 240 : 224;
    if (line >= vis_h)
        return;
    if (REG0_DISABLE_DISPLAY)
        return;

    uint8_t *pb = &render_buffer[PIX_OVERFLOW];
    uint8_t *ps = &sprite_buffer[PIX_OVERFLOW];

    if (MODE_SHI)
        memset(ps, 0, 320);

    draw_line_b(line);
    draw_line_aw(line);

    if (MODE_SHI)
        draw_sprites(line);
    else
        draw_sprites_over_planes(line);

    const int w = screen_width;

    // ── Normal Mode ──────────────────────────────────────────────────────────
    if (!MODE_SHI)
    {
        // Loop srotolato x8 con prefetch — massimizza l'uso della cache S3
        for (int x = 0; x < w; x += 8)
        {
            __builtin_prefetch(&pb[x + 16], 0, 0);
            line565[x] = CRAM565[pb[x] & 0x3F];
            line565[x + 1] = CRAM565[pb[x + 1] & 0x3F];
            line565[x + 2] = CRAM565[pb[x + 2] & 0x3F];
            line565[x + 3] = CRAM565[pb[x + 3] & 0x3F];
            line565[x + 4] = CRAM565[pb[x + 4] & 0x3F];
            line565[x + 5] = CRAM565[pb[x + 5] & 0x3F];
            line565[x + 6] = CRAM565[pb[x + 6] & 0x3F];
            line565[x + 7] = CRAM565[pb[x + 7] & 0x3F];
        }
        GWENESIS_PUSH_SCANLINE(line, line565, w);
        t_render_end = esp_timer_get_time();
        return;
    }

    // ── Shadow/Highlight Mode ─────────────────────────────────────────────────
    for (int x = 0; x < w; ++x)
    {
        const uint8_t plane = pb[x];
        const uint8_t sprite = ps[x];
        const uint8_t p_idx = plane & 0x3F;
        const uint8_t s_idx = sprite & 0x3F;
        uint16_t rgb;

        if ((plane & 0xC0) < (sprite & 0xC0))
        {
            if (s_idx == 0x3E)
                rgb = CRAM565_HI[p_idx];
            else if (s_idx == 0x3F)
                rgb = CRAM565_SH[p_idx];
            else if (s_idx != 0x00)
                rgb = CRAM565[s_idx];
            else
                rgb = CRAM565[p_idx];
        }
        else
        {
            rgb = CRAM565[p_idx];
        }
        line565[x] = rgb;
    }

    GWENESIS_PUSH_SCANLINE(line, line565, w);
    t_render_end = esp_timer_get_time(); // FIX: aggiornato anche nel path SHI
}