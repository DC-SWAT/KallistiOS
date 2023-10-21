/* KallistiOS ##version##

   snd_stream.c
   Copyright (C) 2000, 2001, 2002, 2003, 2004 Megan Potter
   Copyright (C) 2002 Florian Schulze
   Copyright (C) 2020 Lawrence Sebald
   Copyright (C) 2023 Ruslan Rostovtsev

   SH-4 support routines for SPU streaming sound driver
*/

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <sys/queue.h>

#include <arch/cache.h>
#include <arch/timer.h>
#include <dc/g2bus.h>
#include <dc/sq.h>
#include <dc/spu.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/pcm_split.h>

#include "arm/aica_cmd_iface.h"

/*

This module uses a nice circularly queued data stream in SPU RAM, which is
looped by a program running in the SPU itself.

Basically the poll routine checks to see if a certain minimum amount of
data is available to the SPU to be played, and if not, we ask the user
routine for more sound data and load it up. That's about it.

This version is capable of playing back N streams at once, with the limit
being available CPU time and channels.

*/

typedef struct filter {
    TAILQ_ENTRY(filter) lent;
    snd_stream_filter_t func;
    void            * data;
} filter_t;

/* Each of these represents an active streaming channel */
typedef struct strchan {
    // Which AICA channels are we using?
    int ch[2];

    // The last write position in the playing buffer
    uint32 last_write_pos; // = 0
    int curbuffer;  // = 0

    // The buffer size allocated for this stream.
    uint32 buffer_size;    // = 0x10000

    // Stream data location in AICA RAM
    uint32  spu_ram_sch[2];

    // "Get data" callback; we'll call this any time we want to get
    // another buffer of output data.
    snd_stream_callback_t get_data;

    // Our list of filter callback functions for this stream
    TAILQ_HEAD(filterlist, filter) filters;

    // Stereo/mono flag
    int stereo;

    // Playback frequency
    int frequency;

    /* Stream queueing is where we get everything ready to go but don't
       actually start it playing until the signal (for music sync, etc) */
    int     queueing;

    /* Have we been initialized yet? (and reserved a buffer, etc) */
    volatile int    initted;

    /* User data. */
    void *user_data;
} strchan_t;

// Our stream structs
static strchan_t streams[SND_STREAM_MAX];

// Separation buffers (for stereo)
static uint32 *sep_buffer[2] = {NULL, NULL};

/* the address of the sound ram from the SH4 side */
#define SPU_RAM_BASE            0xa0800000

// Check an incoming handle
#define CHECK_HND(x) do { \
        assert( (x) >= 0 && (x) < SND_STREAM_MAX ); \
        assert( streams[(x)].initted ); \
    } while(0)

/* Set "get data" callback */
void snd_stream_set_callback(snd_stream_hnd_t hnd, snd_stream_callback_t cb) {
    CHECK_HND(hnd);
    streams[hnd].get_data = cb;
}

void snd_stream_set_userdata(snd_stream_hnd_t hnd, void *d) {
    CHECK_HND(hnd);
    streams[hnd].user_data = d;
}

void *snd_stream_get_userdata(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    return streams[hnd].user_data;
}

void snd_stream_filter_add(snd_stream_hnd_t hnd, snd_stream_filter_t filtfunc, void * obj) {
    filter_t * f;

    CHECK_HND(hnd);

    f = malloc(sizeof(filter_t));
    f->func = filtfunc;
    f->data = obj;
    TAILQ_INSERT_TAIL(&streams[hnd].filters, f, lent);
}

void snd_stream_filter_remove(snd_stream_hnd_t hnd, snd_stream_filter_t filtfunc, void * obj) {
    filter_t * f;

    CHECK_HND(hnd);

    TAILQ_FOREACH(f, &streams[hnd].filters, lent) {
        if(f->func == filtfunc && f->data == obj) {
            TAILQ_REMOVE(&streams[hnd].filters, f, lent);
            free(f);
            return;
        }
    }
}

static void process_filters(snd_stream_hnd_t hnd, void **buffer, int *samplecnt) {
    filter_t * f;

    TAILQ_FOREACH(f, &streams[hnd].filters, lent) {
        f->func(hnd, f->data, streams[hnd].frequency, streams[hnd].stereo ? 2 : 1, buffer, samplecnt);
    }
}

static void sep_data(void *buffer, int len, int stereo) {
    register int16  *bufsrc, *bufdst;
    register int    x, y, cnt;

    if(stereo) {
        bufsrc = (int16*)buffer;
        bufdst = (int16*)sep_buffer[0];
        x = 0;
        y = 0;
        cnt = len / 2;

        do {
            *bufdst = *bufsrc;
            bufdst++;
            bufsrc += 2;
            cnt--;
        }
        while(cnt > 0);

        bufsrc = (int16*)buffer;
        bufsrc++;
        bufdst = (int16*)sep_buffer[1];
        x = 1;
        y = 0;
        cnt = len / 2;

        do {
            *bufdst = *bufsrc;
            bufdst++;
            bufsrc += 2;
            cnt--;
            x += 2;
            y++;
        }
        while(cnt > 0);
    }
    else {
        memcpy(sep_buffer[0], buffer, len);
        sep_buffer[1] = sep_buffer[0];
    }
}

static void stereo_pcm16_split_sq(uint32 *data, uint32 aica_left, uint32 aica_right, uint32 size) {

	/* Wait for both store queues to complete if they are already used */
	uint32 *d = (uint32 *)0xe0000000;
	d[0] = d[8] = 0;

	uint32 masked_left = (0xe0000000 | (aica_left & 0x03ffffe0));
	uint32 masked_right = (0xe0000000 | (aica_right & 0x03ffffe0));

	/* Set store queue memory area as desired */
	QACR0 = (aica_left >> 24) & 0x1c;
	QACR1 = (aica_right >> 24) & 0x1c;

	g2_fifo_wait();

	/* Separating channels and do fill/write queues as many times necessary. */
	snd_pcm16_split_sq(data, masked_left, masked_right, size);
}

/* Prefill buffers -- do this before calling start() */
void snd_stream_prefill(snd_stream_hnd_t hnd) {
    void *buf;
    int got;

    CHECK_HND(hnd);

    if(!streams[hnd].get_data) return;

    const uint32 buffer_size = streams[hnd].buffer_size;

    if(streams[hnd].stereo)
        buf = streams[hnd].get_data(hnd, buffer_size * 2, &got);
    else
        buf = streams[hnd].get_data(hnd, buffer_size, &got);

    process_filters(hnd, &buf, &got);

    if ((uintptr_t)buf & 31) {
        sep_data(buf, got, streams[hnd].stereo);
        spu_memload(streams[hnd].spu_ram_sch[0], (uint8*)sep_buffer[0], got);
        spu_memload(streams[hnd].spu_ram_sch[1], (uint8*)sep_buffer[1], got);
    }
    else {
        if (streams[hnd].stereo) {
            stereo_pcm16_split_sq((uint32 *)buf,
                streams[hnd].spu_ram_sch[0],
                streams[hnd].spu_ram_sch[1],
                got);
        }
        else {
            g2_fifo_wait();
            sq_cpy((uint32 *)streams[hnd].spu_ram_sch[0], buf, got);
            g2_fifo_wait();
            sq_cpy((uint32 *)streams[hnd].spu_ram_sch[1], buf, got);
        }
    }

    /* Start with playing on buffer 0 */
    streams[hnd].last_write_pos = 0;
    streams[hnd].curbuffer = 0;
}

/* Initialize stream system */
int snd_stream_init(void) {
    /* Create stereo seperation buffers */
    if(!sep_buffer[0]) {
        sep_buffer[0] = memalign(32, SND_STREAM_BUFFER_MAX);
        sep_buffer[1] = sep_buffer[0] + (SND_STREAM_BUFFER_MAX / 8);
    }

    /* Finish loading the stream driver */
    if(snd_init() < 0) {
        dbglog(DBG_ERROR, "snd_stream_init(): snd_init() failed, giving up\n");
        return -1;
    }

    return 0;
}

snd_stream_hnd_t snd_stream_alloc(snd_stream_callback_t cb, int bufsize) {
    int i, old;
    snd_stream_hnd_t hnd;

    // Get an unused handle
    hnd = -1;
    old = irq_disable();

    for(i = 0; i < SND_STREAM_MAX; i++) {
        if(!streams[i].initted) {
            hnd = i;
            break;
        }
    }

    if(hnd != -1)
        streams[hnd].initted = 1;

    irq_restore(old);

    if(hnd == -1)
        return SND_STREAM_INVALID;

    // Default this for now
    streams[hnd].buffer_size = bufsize;

    /* Start off with queueing disabled */
    streams[hnd].queueing = 0;

    /* Setup the callback */
    snd_stream_set_callback(hnd, cb);

    /* Initialize our filter chain list */
    TAILQ_INIT(&streams[hnd].filters);

    // Allocate stream buffers
    streams[hnd].spu_ram_sch[0] = snd_mem_malloc(streams[hnd].buffer_size * 2);
    streams[hnd].spu_ram_sch[1] = streams[hnd].spu_ram_sch[0] + streams[hnd].buffer_size;

    // And channels
    streams[hnd].ch[0] = snd_sfx_chn_alloc();
    streams[hnd].ch[1] = snd_sfx_chn_alloc();
    printf("snd_stream: alloc'd channels %d/%d\n", streams[hnd].ch[0], streams[hnd].ch[1]);

    return hnd;
}

snd_stream_hnd_t snd_stream_reinit(snd_stream_hnd_t hnd, snd_stream_callback_t cb) {
    CHECK_HND(hnd);

    /* Start off with queueing disabled */
    streams[hnd].queueing = 0;

    /* Setup the callback */
    snd_stream_set_callback(hnd, cb);

    return hnd;
}

void snd_stream_destroy(snd_stream_hnd_t hnd) {
    filter_t * c, * n;

    CHECK_HND(hnd);

    if(!streams[hnd].initted)
        return;

    snd_sfx_chn_free(streams[hnd].ch[0]);
    snd_sfx_chn_free(streams[hnd].ch[1]);

    c = TAILQ_FIRST(&streams[hnd].filters);

    while(c) {
        n = TAILQ_NEXT(c, lent);
        free(c);
        c = n;
    }

    TAILQ_INIT(&streams[hnd].filters);

    snd_stream_stop(hnd);
    snd_mem_free(streams[hnd].spu_ram_sch[0]);
    memset(streams + hnd, 0, sizeof(streams[0]));
}

/* Shut everything down and free mem */
void snd_stream_shutdown(void) {
    /* Stop and destroy all active stream */
    int i;

    for(i = 0; i < SND_STREAM_MAX; i++) {
        if(streams[i].initted)
            snd_stream_destroy(i);
    }

    /* Free global buffers */
    if(sep_buffer[0]) {
        free(sep_buffer[0]);
        sep_buffer[0] = NULL;
        sep_buffer[1] = NULL;
    }
}

/* Enable / disable stream queueing */
void snd_stream_queue_enable(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    streams[hnd].queueing = 1;
}

void snd_stream_queue_disable(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    streams[hnd].queueing = 0;
}

/* Start streaming (or if queueing is enabled, just get ready) */
void snd_stream_start(snd_stream_hnd_t hnd, uint32 freq, int st) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    CHECK_HND(hnd);

    if(!streams[hnd].get_data) return;

    streams[hnd].stereo = st;
    streams[hnd].frequency = freq;

    /* Make sure these are sync'd (and/or delayed) */
    snd_sh4_to_aica_stop();

    /* Prefill buffers */
    snd_stream_prefill(hnd);

    /* Channel 0 */
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = streams[hnd].ch[0];
    chan->cmd = AICA_CH_CMD_START | AICA_CH_START_DELAY;
    chan->base = streams[hnd].spu_ram_sch[0];
    chan->type = AICA_SM_16BIT;
    chan->length = (streams[hnd].buffer_size / 2);
    chan->loop = 1;
    chan->loopstart = 0;
    chan->loopend = (streams[hnd].buffer_size / 2);
    chan->freq = freq;
    chan->vol = 255;
    chan->pan = 0;
    snd_sh4_to_aica(tmp, cmd->size);

    /* Channel 1 */
    cmd->cmd_id = streams[hnd].ch[1];
    chan->base = streams[hnd].spu_ram_sch[1];
    chan->pan = 255;
    snd_sh4_to_aica(tmp, cmd->size);

    /* Start both channels simultaneously */
    cmd->cmd_id = (1 << streams[hnd].ch[0]) |
                  (1 << streams[hnd].ch[1]);
    chan->cmd = AICA_CH_CMD_START | AICA_CH_START_SYNC;
    snd_sh4_to_aica(tmp, cmd->size);

    /* Process the changes */
    if(!streams[hnd].queueing)
        snd_sh4_to_aica_start();
}

/* Actually make it go (in queued mode) */
void snd_stream_queue_go(snd_stream_hnd_t hnd) {
    CHECK_HND(hnd);
    snd_sh4_to_aica_start();
}

/* Stop streaming */
void snd_stream_stop(snd_stream_hnd_t hnd) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    CHECK_HND(hnd);

    if(!streams[hnd].get_data) return;

    /* Stop stream */
    /* Channel 0 */
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = streams[hnd].ch[0];
    chan->cmd = AICA_CH_CMD_STOP;
    snd_sh4_to_aica(tmp, cmd->size);

    /* Channel 1 */
    cmd->cmd_id = streams[hnd].ch[1];
    snd_sh4_to_aica(tmp, cmd->size);
}

/* The DMA will chain to this to start the second DMA. */
static uint32 dmadest, dmacnt;
static void dma_chain(ptr_t data) {
    (void)data;
    spu_dma_transfer(sep_buffer[1], dmadest, dmacnt, 0, NULL, 0);
}

/* Poll streamer to load more data if neccessary */
int snd_stream_poll(snd_stream_hnd_t hnd) {
    uint32 ch0pos, ch1pos;
    //int  realbuffer;
    uint32 current_play_pos;
    int    needed_samples;
    int    got_samples;
    void   *data;

    CHECK_HND(hnd);

    if(!streams[hnd].get_data) return -1;

    /* Get "real" buffer */
    ch0pos = g2_read_32(SPU_RAM_BASE + AICA_CHANNEL(streams[hnd].ch[0]) + offsetof(aica_channel_t, pos));
    ch1pos = g2_read_32(SPU_RAM_BASE + AICA_CHANNEL(streams[hnd].ch[1]) + offsetof(aica_channel_t, pos));

    if(ch0pos >= (streams[hnd].buffer_size / 2)) {
        dbglog(DBG_ERROR, "snd_stream_poll: chan0(%d).pos = %ld (%08lx)\n", streams[hnd].ch[0], ch0pos, ch0pos);
        return -1;
    }

    //realbuffer = !((ch0pos < (streams[hnd].buffer_size / 4)) && (ch1pos < (streams[hnd].buffer_size / 4)));

    current_play_pos = (ch0pos < ch1pos) ? (ch0pos) : (ch1pos);

    /* count just till the end of the buffer, so we don't have to
       handle buffer wraps */
    if(streams[hnd].last_write_pos <= current_play_pos)
        needed_samples = current_play_pos - streams[hnd].last_write_pos;
    else
        needed_samples = (streams[hnd].buffer_size / 2) - streams[hnd].last_write_pos;

    /* round it a little bit */
    needed_samples &= ~0x7ff;
    /* printf("last_write_pos %6u, current_play_pos %6u, needed_samples %6i\n",streams[hnd].last_write_pos,current_play_pos,needed_samples); */

    if(needed_samples > 0) {
        if(streams[hnd].stereo) {
            needed_samples = ((unsigned)needed_samples > streams[hnd].buffer_size/4) ? (int)streams[hnd].buffer_size/4 : needed_samples;
            data = streams[hnd].get_data(hnd, needed_samples * 4, &got_samples);
            process_filters(hnd, &data, &got_samples);

            if(got_samples < needed_samples * 4) {
                needed_samples = got_samples / 4;

                if(needed_samples & 3)
                    needed_samples = (needed_samples + 4) & ~3;
            }
        }
        else {
            needed_samples = ((unsigned)needed_samples > streams[hnd].buffer_size/2) ? (int)streams[hnd].buffer_size/2 : needed_samples;
            data = streams[hnd].get_data(hnd, needed_samples * 2, &got_samples);
            process_filters(hnd, &data, &got_samples);

            if(got_samples < needed_samples * 2) {
                needed_samples = got_samples / 2;

                if(needed_samples & 1)
                    needed_samples = (needed_samples + 2) & ~1;
            }
        }

        if(data == NULL) {
            /* Fill the "other" buffer with zeros */
            spu_memset(streams[hnd].spu_ram_sch[0] + (streams[hnd].last_write_pos * 2), 0, needed_samples * 2);
            spu_memset(streams[hnd].spu_ram_sch[1] + (streams[hnd].last_write_pos * 2), 0, needed_samples * 2);
            return -3;
        }

        if(streams[hnd].stereo) {
            sep_buffer[1] = sep_buffer[0] + (SND_STREAM_BUFFER_MAX / 8);
        }

        if ((uintptr_t)data & 31) {
            sep_data(data, needed_samples * 2, streams[hnd].stereo);
        } 
        else {
            if(streams[hnd].stereo) {
                snd_pcm16_split(data, sep_buffer[0], sep_buffer[1], needed_samples * 4);
            }
            else {
                sep_buffer[0] = data;
                sep_buffer[1] = data;
            }
        }

        // Second DMA will get started by the chain handler
        dcache_flush_range((uint32)sep_buffer[0], needed_samples * 2);
        if (streams[hnd].stereo) {
            dcache_flush_range((uint32)sep_buffer[1], needed_samples * 2);
        }
        dmadest = streams[hnd].spu_ram_sch[1] + (streams[hnd].last_write_pos * 2);
        dmacnt = needed_samples * 2;
        spu_dma_transfer(sep_buffer[0], streams[hnd].spu_ram_sch[0] + (streams[hnd].last_write_pos * 2), needed_samples * 2, 0, dma_chain, 0);

        streams[hnd].last_write_pos += needed_samples;

        if(streams[hnd].last_write_pos >= (streams[hnd].buffer_size / 2))
            streams[hnd].last_write_pos -= (streams[hnd].buffer_size / 2);
    }

    return 0;
}

/* Set the volume on the streaming channels */
void snd_stream_volume(snd_stream_hnd_t hnd, int vol) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    CHECK_HND(hnd);

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = streams[hnd].ch[0];
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL;
    chan->vol = vol;
    snd_sh4_to_aica(tmp, cmd->size);

    cmd->cmd_id = streams[hnd].ch[1];
    snd_sh4_to_aica(tmp, cmd->size);
}
