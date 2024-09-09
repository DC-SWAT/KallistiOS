/* KallistiOS ##version##

   cdrom.c

   Copyright (C) 2000 Megan Potter
   Copyright (C) 2014 Lawrence Sebald
   Copyright (C) 2014 Donald Haase
   Copyright (C) 2023, 2024 Ruslan Rostovtsev
   Copyright (C) 2024 Andy Barajas

 */
#include <assert.h>

#include <arch/cache.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <arch/memory.h>

#include <dc/asic.h>
#include <dc/cdrom.h>
#include <dc/g1ata.h>
#include <dc/syscalls.h>

#include <kos/thread.h>
#include <kos/mutex.h>
#include <kos/dbglog.h>

/*

This module contains low-level primitives for accessing the CD-Rom (I
refer to it as a CD-Rom and not a GD-Rom, because this code will not
access the GD area, by design). Whenever a file is accessed and a new
disc is inserted, it reads the TOC for the disc in the drive and
gets everything situated. After that it will read raw sectors from
the data track on a standard DC bootable CDR (one audio track plus
one data track in xa1 format).

Most of the information/algorithms in this file are thanks to
Marcus Comstedt. Thanks to Maiwe for the verbose command names and
also for the CDDA playback routines.

Note that these functions may be affected by changing compiler options...
they require their parameters to be in certain registers, which is
normally the case with the default options. If in doubt, decompile the
output and look to make sure.

XXX: This could all be done in a non-blocking way by taking advantage of
command queuing. Every call to syscall_gdrom_send_command returns a 
'request id' which just needs to eventually be checked by cmd_stat. A 
non-blocking version of all functions would simply require manual calls 
to check the status. Doing this would probably allow data reading while 
cdda is playing without hiccups (by severely reducing the number of gd 
commands being sent).
*/

/* Protection register. */
#define G1_ATA_DMA_PROTECTION   0x005F74B8
/* Protection register code. */
#define G1_DMA_UNLOCK_CODE      0x8843
/* System memory protection unlock value. */
#define G1_DMA_UNLOCK_SYSMEM    (G1_DMA_UNLOCK_CODE << 16 | 0x407F)
/* All memory protection unlock value. */
#define G1_DMA_UNLOCK_ALLMEM    (G1_DMA_UNLOCK_CODE << 16 | 0x007F)

typedef int gdc_cmd_hnd_t;

/* The G1 ATA access mutex */
mutex_t _g1_ata_mutex = MUTEX_INITIALIZER;

static gdc_cmd_hnd_t cmd_hnd = 0;
static int stream_mode = 0;
static int dma_in_progress = 0;
static int dma_blocking = 0;
static kthread_t *dma_thd = NULL;
static semaphore_t dma_done = SEM_INITIALIZER(0);
static int cur_sector_size = 2048;

/* Shortcut to cdrom_reinit_ex. Typically this is the only thing changed. */
int cdrom_set_sector_size(int size) {
    return cdrom_reinit_ex(-1, -1, size);
}

/* Command execution sequence */
int cdrom_exec_cmd(int cmd, void *param) {
    return cdrom_exec_cmd_timed(cmd, param, 0);
}

int cdrom_exec_cmd_timed(int cmd, void *param, int timeout) {
    int32_t status[4] = {
        0, /* Error code 1 */
        0, /* Error code 2 */
        0, /* Transferred size */
        0  /* ATA status waiting */
    };
    gdc_cmd_hnd_t hnd;
    int n, rv = ERR_OK;
    uint64_t begin;

    assert(cmd > 0 && cmd < CMD_MAX);
    mutex_lock(&_g1_ata_mutex);

    /* Submit the command */
    for(n = 0; n < 10; ++n) {
        hnd = syscall_gdrom_send_command(cmd, param);
        if (hnd != 0) {
            break;
        }
        syscall_gdrom_exec_server();
        thd_pass();
    }

    if(hnd <= 0) {
        mutex_unlock(&_g1_ata_mutex);
        return ERR_SYS;
    }

    /* Wait command to finish */
    if(timeout) {
        begin = timer_ms_gettime64();
    }
    do {
        syscall_gdrom_exec_server();
        n = syscall_gdrom_check_command(hnd, status);

        if(n != PROCESSING && n != BUSY) {
            break;
        }
        if(timeout) {
            if((timer_ms_gettime64() - begin) >= (unsigned)timeout) {
                mutex_unlock(&_g1_ata_mutex);
                cdrom_abort_cmd(timeout);
                rv = ERR_TIMEOUT;
                dbglog(DBG_ERROR, "cdrom_exec_cmd_timed: Timeout exceeded\n");
                break;
            }
        }
        thd_pass();
    } while(1);

    cmd_hnd = (n == STREAMING ? hnd : 0);

    if (rv != ERR_TIMEOUT) {
        mutex_unlock(&_g1_ata_mutex);
    }

    if(rv != ERR_OK)
        return rv;
    else if(n == COMPLETED || n == STREAMING)
        return ERR_OK;
    else if(n == NO_ACTIVE)
        return ERR_NO_ACTIVE;
    else {
        switch(status[0]) {
            case 2:
                return ERR_NO_DISC;
            case 6:
                return ERR_DISC_CHG;
            default:
                return ERR_SYS;
        }
        if(status[1] != 0)
            return ERR_SYS;
    }
}

int cdrom_abort_cmd(uint32_t timeout) {
    int32_t status[4] = {
        0, /* Error code 1 */
        0, /* Error code 2 */
        0, /* Transferred size */
        0  /* ATA status waiting */
    };
    int rs, rv = ERR_OK;
    uint64_t begin;

    if (cmd_hnd <= 0) {
        return ERR_NO_ACTIVE;
    }

    mutex_lock(&_g1_ata_mutex);
    syscall_gdrom_abort_command(cmd_hnd);

    if(timeout) {
        begin = timer_ms_gettime64();
    }
    do {
        syscall_gdrom_exec_server();
        rs = syscall_gdrom_check_command(cmd_hnd, status);

        if(rs == NO_ACTIVE || rs == COMPLETED) {
            break;
        }
        if(timeout) {
            if((timer_ms_gettime64() - begin) >= (unsigned)timeout) {
                dbglog(DBG_ERROR, "cdrom_abort_cmd: Timeout exceeded, resetting.\n");
                rv = ERR_TIMEOUT;
                syscall_gdrom_reset();
                syscall_gdrom_init();
                break;
            }
        }
        thd_pass();
    } while(1);

    cmd_hnd = 0;
    mutex_unlock(&_g1_ata_mutex);
    return rv;
}

/* Return the status of the drive as two integers (see constants) */
int cdrom_get_status(int *status, int *disc_type) {
    int rv = ERR_OK;
    uint32_t params[2];

    /* We might be called in an interrupt to check for ISO cache
       flushing, so make sure we're not interrupting something
       already in progress. */
    if(irq_inside_int()) {
        if(mutex_trylock(&_g1_ata_mutex))
            /* DH: Figure out a better return to signal error */
            return -1;
    }
    else {
        mutex_lock(&_g1_ata_mutex);
    }

    do {
        rv = syscall_gdrom_check_drive(params);

        if(rv != BUSY) {
            break;
        }
        thd_pass();
    } while(1);

    mutex_unlock(&_g1_ata_mutex);

    if(rv >= 0) {
        if(status != NULL)
            *status = params[0];

        if(disc_type != NULL)
            *disc_type = params[1];
    }
    else {
        if(status != NULL)
            *status = -1;

        if(disc_type != NULL)
            *disc_type = -1;
    }

    return rv;
}

/* Helper function to account for long-standing typo */
int cdrom_change_dataype(int sector_part, int cdxa, int sector_size) {
    return cdrom_change_datatype(sector_part, cdxa, sector_size);
}

/* Wrapper for the change datatype syscall */
int cdrom_change_datatype(int sector_part, int cdxa, int sector_size) {
    int rv = ERR_OK;
    uint32_t params[4];

    mutex_lock(&_g1_ata_mutex);

    /* Check if we are using default params */
    if(sector_size == 2352) {
        if(cdxa == -1)
            cdxa = 0;

        if(sector_part == -1)
            sector_part = CDROM_READ_WHOLE_SECTOR;
    }
    else {
        if(cdxa == -1) {
            /* If not overriding cdxa, check what the drive thinks we should 
               use */
            syscall_gdrom_check_drive(params);
            cdxa = (params[1] == 32 ? 2048 : 1024);
        }

        if(sector_part == -1)
            sector_part = CDROM_READ_DATA_AREA;

        if(sector_size == -1)
            sector_size = 2048;
    }

    params[0] = 0;              /* 0 = set, 1 = get */
    params[1] = sector_part;    /* Get Data or Full Sector */
    params[2] = cdxa;           /* CD-XA mode 1/2 */
    params[3] = sector_size;    /* sector size */
    cur_sector_size = sector_size;
    rv = syscall_gdrom_sector_mode(params);
    mutex_unlock(&_g1_ata_mutex);
    return rv;
}

/* Re-init the drive, e.g., after a disc change, etc */
int cdrom_reinit(void) {
    /* By setting -1 to each parameter, they fall to the old defaults */
    return cdrom_reinit_ex(-1, -1, -1);
}

/* Enhanced cdrom_reinit, takes the place of the old 'sector_size' function */
int cdrom_reinit_ex(int sector_part, int cdxa, int sector_size) {
    int r;

    do {
        r = cdrom_exec_cmd_timed(CMD_INIT, NULL, 10000);
    } while(r == ERR_DISC_CHG);

    if(r == ERR_NO_DISC || r == ERR_SYS || r == ERR_TIMEOUT) {
        return r;
    }

    r = cdrom_change_datatype(sector_part, cdxa, sector_size);

    return r;
}

/* Read the table of contents */
int cdrom_read_toc(CDROM_TOC *toc_buffer, int session) {
    struct {
        int session;
        void *buffer;
    } params;
    int rv;

    params.session = session;
    params.buffer = toc_buffer;

    rv = cdrom_exec_cmd(CMD_GETTOC2, &params);

    return rv;
}

/* Enhanced Sector reading: Choose mode to read in. */
int cdrom_read_sectors_ex(void *buffer, int sector, int cnt, int mode) {
    struct {
        int sec, num;
        void *buffer;
        int is_test;
    } params;
    int rv = ERR_OK;
    uintptr_t buf_addr = ((uintptr_t)buffer);

    params.sec = sector;    /* Starting sector */
    params.num = cnt;       /* Number of sectors */
    params.is_test = 0;     /* Enable test mode */

    /* The DMA mode blocks the thread it is called in by the way we execute
       gd syscalls. It does however allow for other threads to run. */
    if (mode == CDROM_READ_DMA) {

        params.buffer = (void *)(buf_addr & MEM_AREA_CACHE_MASK);

        if (buf_addr & 0x1f) {
            dbglog(DBG_ERROR, "cdrom_read_sectors_ex: Unaligned memory for DMA (32-byte).\n");
            return ERR_SYS;
        }
        if ((buf_addr >> 24) == 0x0c) {
            dcache_inval_range((uintptr_t)params.buffer, cnt * cur_sector_size);
        }
        rv = cdrom_exec_cmd(CMD_DMAREAD, &params);
    }
    else if(mode == CDROM_READ_PIO) {

        params.buffer = buffer;

        if (buf_addr & 0x01) {
            dbglog(DBG_ERROR, "cdrom_read_sectors_ex: Unaligned memory for PIO (2-byte).\n");
            return ERR_SYS;
        }
        rv = cdrom_exec_cmd(CMD_PIOREAD, &params);
    }
    return rv;
}

/* Basic old sector read */
int cdrom_read_sectors(void *buffer, int sector, int cnt) {
    return cdrom_read_sectors_ex(buffer, sector, cnt, CDROM_READ_PIO);
}

int cdrom_stream_start(int sector, int cnt, int mode) {
    struct {
        int sec;
        int num;
    } params;
    int rv = ERR_SYS;

    params.sec = sector;
    params.num = cnt;

    if (cmd_hnd > 0) {
        cdrom_abort_cmd(1000);
    }

    if (mode == CDROM_READ_DMA) {
        rv = cdrom_exec_cmd(CMD_DMAREAD_STREAM, &params);
    }
    else if (mode == CDROM_READ_PIO) {
        rv = cdrom_exec_cmd(CMD_PIOREAD_STREAM, &params);
    }

    if (rv == ERR_OK) {
        stream_mode = mode;
    }
    return rv;
}

int cdrom_stream_stop(void) {
    int rv = ERR_OK;
    int rs;
    int32_t status[4] = {
        0, /* Error code 1 */
        0, /* Error code 2 */
        0, /* Transferred size */
        0  /* ATA status waiting */
    };

    if (cmd_hnd <= 0) {
        return rv;
    }
    if (stream_mode != CDROM_READ_DMA || cdrom_stream_progress(NULL) == 0) {
        mutex_lock(&_g1_ata_mutex);
    }

    do {
        syscall_gdrom_exec_server();
        rs = syscall_gdrom_check_command(cmd_hnd, status);

        if (rs < 0) {
            rv = ERR_SYS;
            break;
        }
        else if (rs == COMPLETED || rs == NO_ACTIVE) {
            break;
        }
        else if (rs == STREAMING) {
            mutex_unlock(&_g1_ata_mutex);
            cdrom_abort_cmd(1000);
            break;
        }
        thd_pass();
    } while (1);

    cmd_hnd = 0;

    if (rs != STREAMING) {
        mutex_unlock(&_g1_ata_mutex);
    }

    if (dma_in_progress) {
        dma_in_progress = 0;
        if (dma_blocking) {
            dma_blocking = 0;
            sem_signal(&dma_done);
        }
    }
    return rv;
}

int cdrom_stream_request(void *buffer, size_t size, int block) {
    int rs;
    int32_t params[2];
    size_t check_size = -1;
    int32_t status[4] = {
        0, /* Error code 1 */
        0, /* Error code 2 */
        0, /* Transferred size */
        0  /* ATA status waiting */
    };

    if (cmd_hnd <= 0) {
        return ERR_NO_ACTIVE;
    }
    if (cdrom_stream_progress(NULL)) {
        dbglog(DBG_ERROR, "cdrom_stream_request: Previous request in progress.\n");
        return ERR_SYS;
    }

    if (stream_mode == CDROM_READ_DMA) {
        params[0] = ((uintptr_t)buffer) & MEM_AREA_CACHE_MASK;
        if (params[0] & 0x1f) {
            dbglog(DBG_ERROR, "cdrom_stream_request: Unaligned memory for DMA (32-byte).\n");
            return ERR_SYS;
        }
        if ((params[0] >> 24) == 0x0c) {
            dcache_inval_range((uintptr_t)buffer, size);
        }
    }
    else if(stream_mode == CDROM_READ_PIO) {
        params[0] = (uintptr_t)buffer;
        if (params[0] & 0x01) {
            dbglog(DBG_ERROR, "cdrom_stream_request: Unaligned memory for PIO (2-byte).\n");
            return ERR_SYS;
        }
    }

    params[1] = size;
    mutex_lock(&_g1_ata_mutex);

    if (stream_mode == CDROM_READ_DMA) {

        dma_in_progress = 1;
        dma_blocking = block;
        dma_thd = thd_current;

        rs = syscall_gdrom_dma_transfer(cmd_hnd, params);

        if (rs < 0) {
            dma_in_progress = 0;
            dma_blocking = 0;
            mutex_unlock(&_g1_ata_mutex);
            return ERR_SYS;
        }
        if (block == 0) {
            return ERR_OK;
        }
        sem_wait(&dma_done);

        do {
            syscall_gdrom_exec_server();
            rs = syscall_gdrom_check_command(cmd_hnd, status);

            if (rs < 0) {
                mutex_unlock(&_g1_ata_mutex);
                return ERR_SYS;
            }
            else if(rs == COMPLETED || rs == NO_ACTIVE) {
                cmd_hnd = 0;
                break;
            }
            else if (syscall_gdrom_dma_check(cmd_hnd, &check_size) == 0) {
                break;
            }
            thd_pass();

        } while (1);
    }
    else if(stream_mode == CDROM_READ_PIO) {

        rs = syscall_gdrom_pio_transfer(cmd_hnd, params);

        if (rs < 0) {
            mutex_unlock(&_g1_ata_mutex);
            return ERR_SYS;
        }
        do {
            syscall_gdrom_exec_server();
            rs = syscall_gdrom_check_command(cmd_hnd, status);

            if (rs < 0) {
                mutex_unlock(&_g1_ata_mutex);
                return ERR_SYS;
            }
            else if(rs == COMPLETED || rs == NO_ACTIVE) {
                cmd_hnd = 0;
                break;
            }
            else if (syscall_gdrom_pio_check(cmd_hnd, &check_size) == 0) {
                break;
            }
            thd_pass();
        } while (1);
    }

    mutex_unlock(&_g1_ata_mutex);
    return ERR_OK;
}

int cdrom_stream_progress(size_t *size) {
    int rv = 0;
    size_t check_size = 0;

    if (cmd_hnd <= 0) {
        if (size) {
            *size = check_size;
        }
        return rv;
    }
    mutex_lock(&_g1_ata_mutex);

    if (stream_mode == CDROM_READ_DMA) {
        rv = syscall_gdrom_dma_check(cmd_hnd, &check_size);
    }
    else if (stream_mode == CDROM_READ_PIO) {
        rv = syscall_gdrom_pio_check(cmd_hnd, &check_size);
    }

    if (rv == 0) {
        syscall_gdrom_exec_server();
    }
    mutex_unlock(&_g1_ata_mutex);

    if (size) {
        *size = check_size;
    }
    return rv;
}

/* Read a piece of or all of the Q byte of the subcode of the last sector read.
   If you need the subcode from every sector, you cannot read more than one at 
   a time. */
/* XXX: Use some CD-Gs and other stuff to test if you get more than just the 
   Q byte */
int cdrom_get_subcode(void *buffer, int buflen, int which) {
    struct {
        int which;
        int buflen;
        void *buffer;
    } params;
    int rv;

    params.which = which;
    params.buflen = buflen;
    params.buffer = buffer;
    rv = cdrom_exec_cmd(CMD_GETSCD, &params);
    return rv;
}

/* Locate the LBA sector of the data track; use after reading TOC */
uint32 cdrom_locate_data_track(CDROM_TOC *toc) {
    int i, first, last;

    first = TOC_TRACK(toc->first);
    last = TOC_TRACK(toc->last);

    if(first < 1 || last > 99 || first > last)
        return 0;

    /* Find the last track which as a CTRL of 4 */
    for(i = last; i >= first; i--) {
        if(TOC_CTRL(toc->entry[i - 1]) == 4)
            return TOC_LBA(toc->entry[i - 1]);
    }

    return 0;
}

/* Play CDDA tracks
   start  -- track to play from
   end    -- track to play to
   repeat -- number of times to repeat (0-15, 15=infinite)
   mode   -- CDDA_TRACKS or CDDA_SECTORS
 */
int cdrom_cdda_play(uint32 start, uint32 end, uint32 repeat, int mode) {
    struct {
        int start;
        int end;
        int repeat;
    } params;
    int rv = ERR_OK;

    /* Limit to 0-15 */
    if(repeat > 15)
        repeat = 15;

    params.start = start;
    params.end = end;
    params.repeat = repeat;

    if(mode == CDDA_TRACKS)
        rv = cdrom_exec_cmd(CMD_PLAY, &params);
    else if(mode == CDDA_SECTORS)
        rv = cdrom_exec_cmd(CMD_PLAY2, &params);

    return rv;
}

/* Pause CDDA audio playback */
int cdrom_cdda_pause(void) {
    int rv;
    rv = cdrom_exec_cmd(CMD_PAUSE, NULL);
    return rv;
}

/* Resume CDDA audio playback */
int cdrom_cdda_resume(void) {
    int rv;
    rv = cdrom_exec_cmd(CMD_RELEASE, NULL);
    return rv;
}

/* Spin down the CD */
int cdrom_spin_down(void) {
    int rv;
    rv = cdrom_exec_cmd(CMD_STOP, NULL);
    return rv;
}

static void g1_dma_irq_hnd(uint32_t code, void *data) {
    (void)code;
    (void)data;

    if (dma_in_progress) {
        syscall_gdrom_dma_callback(0, NULL);
        dma_in_progress = 0;

        if(dma_blocking) {
            dma_blocking = 0;
            mutex_unlock_as_thread(&_g1_ata_mutex, dma_thd);
            sem_signal(&dma_done);
            thd_schedule(1, 0);
        }
    }
}

static void unlock_dma_memory(void) {
    volatile uint32_t *prot_reg = (uint32_t *)(G1_ATA_DMA_PROTECTION | MEM_AREA_P2_BASE);
    const size_t size_loc = 16 << 10;
    uintptr_t start_loc = (0x0c000000 | MEM_AREA_P2_BASE);
    uint32_t end_loc = start_loc + size_loc;
    uintptr_t cur_loc;
    int count = 0;

    for(cur_loc = start_loc; cur_loc <= end_loc; cur_loc += sizeof(uint32_t)) {
        if(*(uint32_t *)cur_loc == (uint32_t)G1_DMA_UNLOCK_SYSMEM) {
            *(uint32_t *)cur_loc = G1_DMA_UNLOCK_ALLMEM;
            ++count;
        }
    }
    if (count) {
        icache_flush_range(0x0c000000 | MEM_AREA_P1_BASE, size_loc);
    }
    *prot_reg = G1_DMA_UNLOCK_ALLMEM;
}

/* Initialize: assume no threading issues */
void cdrom_init(void) {
    uint32_t p;
    volatile uint32_t *react = (uint32_t *)(0x005f74e4 | MEM_AREA_P2_BASE);
    volatile uint32_t *bios = (uint32_t *)MEM_AREA_P2_BASE;

    mutex_lock(&_g1_ata_mutex);

    /* Reactivate drive: send the BIOS size and then read each
       word across the bus so the controller can verify it.
       If first bytes are 0xe6ff instead of usual 0xe3ff, then
       hardware is fitted with custom BIOS using magic bootstrap
       which can and must pass controller verification with only
       the first 1024 bytes */
    if((*(uint16_t *)MEM_AREA_P2_BASE) == 0xe6ff) {
        *react = 0x3ff;
        for(p = 0; p < 0x400 / sizeof(bios[0]); p++) {
            (void)bios[p];
        }
    } else {
        *react = 0x1fffff;
        for(p = 0; p < 0x200000 / sizeof(bios[0]); p++) {
            (void)bios[p];
        }
    }

    /* Reset system functions */
    syscall_gdrom_reset();
    syscall_gdrom_init();

    unlock_dma_memory();
    mutex_unlock(&_g1_ata_mutex);

    /* Hook all the DMA related events. */
    asic_evt_set_handler(ASIC_EVT_GD_DMA, g1_dma_irq_hnd, NULL);
    asic_evt_enable(ASIC_EVT_GD_DMA, ASIC_IRQB);
    asic_evt_set_handler(ASIC_EVT_GD_DMA_OVERRUN, g1_dma_irq_hnd, NULL);
    asic_evt_enable(ASIC_EVT_GD_DMA_OVERRUN, ASIC_IRQB);
    asic_evt_set_handler(ASIC_EVT_GD_DMA_ILLADDR, g1_dma_irq_hnd, NULL);
    asic_evt_enable(ASIC_EVT_GD_DMA_ILLADDR, ASIC_IRQB);

    cdrom_reinit();
}

void cdrom_shutdown(void) {

    /* Unhook the events and disable the IRQs. */
    asic_evt_disable(ASIC_EVT_GD_DMA, ASIC_IRQB);
    asic_evt_remove_handler(ASIC_EVT_GD_DMA);
    asic_evt_disable(ASIC_EVT_GD_DMA_OVERRUN, ASIC_IRQB);
    asic_evt_remove_handler(ASIC_EVT_GD_DMA_OVERRUN);
    asic_evt_disable(ASIC_EVT_GD_DMA_ILLADDR, ASIC_IRQB);
    asic_evt_remove_handler(ASIC_EVT_GD_DMA_ILLADDR);
}
