/*
 * audio_diag.h — Diagnostica audio SMS/GG per ESP32-S3
 *
 * COME USARE:
 *   1. #include "audio_diag.h" in cima a system.c
 *   2. Compila e avvia, apri Serial Monitor a 115200 baud
 *   3. Gioca per ~10 secondi con un gioco che ha musica semplice
 *   4. Copia l'output Serial e mandalo — la causa del gracchiante sarà evidente
 *   5. Dopo la diagnosi, rimuovi l'#include
 *
 * Il file NON modifica il comportamento audio — solo osserva e logga.
 * I printf sono throttlati (ogni 60 frame = ~1 sec) per non impattare le performance.
 */

#pragma once
#include <stdint.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────────────────────
// Contatori globali diagnostica
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t diag_frame_count   = 0;   // frame totali processati
static uint32_t diag_underrun_count = 0;  // volte che i2s_write ha scritto meno del previsto
static uint32_t diag_overwrite_count = 0; // volte che xQueueOverwrite ha sovrascritt un frame
static int32_t  diag_peak_L = 0;          // picco assoluto canale L (per rilevare clipping)
static int32_t  diag_peak_R = 0;          // picco assoluto canale R
static int32_t  diag_min_L  = 0;          // valore minimo (per rilevare DC offset)
static int32_t  diag_min_R  = 0;
static int32_t  diag_sum_L  = 0;          // somma per calcolare media (DC offset)
static int32_t  diag_sum_R  = 0;
static uint32_t diag_sample_count = 0;    // campioni totali analizzati
static uint32_t diag_clip_count   = 0;    // campioni che hanno toccato ±32767 (hard clip PSG)

// ─────────────────────────────────────────────────────────────────────────────
// MACRO 1 — Da chiamare in audio_prepare_frame(), DOPO SN76496Update2()
//           e PRIMA della normalizzazione >> 2
//
// Rivela: range grezzo del PSG, presenza di hard clip, DC offset
// ─────────────────────────────────────────────────────────────────────────────
#define DIAG_ANALYZE_PSG_RAW(buf0, buf1, n)                             \
do {                                                                     \
    for (int _i = 0; _i < (n); _i++) {                                  \
        int32_t _l = (int32_t)(buf0)[_i];                               \
        int32_t _r = (int32_t)(buf1)[_i];                               \
        if (_l >  diag_peak_L) diag_peak_L = _l;                        \
        if (_r >  diag_peak_R) diag_peak_R = _r;                        \
        if (_l <  diag_min_L)  diag_min_L  = _l;                        \
        if (_r <  diag_min_R)  diag_min_R  = _r;                        \
        diag_sum_L += _l;                                                \
        diag_sum_R += _r;                                                \
        /* hard clip = campione a ±32767 PRIMA della normalizzazione */  \
        if (_l >= 32767 || _l <= -32768 ||                               \
            _r >= 32767 || _r <= -32768)                                 \
            diag_clip_count++;                                           \
    }                                                                    \
    diag_sample_count += (n);                                            \
    diag_frame_count++;                                                  \
} while(0)

// ─────────────────────────────────────────────────────────────────────────────
// MACRO 2 — Da chiamare in audioOutput(), dopo i2s_write()
//           Rileva underrun I2S (bytes scritti < bytes richiesti)
// ─────────────────────────────────────────────────────────────────────────────
#define DIAG_CHECK_I2S_UNDERRUN(written, expected)                       \
do {                                                                     \
    if ((written) < (expected)) {                                        \
        diag_underrun_count++;                                           \
        printf("[DIAG] I2S UNDERRUN: scritti %u / %u bytes\n",          \
               (unsigned)(written), (unsigned)(expected));               \
    }                                                                    \
} while(0)

// ─────────────────────────────────────────────────────────────────────────────
// MACRO 3 — Report periodico: ogni 60 frame (~1 secondo)
//           Da chiamare alla fine di DIAG_ANALYZE_PSG_RAW
// ─────────────────────────────────────────────────────────────────────────────
#define DIAG_PRINT_REPORT()                                              \
do {                                                                     \
    if (diag_frame_count % 60 == 0 && diag_sample_count > 0) {          \
        int32_t _dc_l = diag_sum_L / (int32_t)diag_sample_count;        \
        int32_t _dc_r = diag_sum_R / (int32_t)diag_sample_count;        \
        float   _clip_pct = 100.0f * diag_clip_count / diag_sample_count; \
        printf("\n[DIAG] ── Report frame #%u ──────────────────\n",      \
               (unsigned)diag_frame_count);                              \
        printf("[DIAG] Peak L: %6d  Peak R: %6d  (max int16=32767)\n",  \
               (int)diag_peak_L, (int)diag_peak_R);                     \
        printf("[DIAG] Min  L: %6d  Min  R: %6d\n",                     \
               (int)diag_min_L, (int)diag_min_R);                       \
        printf("[DIAG] DC offset L: %+d  R: %+d  (ideale=0)\n",         \
               (int)_dc_l, (int)_dc_r);                                 \
        printf("[DIAG] Hard-clip PSG: %u campioni su %u (%.1f%%)\n",    \
               (unsigned)diag_clip_count,                                \
               (unsigned)diag_sample_count, _clip_pct);                 \
        printf("[DIAG] I2S underrun totali: %u\n",                      \
               (unsigned)diag_underrun_count);                           \
        printf("[DIAG] bufsize=%d  snd.enabled=%d\n",                   \
               snd.bufsize, snd.enabled);                                \
        printf("[DIAG] ─────────────────────────────────────────\n\n"); \
        /* Reset accumulatori (ma non frame_count e underrun) */         \
        diag_peak_L = diag_peak_R = 0;                                  \
        diag_min_L  = diag_min_R  = 0;                                  \
        diag_sum_L  = diag_sum_R  = 0;                                  \
        diag_sample_count = 0;                                           \
        diag_clip_count   = 0;                                           \
    }                                                                    \
} while(0)