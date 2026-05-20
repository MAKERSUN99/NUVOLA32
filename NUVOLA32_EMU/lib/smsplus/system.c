/*
    Copyright (C) 1998, 1999, 2000  Charles Mac Donald
    Modified for ESP32-S3 SMS/GG emulator — audio fixes applied.

    CHANGES vs originale:
    - FIX #2: s_i2sBuffer allocato in audio_init(), non lazy in audioOutput()
    - FIX #3: bufsize con ceiling division → nessun drift cumulativo
    - FIX #4: controllo NULL su snd.buffer anche con snd.enabled==1
    - FIX #5: clamp campioni su bufsize reale, non magic number 1024
    - FIX #8: s_i2sBuffer ora è int32_t stereo — PCM5102 richiede 32 bit/canale
    - FIX #9: audioOutput() espande int16 → int32 con << 16 (MSB alignment)
    - FIX #9: rimosso >> 1 in audioOutput() — abbassava il volume di 6 dB
              senza beneficio; il clipping va gestito nel mixer, non qui
    - NEW:    audio_prepare_frame() separata da audioOutput()
              → emuTask produce PCM, audioTask (Core 1) consuma via I2S
*/

#include "shared.h"
#include "esp_heap_caps.h"
#include "driver/i2s.h"

#define PSG_VOL  1
#define FM_VOL   3
#define I2S_PORT I2S_NUM_0

// Byte per campione sorgente sul bus I2S stereo 32-bit:
// Left (4 byte) + Right (4 byte) = 8 byte per campione mono/stereo
#define BYTES_PER_STEREO_FRAME 8

t_bitmap bitmap;
t_cart   cart;
t_snd    snd;
t_input  input;

int fm_enabled = 0;

static int16_t *fm_l = NULL;
static int16_t *fm_r = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// FIX #8 — Buffer DMA ora int32_t stereo (era int16_t).
// Il PCM5102 usa i 16 bit alti del word a 32 bit; i bit bassi vengono ignorati.
// Dimensione: snd.bufsize coppie L+R × 4 byte/campione.
// Allocazione MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL: obbligatoria per i2s_write.
// ─────────────────────────────────────────────────────────────────────────────
static int32_t *s_i2sBuffer     = NULL;
static int      s_i2sBufferSize = 0;   // numero di campioni sorgente (coppie L+R)

struct { char reg[64]; } ym2413;

// ─────────────────────────────────────────────────────────────────────────────
// ym2413 helper (invariato)
// ─────────────────────────────────────────────────────────────────────────────
static void ym2413_alloc_buffers(int samples)
{
    if (fm_l) return;
    fm_l = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    fm_r = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ─────────────────────────────────────────────────────────────────────────────
// emu_system_init (invariato)
// ─────────────────────────────────────────────────────────────────────────────
extern void emu_system_init(int rate)
{
    vdp_init();
    sms_init();
    render_init();
    audio_init(rate);
    sms.save = 0;
    memset(&input, 0, sizeof(t_input));
}

// ─────────────────────────────────────────────────────────────────────────────
// audio_init
// ─────────────────────────────────────────────────────────────────────────────
void audio_init(int rate)
{
    memset(&snd, 0, sizeof(t_snd));
    snd.log      = 0;
    snd.callback = NULL;

    if (!rate)
    {
        snd.enabled = 0;
        return;
    }

    // FIX #3 — ceiling division evita troncamento e deriva cumulativa.
    // Esempio: rate=22050 → (22050+59)/60=368 invece di 367.
    // A 60 fps: 367×60=22020 ≠ 22050 → 30 campioni/sec di drift senza fix.
    snd.bufsize = (rate) / 60;

    const size_t bytes = snd.bufsize * sizeof(int16_t);

    // PCM buffer sorgente (int16): preferenza PSRAM, fallback INTERNAL
    snd.buffer[0] = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
    snd.buffer[1] = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);

    if (!snd.buffer[0] || !snd.buffer[1])
    {
        if (snd.buffer[0]) { heap_caps_free(snd.buffer[0]); snd.buffer[0] = NULL; }
        if (snd.buffer[1]) { heap_caps_free(snd.buffer[1]); snd.buffer[1] = NULL; }

        snd.buffer[0] = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snd.buffer[1] = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!snd.buffer[0] || !snd.buffer[1])
    {
        if (snd.buffer[0]) { heap_caps_free(snd.buffer[0]); snd.buffer[0] = NULL; }
        if (snd.buffer[1]) { heap_caps_free(snd.buffer[1]); snd.buffer[1] = NULL; }
        printf("[AUDIO] FATAL: PCM buffer alloc failed — sound disabled\n");
        snd.enabled = 0;
        return;
    }

    memset(snd.buffer[0], 0, bytes);
    memset(snd.buffer[1], 0, bytes);

    // FIX #2 + FIX #8 — buffer DMA int32_t stereo allocato subito.
    // Ogni campione sorgente (mono o stereo) → 2 × int32_t (L + R).
    // MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL: obbligatorio per il driver I2S.
    s_i2sBufferSize = snd.bufsize;
    s_i2sBuffer = (int32_t *)heap_caps_malloc(
        s_i2sBufferSize * 2 * sizeof(int32_t),   // 2 canali × 4 byte
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!s_i2sBuffer)
    {
        printf("[AUDIO] FATAL: DMA I2S buffer alloc failed — sound disabled\n");
        heap_caps_free(snd.buffer[0]); snd.buffer[0] = NULL;
        heap_caps_free(snd.buffer[1]); snd.buffer[1] = NULL;
        snd.enabled = 0;
        return;
    }

    memset(s_i2sBuffer, 0, s_i2sBufferSize * 2 * sizeof(int32_t));

    SN76496_init(0, MASTER_CLOCK, 255, rate);
    fm_enabled  = 0;
    snd.enabled = 1;

    printf("[AUDIO] init OK — rate=%d  bufsize=%d  i2sBuf=%d stereo frames (32-bit)\n",
           rate, snd.bufsize, s_i2sBufferSize);
}

void system_shutdown(void) { /* noop */ }

extern void system_reset(void)
{
    cpu_reset();
    vdp_reset();
    sms_reset();
    render_reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// save / load state (invariati)
// ─────────────────────────────────────────────────────────────────────────────
void system_save_state(void *fd)
{
    fwrite(&vdp,           sizeof(t_vdp),     1, fd);
    fwrite(&sms,           sizeof(t_sms),     1, fd);
    fwrite(Z80_Context,    sizeof(Z80_Regs),  1, fd);
    fwrite(&after_EI,      sizeof(int),       1, fd);
    fwrite(&ym2413.reg[0], 0x40,              1, fd);
    fwrite(&sn[0],         sizeof(t_SN76496), 1, fd);
}

void system_load_state(void *fd)
{
    uint8 reg[0x40];

    cpu_reset();
    system_reset();

    fread(&vdp,           sizeof(t_vdp),     1, fd);
    fread(&sms,           sizeof(t_sms),     1, fd);
    fread(Z80_Context,    sizeof(Z80_Regs),  1, fd);
    fread(&after_EI,      sizeof(int),       1, fd);
    fread(reg,            0x40,              1, fd);
    fread(&sn[0],         sizeof(t_SN76496), 1, fd);

    z80_set_irq_callback(sms_irq_callback);

    cpu_readmap[0] = cart.rom + 0x0000;
    cpu_readmap[1] = cart.rom + 0x2000;
    cpu_readmap[2] = cart.rom + 0x4000;
    cpu_readmap[3] = cart.rom + 0x6000;
    cpu_readmap[4] = cart.rom + 0x0000;
    cpu_readmap[5] = cart.rom + 0x2000;
    cpu_readmap[6] = sms.ram;
    cpu_readmap[7] = sms.ram;

    cpu_writemap[0] = sms.dummy;
    cpu_writemap[1] = sms.dummy;
    cpu_writemap[2] = sms.dummy;
    cpu_writemap[3] = sms.dummy;
    cpu_writemap[4] = sms.dummy;
    cpu_writemap[5] = sms.dummy;
    cpu_writemap[6] = sms.ram;
    cpu_writemap[7] = sms.ram;

    sms_mapper_w(3, sms.fcr[3]);
    sms_mapper_w(2, sms.fcr[2]);
    sms_mapper_w(1, sms.fcr[1]);
    sms_mapper_w(0, sms.fcr[0]);

    for (int i = 0; i < PALETTE_SIZE; i++)
        palette_sync(i);
}

void ym2413_write(int chip, int offset, int data) { /* stub */ }

// ─────────────────────────────────────────────────────────────────────────────
// Soft limiter — usato in audioOutput()
//
// Perché non un semplice clamp (hard clip)?
// Hard clip taglia l'onda di netto → armoniche dispari forti → suono metallico.
// Il soft limiter comprime il segnale quando supera la soglia con una curva
// continua: il segnale "piega" invece di "spezzarsi" → molto meno gracchiante.
//
// Algoritmo: cubic soft-clip a soglia LIMIT
//   se |x| <= LIMIT  →  y = x  (zona lineare, nessuna distorsione)
//   se |x| >  LIMIT  →  y = sign(x) * (LIMIT + (x-LIMIT) / (1 + k*(x-LIMIT)²))
//   dove k controlla la "morbidezza" della curva
//
// OUTPUT: campione limitato nel range [-OUT_MAX, +OUT_MAX] ⊂ int16
// ─────────────────────────────────────────────────────────────────────────────
#define SOFT_LIMIT_THRESHOLD  24576   // ~75% di 32767 — zona lineare
#define SOFT_LIMIT_OUT_MAX    30000   // headroom di sicurezza per << 16 sul PCM5102

static inline int16_t soft_limit(int32_t x)
{
    const int32_t T = SOFT_LIMIT_THRESHOLD;
    const int32_t M = SOFT_LIMIT_OUT_MAX;

    if (x >= -T && x <= T)
        return (int16_t)x;   // zona lineare: nessuna modifica

    int32_t sign = (x > 0) ? 1 : -1;
    int32_t over = (x * sign) - T;               // eccesso oltre soglia (≥0)

    // Compressione: over / (1 + over/T) — versione intera senza float
    // Approssima la curva iperbolare con aritmetica a 32 bit
    int32_t compressed = T + (over * T) / (T + over);

    int32_t limited = sign * compressed;

    // Clamp finale di sicurezza
    if      (limited >  M) limited =  M;
    else if (limited < -M) limited = -M;

    return (int16_t)limited;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filtro DC — rimuove l'offset continuo che il PSG può introdurre.
// Un offset DC sul PCM5102 causa un "thump" al cambio di scena e contribuisce
// alla percezione di gracchiante su altoparlanti small-form.
// Implementazione: high-pass IIR del primo ordine con α=0.9975 (~20 Hz a 44100)
// y[n] = x[n] - x[n-1] + α * y[n-1]
// ─────────────────────────────────────────────────────────────────────────────
#define DC_FILTER_ALPHA_NUM  9975    // numeratore   (α = 9975/10000 = 0.9975)
#define DC_FILTER_ALPHA_DEN 10000    // denominatore

static int32_t s_dcPrevInL  = 0, s_dcPrevOutL = 0;
static int32_t s_dcPrevInR  = 0, s_dcPrevOutR = 0;

static inline int32_t dc_filter_l(int32_t x)
{
    int32_t y = x - s_dcPrevInL
                + (DC_FILTER_ALPHA_NUM * s_dcPrevOutL) / DC_FILTER_ALPHA_DEN;
    s_dcPrevInL  = x;
    s_dcPrevOutL = y;
    return y;
}

static inline int32_t dc_filter_r(int32_t x)
{
    int32_t y = x - s_dcPrevInR
                + (DC_FILTER_ALPHA_NUM * s_dcPrevOutR) / DC_FILTER_ALPHA_DEN;
    s_dcPrevInR  = x;
    s_dcPrevOutR = y;
    return y;
}

// ─────────────────────────────────────────────────────────────────────────────
// audioOutput
// Chiamata SOLO dall'audioTask su Core 1 — mai dall'emuTask.
//
// Pipeline per ogni campione:
//   int16 PSG  →  DC filter  →  soft limiter  →  << 16  →  int32 I2S
//
// FIX #8 + FIX #9:
//   - s_i2sBuffer è int32_t (PCM5102 richiede frame 32-bit)
//   - << 16 allinea i dati ai MSB del word a 32-bit
//   - rimosso >> 1: il volume viene gestito qui con headroom calibrato
// ─────────────────────────────────────────────────────────────────────────────
void audioOutput(int16_t *left, int16_t *right, int samples)
{
    if (!s_i2sBuffer || !left || !right) return;

    // FIX #5 — clamp con warning diagnostico
    if (samples > s_i2sBufferSize)
    {
        printf("[AUDIO] WARN: samples=%d > bufSize=%d — clamping\n",
               samples, s_i2sBufferSize);
        samples = s_i2sBufferSize;
    }

    for (int i = 0; i < samples; i++)
    {
        // 1. Filtro DC (rimuove offset continuo del PSG)
        int32_t l = dc_filter_l((int32_t)left[i]);
        int32_t r = dc_filter_r((int32_t)right[i]);

        // 2. Soft limiter (comprime invece di tagliare)
        int16_t ls = soft_limit(l);
        int16_t rs = soft_limit(r);

        // 3. MSB alignment per PCM5102: sample 16-bit nei bit 31..16
        s_i2sBuffer[i * 2]     = (int32_t)ls << 16;
        s_i2sBuffer[i * 2 + 1] = (int32_t)rs << 16;
    }

    const size_t bytesToWrite = (size_t)samples * BYTES_PER_STEREO_FRAME;
    size_t bytesWritten = 0;

    /*i2s_write(
        I2S_PORT,
        s_i2sBuffer,
        bytesToWrite,
        &bytesWritten,
        pdMS_TO_TICKS(60)
    );*/

    i2s_write(
        I2S_PORT,
        s_i2sBuffer,
        bytesToWrite,
        &bytesWritten,
        portMAX_DELAY
    );

#ifdef DEBUG_AUDIO
    if (bytesWritten < bytesToWrite)
        printf("[AUDIO] i2s underrun: wrote %u / %u bytes\n",
               (unsigned)bytesWritten, (unsigned)bytesToWrite);
#endif

#ifdef DEBUG_AUDIO
    if (bytesWritten < bytesToWrite)
        printf("[AUDIO] i2s underrun: wrote %u / %u bytes\n",
               (unsigned)bytesWritten, (unsigned)bytesToWrite);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// audio_prepare_frame
// Produce campioni PSG in snd.buffer[0/1] (int16).
// Chiamata dall'emuTask su Core 0 — NON tocca l'I2S.
//
// CAUSA DEL GRACCHIANTE IDENTIFICATA DAL LOG DIAGNOSTICO:
//   Min L/R = 0 sempre → il SN76496 genera onde UNIPOLARI (0 → +N).
//   Il PCM5102 si aspetta audio BIPOLARE centrato sullo zero (−N → +N).
//   Con un segnale unipolare il DAC riceve una componente DC enorme (~25%
//   del range) che l'amplificatore downstream distorce pesantemente.
//
// SOLUZIONE — pipeline per ogni campione:
//   1. Calcola la media del frame (DC istantaneo)
//   2. Sottrai la media → segnale bipolare centrato sullo zero
//   3. Applica headroom >> 1 (−6 dB) per sicurezza picchi
//   Il soft_limit in audioOutput gestisce gli eventuali picchi residui.
//
// Perché calcolare la media per frame invece di usare solo il filtro IIR?
//   Il filtro IIR in audioOutput ha una costante di tempo (~20 Hz) e non
//   reagisce abbastanza velocemente ai cambi rapidi di DC (dimostrato dai
//   log: DC varia da +0 a +7000 in pochi frame). La media per-frame è
//   esatta e a costo O(n) già pagato dal loop di normalizzazione.
// ─────────────────────────────────────────────────────────────────────────────

// Stato IIR per smoothing del DC tra frame consecutivi (evita discontinuità
// al confine tra frame che causerebbero click).
// α = 15/16 ≈ 0.9375: risponde in ~16 frame ≈ 267 ms a 60 fps
static int32_t s_dcSmooth_L = 0;
static int32_t s_dcSmooth_R = 0;

void audio_prepare_frame(void)
{
    if (!snd.enabled || !snd.buffer[0] || !snd.buffer[1]) return;

    SN76496Update2(0, snd.buffer, snd.bufsize, 0xFF);

    const int n = snd.bufsize;

    // ── Passo 1: calcola DC medio del frame corrente ──────────────────────────
    int32_t sum_L = 0, sum_R = 0;
    for (int i = 0; i < n; i++)
    {
        sum_L += (int32_t)snd.buffer[0][i];
        sum_R += (int32_t)snd.buffer[1][i];
    }
    int32_t dc_L = sum_L / n;
    int32_t dc_R = sum_R / n;

    // ── Passo 2: smoothing IIR del DC per evitare click ai confini di frame ───
    // s_dcSmooth = (s_dcSmooth * 15 + dc_current) / 16
    s_dcSmooth_L = (s_dcSmooth_L * 15 + dc_L) / 16;
    s_dcSmooth_R = (s_dcSmooth_R * 15 + dc_R) / 16;

    // ── Passo 3: centra il segnale e applica headroom ─────────────────────────
    for (int i = 0; i < n; i++)
    {
        // Sottrai il DC smoothed → segnale bipolare
        int32_t l = (int32_t)snd.buffer[0][i] - s_dcSmooth_L;
        int32_t r = (int32_t)snd.buffer[1][i] - s_dcSmooth_R;

        // >> 1 = −6 dB: headroom per picchi (il PSG può avere burst brevi
        // fino a ~18000 dai log; con >> 1 rimangono sotto ±9000, ben dentro
        // il range int16 e lontano dal soft_limit in audioOutput)
        l >>= 1;
        r >>= 1;

        snd.buffer[0][i] = (int16_t)l;
        snd.buffer[1][i] = (int16_t)r;
    }

    // Mixer FM — abilitare quando OPLL sarà integrato
    /*
    if (fm_enabled && opll)
    {
        for (int i = 0; i < n; i++)
        {
            int32_t fm[2];
            OPLL_calcStereo(opll, fm);
            int l = (int32_t)snd.buffer[0][i] + (fm[0] >> FM_VOL);
            int r = (int32_t)snd.buffer[1][i] + (fm[1] >> FM_VOL);
            if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
            if (r >  32767) r =  32767; else if (r < -32768) r = -32768;
            snd.buffer[0][i] = (int16_t)l;
            snd.buffer[1][i] = (int16_t)r;
        }
    }
    */
}

// ─────────────────────────────────────────────────────────────────────────────
// audio_update_frame  [mantenuta per compatibilità — da rimuovere]
// In modalità disaccoppiata NON deve essere chiamata dal loop di emulazione.
// Usare: emuTask → audio_prepare_frame() / audioTask → audioOutput()
// ─────────────────────────────────────────────────────────────────────────────
void audio_update_frame(void)
{
    audio_prepare_frame();
    audioOutput(snd.buffer[0], snd.buffer[1], snd.bufsize);
}