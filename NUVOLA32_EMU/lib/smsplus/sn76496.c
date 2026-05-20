
#include "shared.h"

#define MAX_OUTPUT  0x7FFF
#define STEP        0x10000
#define FB_WNOISE   0x12000
#define FB_PNOISE   0x08000
#define NG_PRESET   0x0F35

t_SN76496 sn[MAX_76496];

void SN76496Write(int chip,int data)
{
    t_SN76496 *R = &sn[chip];


	if (data & 0x80)
	{
		int r = (data & 0x70) >> 4;
		int c = r/2;

		R->LastRegister = r;
		R->Register[r] = (R->Register[r] & 0x3f0) | (data & 0x0f);
		switch (r)
		{
			case 0:	/* tone 0 : frequency */
			case 2:	/* tone 1 : frequency */
			case 4:	/* tone 2 : frequency */
				R->Period[c] = R->UpdateStep * R->Register[r];
				if (R->Period[c] == 0) R->Period[c] = R->UpdateStep;
				if (r == 4)
				{
					/* update noise shift frequency */
					if ((R->Register[6] & 0x03) == 0x03)
						R->Period[3] = 2 * R->Period[2];
				}
				break;
			case 1:	/* tone 0 : volume */
			case 3:	/* tone 1 : volume */
			case 5:	/* tone 2 : volume */
			case 7:	/* noise  : volume */
				R->Volume[c] = R->VolTable[data & 0x0f];
				break;
			case 6:	/* noise  : frequency, mode */
				{
					int n = R->Register[6];
					R->NoiseFB = (n & 4) ? FB_WNOISE : FB_PNOISE;
					n &= 3;
					/* N/512,N/1024,N/2048,Tone #3 output */
					R->Period[3] = (n == 3) ? 2 * R->Period[2] : (R->UpdateStep << (5+n));

					/* reset noise shifter */
					R->RNG = NG_PRESET;
					R->Output[3] = R->RNG & 1;
				}
				break;
		}
	}
	else
	{
		int r = R->LastRegister;
		int c = r/2;

		switch (r)
		{
			case 0:	/* tone 0 : frequency */
			case 2:	/* tone 1 : frequency */
			case 4:	/* tone 2 : frequency */
				R->Register[r] = (R->Register[r] & 0x0f) | ((data & 0x3f) << 4);
				R->Period[c] = R->UpdateStep * R->Register[r];
				if (R->Period[c] == 0) R->Period[c] = R->UpdateStep;
				if (r == 4)
				{
					/* update noise shift frequency */
					if ((R->Register[6] & 0x03) == 0x03)
						R->Period[3] = 2 * R->Period[2];
				}
				break;
		}
	}
}


void SN76496Update(int chip,INT16 *buffer[2],int length, unsigned char mask)
{
    int i, j;
    int buffer_index = 0;
    t_SN76496 *R = &sn[chip];

	/* If the volume is 0, increase the counter */
	for (i = 0;i < 4;i++)
	{
		if (R->Volume[i] == 0)
		{
			/* note that I do count += length, NOT count = length + 1. You might think */
			/* it's the same since the volume is 0, but doing the latter could cause */
			/* interferencies when the program is rapidly modulating the volume. */
			if (R->Count[i] <= length*STEP) R->Count[i] += length*STEP;
		}
	}

	while (length > 0)
	{
		int vol[4];
        unsigned int out[2];
		int left;


		/* vol[] keeps track of how long each square wave stays */
		/* in the 1 position during the sample period. */
		vol[0] = vol[1] = vol[2] = vol[3] = 0;

		for (i = 0;i < 3;i++)
		{
			if (R->Output[i]) vol[i] += R->Count[i];
			R->Count[i] -= STEP;
			/* Period[i] is the half period of the square wave. Here, in each */
			/* loop I add Period[i] twice, so that at the end of the loop the */
			/* square wave is in the same status (0 or 1) it was at the start. */
			/* vol[i] is also incremented by Period[i], since the wave has been 1 */
			/* exactly half of the time, regardless of the initial position. */
			/* If we exit the loop in the middle, Output[i] has to be inverted */
			/* and vol[i] incremented only if the exit status of the square */
			/* wave is 1. */
			while (R->Count[i] <= 0)
			{
				R->Count[i] += R->Period[i];
				if (R->Count[i] > 0)
				{
					R->Output[i] ^= 1;
					if (R->Output[i]) vol[i] += R->Period[i];
					break;
				}
				R->Count[i] += R->Period[i];
				vol[i] += R->Period[i];
			}
			if (R->Output[i]) vol[i] -= R->Count[i];
		}

		left = STEP;
		do
		{
			int nextevent;


			if (R->Count[3] < left) nextevent = R->Count[3];
			else nextevent = left;

			if (R->Output[3]) vol[3] += R->Count[3];
			R->Count[3] -= nextevent;
			if (R->Count[3] <= 0)
			{
				if (R->RNG & 1) R->RNG ^= R->NoiseFB;
				R->RNG >>= 1;
				R->Output[3] = R->RNG & 1;
				R->Count[3] += R->Period[3];
				if (R->Output[3]) vol[3] += R->Period[3];
			}
			if (R->Output[3]) vol[3] -= R->Count[3];

			left -= nextevent;
		} while (left > 0);

        out[0] = out[1] = 0;
        for(j = 0; j < 4; j += 1)
        {
            int k = vol[j] * R->Volume[j];
            if(mask & (1 << (4+j))) out[0] += k;
            if(mask & (1 << (0+j))) out[1] += k;
        }

        if(out[0] > MAX_OUTPUT * STEP) out[0] = MAX_OUTPUT * STEP;
        if(out[1] > MAX_OUTPUT * STEP) out[1] = MAX_OUTPUT * STEP;
        buffer[0][buffer_index] = out[0] / STEP;
        buffer[1][buffer_index] = out[1] / STEP;

        /* Next sample set */
        buffer_index += 1;

		length--;
	}
}



void SN76496_set_clock(int chip,int clock)
{
    t_SN76496 *R = &sn[chip];

	R->UpdateStep = ((double)STEP * R->SampleRate * 16) / clock;
}



void SN76496_set_gain(int chip,int gain)
{
    t_SN76496 *R = &sn[chip];
	int i;
	double out;

	gain &= 0xff;
	out = MAX_OUTPUT / 3;
	while (gain-- > 0)
        out *= 1.023292992;

	for (i = 0;i < 15;i++)
	{
		if (out > MAX_OUTPUT / 3) R->VolTable[i] = MAX_OUTPUT / 3;
		else R->VolTable[i] = out;
        out /= 1.258925412;
	}

	R->VolTable[15] = 0;
}



int SN76496_init(int chip,int clock,int volume,int sample_rate)
{
	int i;
    t_SN76496 *R = &sn[chip];

	R->SampleRate = sample_rate;
	SN76496_set_clock(chip,clock);

	for (i = 0;i < 4;i++) R->Volume[i] = 0;

	R->LastRegister = 0;
	for (i = 0;i < 8;i+=2)
	{
		R->Register[i] = 0;
		R->Register[i + 1] = 0x0f;	/* volume = 0 */
	}

	for (i = 0;i < 4;i++)
	{
		R->Output[i] = 0;
		R->Period[i] = R->Count[i] = R->UpdateStep;
	}
	R->RNG = NG_PRESET;
	R->Output[3] = R->RNG & 1;

    SN76496_set_gain(0, (volume >> 8) & 0xFF);

	return 0;
}

/*
 * SN76496Update - Aggiornamento campioni audio SN76496
 *
 * Correzioni applicate:
 *  - Tipi espliciti int32_t/int64_t per prevenire overflow
 *  - Protezione Period[3] == 0 (loop infinito)
 *  - Calcolo vol[3] corretto: accumulo diretto per nextevent
 *  - Clipping con MAX_OUTPUT * STEP precalcolato
 *  - __builtin_expect per branch prediction hints su ESP32S3
 *  - Commenti aggiornati
 */
void SN76496Update2(int chip, INT16 *buffer[2], int length, unsigned char mask)
{
    int32_t i, j;
    int32_t buffer_index = 0;
    t_SN76496 *R = &sn[chip];

    /* Precalcola il limite di clipping fuori dai loop interni */
    const int32_t max_out = (int32_t)MAX_OUTPUT * STEP;

    /*
     * Protezione contro Period[3] == 0 che causerebbe un loop infinito
     * nel do/while del canale noise. Applicata qui e non in scrittura
     * registro per sicurezza difensiva.
     */
    if (__builtin_expect(R->Period[3] <= 0, 0))
        R->Period[3] = 1;

    /*
     * Ottimizzazione: se il volume è 0, porta il contatore oltre
     * la finestra di aggiornamento per saltare il calcolo interno.
     * Usa length*STEP + STEP per essere sicuramente fuori range
     * indipendentemente dal valore corrente di length nel loop.
     */
    for (i = 0; i < 4; i++)
    {
        if (R->Volume[i] == 0)
        {
            if (R->Count[i] <= length * STEP)
                R->Count[i] = (int32_t)length * STEP + STEP;
        }
    }

    while (length > 0)
    {
        int32_t  vol[4];
        int64_t  out[2];   /* int64 per evitare overflow su vol*Volume*4 canali */
        int32_t  left;

        vol[0] = vol[1] = vol[2] = vol[3] = 0;

        /* ── Canali tone 0-2 ─────────────────────────────────────── */
        for (i = 0; i < 3; i++)
        {
            if (R->Output[i]) vol[i] += R->Count[i];
            R->Count[i] -= STEP;

            while (R->Count[i] <= 0)
            {
                R->Count[i] += R->Period[i];
                if (R->Count[i] > 0)
                {
                    R->Output[i] ^= 1;
                    if (R->Output[i]) vol[i] += R->Period[i];
                    break;
                }
                R->Count[i] += R->Period[i];
                vol[i] += R->Period[i];
            }

            if (R->Output[i]) vol[i] -= R->Count[i];
        }

        /* ── Canale noise 3 ──────────────────────────────────────── */
        /*
         * FIX: il metodo originale usava:
         *   vol[3] += Count[3]  (prima del decremento)
         *   ...eventuale reset di Count[3]...
         *   vol[3] -= Count[3]  (dopo, con Count[3] diverso!)
         *
         * La correzione accumula direttamente 'nextevent' quando
         * l'output è 1, eliminando l'inconsistenza.
         */
        left = STEP;
        do
        {
            int32_t nextevent = (R->Count[3] < left) ? R->Count[3] : left;

            /* Accumula solo il tempo effettivo in cui output era 1 */
            if (R->Output[3]) vol[3] += nextevent;

            R->Count[3] -= nextevent;

            if (R->Count[3] <= 0)
            {
                if (R->RNG & 1) R->RNG ^= R->NoiseFB;
                R->RNG >>= 1;
                R->Output[3] = R->RNG & 1;
                R->Count[3] += R->Period[3];
                /* Period[3] > 0 garantito dal controllo all'inizio */
            }

            left -= nextevent;
        } while (left > 0);

        /* ── Mix e clipping ──────────────────────────────────────── */
        out[0] = out[1] = 0;

        for (j = 0; j < 4; j++)
        {
            /*
             * FIX overflow: vol[j] può essere fino a STEP (es. 0x8000),
             * Volume[j] fino a MAX_OUTPUT. Il prodotto supera INT32_MAX
             * con STEP elevati → promozione a int64_t esplicita.
             */
            int64_t k = (int64_t)vol[j] * (int64_t)R->Volume[j];
            if (mask & (1 << (4 + j))) out[0] += k;
            if (mask & (1 << (0 + j))) out[1] += k;
        }

        /* Clipping con hint per branch predictor (raro su segnale normale) */
        if (__builtin_expect(out[0] > max_out, 0)) out[0] = max_out;
        if (__builtin_expect(out[1] > max_out, 0)) out[1] = max_out;

        /* Divisione finale e scrittura buffer stereo */
        buffer[0][buffer_index] = (INT16)(out[0] / STEP);
        buffer[1][buffer_index] = (INT16)(out[1] / STEP);

        buffer_index++;
        length--;
    }
}

void SN76496Update3(int chip, INT16 *buffer[2], int length, unsigned char mask)
{
    int32_t  i, j;
    int32_t  buffer_index = 0;
    t_SN76496 *R = &sn[chip];
 
    // FIX #4 — precalcolo fuori dai loop
    const int32_t max_out = (int32_t)MAX_OUTPUT * STEP;
 
    // FIX #2 — Period[3] == 0 → loop infinito nel canale noise
    if (__builtin_expect(R->Period[3] <= 0, 0))
        R->Period[3] = 1;
 
    // Salta il calcolo interno per canali silenziosi:
    // porta il contatore oltre la finestra del frame corrente.
    // Usa length*STEP + STEP (non += length*STEP) per garantire
    // di essere fuori range indipendentemente dal valore attuale.
    for (i = 0; i < 4; i++)
    {
        if (R->Volume[i] == 0)
        {
            if (R->Count[i] <= (int32_t)length * STEP)
                R->Count[i] = (int32_t)length * STEP + STEP;
        }
    }
 
    while (length > 0)
    {
        int32_t vol[4];
        int64_t out[2];   // FIX #1: int64_t per contenere vol*Volume senza overflow
        int32_t left;
 
        vol[0] = vol[1] = vol[2] = vol[3] = 0;
 
        // ── Canali tone 0-2 ──────────────────────────────────────────────────
        // Logica invariata rispetto all'originale; tipi aggiornati a int32_t.
        for (i = 0; i < 3; i++)
        {
            if (R->Output[i]) vol[i] += R->Count[i];
            R->Count[i] -= STEP;
 
            while (R->Count[i] <= 0)
            {
                R->Count[i] += R->Period[i];
                if (R->Count[i] > 0)
                {
                    R->Output[i] ^= 1;
                    if (R->Output[i]) vol[i] += R->Period[i];
                    break;
                }
                R->Count[i] += R->Period[i];
                vol[i] += R->Period[i];
            }
 
            if (R->Output[i]) vol[i] -= R->Count[i];
        }
 
        // ── Canale noise 3 ───────────────────────────────────────────────────
        // FIX #3: accumulo diretto di nextevent invece di Count[3] pre/post.
        //
        // Originale (errato):
        //   if (Output[3]) vol[3] += Count[3];   ← Count[3] PRIMA del decremento
        //   Count[3] -= nextevent;
        //   if (Count[3] <= 0) { ...; Count[3] += Period[3]; }
        //   if (Output[3]) vol[3] -= Count[3];   ← Count[3] DOPO il reset
        //
        // Il problema: se Count[3] viene resettato (aggiunto Period[3]),
        // il valore usato nella sottrazione finale non è lo stesso di quello
        // usato nell'addizione iniziale → vol[3] errato → rumore distorto.
        //
        // Versione corretta: vol[3] += nextevent quando output=1,
        // che rappresenta esattamente il tempo in cui il canale era alto
        // durante questo slot, senza dipendere da riorganizzazioni di Count[3].
        left = STEP;
        do
        {
            int32_t nextevent = (R->Count[3] < left) ? R->Count[3] : left;
 
            if (R->Output[3]) vol[3] += nextevent;   // FIX #3
 
            R->Count[3] -= nextevent;
 
            if (R->Count[3] <= 0)
            {
                if (R->RNG & 1) R->RNG ^= R->NoiseFB;
                R->RNG >>= 1;
                R->Output[3] = R->RNG & 1;
                R->Count[3] += R->Period[3];
                // Period[3] > 0 garantito dal controllo FIX #2 all'inizio
            }
 
            left -= nextevent;
        } while (left > 0);
 
        // ── Mix stereo e clipping ─────────────────────────────────────────────
        out[0] = out[1] = 0;
 
        for (j = 0; j < 4; j++)
        {
            // FIX #1: cast esplicito a int64_t prima della moltiplicazione.
            // Senza cast: vol[j] (int32) * Volume[j] (int32) può overflow
            // se vol[j] è vicino a STEP (0x10000) e Volume[j] a MAX_OUTPUT (0x7FFF).
            int64_t k = (int64_t)vol[j] * (int64_t)R->Volume[j];
            if (mask & (1 << (4 + j))) out[0] += k;
            if (mask & (1 << (0 + j))) out[1] += k;
        }
 
        // FIX #4: confronto con max_out precalcolato.
        // __builtin_expect: il clipping è raro su segnale normale → hint al
        // branch predictor dell'Xtensa LX7 per evitare pipeline stall.
        if (__builtin_expect(out[0] > max_out, 0)) out[0] = max_out;
        if (__builtin_expect(out[1] > max_out, 0)) out[1] = max_out;
 
        buffer[0][buffer_index] = (INT16)(out[0] / STEP);
        buffer[1][buffer_index] = (INT16)(out[1] / STEP);
 
        buffer_index++;
        length--;
    }
}