/* KallistiOS ##version##

   vmu_pkg.c
   (c)2002 Megan Potter
*/

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dc/vmu_pkg.h>
#include <kos/regfield.h>

/*

VMU data files can be stored raw, but if you want to interact with the
rest of the DC world, it's much better to package them in a nice data
file format. This module takes care of that for you.

Thanks to Marcus Comstedt for this information.

*/

struct ico_header {
    uint16_t resv;
    uint16_t type;
    uint16_t nb_images;
};

struct ico_dir {
    uint8_t width;
    uint8_t height;
    uint8_t nb_colors;
    uint8_t resv;
    uint16_t nb_planes;
    uint16_t bpp;
    uint32_t size;
    uint32_t offset;
};

struct bmp_dib_header {
    uint32_t hdr_size;
    int32_t width;
    int32_t height;
    uint16_t nb_planes;
    uint16_t bpp;
    uint32_t comp;
    uint32_t size;
    uint32_t hppm;
    uint32_t vppm;
    uint32_t nb_colors;
    uint32_t important_colors;
};

/* CRC calculation: calculates the CRC on a VMU file to be written out */
static uint16_t vmu_pkg_crc(const uint8_t *buf, size_t size) {
    size_t i;
    uint16_t c, n;

    for(i = 0, n = 0; i < size; i++) {
        n ^= (buf[i] << 8);

        for(c = 0; c < 8; c++) {
            if(n & 0x8000)
                n = (n << 1) ^ 0x1021;
            else
                n = (n << 1);
        }
    }

    return n;
}

static int vmu_eyecatch_size(int eyecatch_type) {
    switch(eyecatch_type) {
        case VMUPKG_EC_NONE:
            return 0;
        case VMUPKG_EC_16BIT:
            return 72 * 56 * 2;
        case VMUPKG_EC_256COL:
            return 512 + 72 * 56;
        case VMUPKG_EC_16COL:
            return 32 + 72 * 56 / 2;
        default:
            return -1;
    }
}

/* Converts a vmu_pkg_t structure into an array of uint8_t's which may be
   written to a VMU file via fs_vmu, or whatever. */
int vmu_pkg_build(vmu_pkg_t *src, uint8_t **dst, int *dst_size) {
    uint8_t     *out;
    int         ec_size, out_size;
    vmu_hdr_t   *hdr;

    assert(src && dst);

    /* First off, figure out how big it will be */
    out_size = sizeof(vmu_hdr_t) + 512 * src->icon_cnt + src->data_len;
    ec_size = vmu_eyecatch_size(src->eyecatch_type);

    if(ec_size < 0) return -1;

    out_size += ec_size;

    /* Allocate a return array */
    out = malloc(out_size);

    if(!out) return -1;

    /* We're done so write outputs */
    *dst_size = out_size;
    *dst = out;

    /* Setup some defaults */
    memset(out, 0, out_size);
    hdr = (vmu_hdr_t *)out;
    memset(hdr->desc_short, 32, sizeof(hdr->desc_short));
    memset(hdr->desc_long, 32, sizeof(hdr->desc_long));

    /* Fill in the data from the pkg struct */
    memcpy(hdr->desc_short, src->desc_short, strlen(src->desc_short));
    memcpy(hdr->desc_long, src->desc_long, strlen(src->desc_long));
    strcpy(hdr->app_id, src->app_id);
    hdr->icon_cnt = src->icon_cnt;
    hdr->icon_anim_speed = src->icon_anim_speed;
    hdr->eyecatch_type = src->eyecatch_type;
    hdr->crc = 0;
    hdr->data_len = src->data_len;
    memcpy(hdr->icon_pal, src->icon_pal, sizeof(src->icon_pal));
    out += sizeof(vmu_hdr_t);

    memcpy(out, src->icon_data, 512 * src->icon_cnt);
    out += 512 * src->icon_cnt;

    memcpy(out, src->eyecatch_data, ec_size);
    out += ec_size;

    memcpy(out, src->data, src->data_len);
    out += src->data_len;

    /* Verify the size */
    assert((out - *dst) == out_size);
    out = *dst;

    /* Calculate CRC */
    hdr->crc = vmu_pkg_crc(out, out_size);

    return 0;
}

/* Parse an array of uint8_t's (i.e. a VMU data file) into a
 * vmu_pkg_t package structure. */
int vmu_pkg_parse(uint8_t *data, size_t data_size, vmu_pkg_t *pkg) {
    uint16_t crc, crc_save;
    int ec_size, hdr_size, total_size, icon_size;
    vmu_hdr_t *hdr;

    assert(data && pkg);

    hdr = (vmu_hdr_t *)data;

    icon_size = 512 * hdr->icon_cnt;
    ec_size = vmu_eyecatch_size(hdr->eyecatch_type);
    hdr_size = sizeof(vmu_hdr_t) + icon_size + ec_size;
    total_size = hdr->data_len + hdr_size;

    if(total_size < 0 || (size_t)total_size > data_size) {
        dbglog(DBG_ERROR, "vmu_pkg_parse: file header is corrupted, or no header?\n");
        return -1;
    }

    /* Verify the CRC.  Note: this writes a zero byte into data[].
     * The byte is later restored in case data is an mmapped pointer to a
     * writable file, and a new header is not generated by the user. */
    crc_save = hdr->crc;
    hdr->crc = 0;
    crc = vmu_pkg_crc(data, total_size);
    hdr->crc = crc_save;

    if(crc_save != crc) {
        dbglog(DBG_ERROR, "vmu_pkg_parse: expected CRC %04x, got %04x\n", crc_save, crc);
        return -1;
    }

    /* Fill in pkg struct for caller. */
    pkg->icon_cnt = hdr->icon_cnt;
    pkg->icon_anim_speed = hdr->icon_anim_speed;
    pkg->eyecatch_type = hdr->eyecatch_type;
    pkg->data_len = hdr->data_len;
    memcpy(pkg->icon_pal, hdr->icon_pal, sizeof(hdr->icon_pal));
    pkg->icon_data = data + sizeof(vmu_hdr_t);
    pkg->eyecatch_data = data + sizeof(vmu_hdr_t) + icon_size;
    pkg->data = data + hdr_size;
    /* Copy space and null-padded fields, keeping the padding,
     * and ensure our representations are null-terminated. */
    memcpy(pkg->desc_short, hdr->desc_short, sizeof(hdr->desc_short));
    memcpy(pkg->desc_long, hdr->desc_long, sizeof(hdr->desc_long));
    memcpy(pkg->app_id, hdr->app_id, sizeof(hdr->app_id));
    *(pkg->desc_short + sizeof(hdr->desc_short)) = '\0';
    *(pkg->desc_long  + sizeof(hdr->desc_long))  = '\0';
    *(pkg->app_id     + sizeof(hdr->app_id))     = '\0';

    return 0;
}

static unsigned int pal_get_map(uint32_t *pal, const uint32_t *curr_pal,
                                uint8_t *map, unsigned int nb_colors) {
    unsigned int i, j;

    for(i = 0; i < 16; i++) {
        for(j = 0; j < nb_colors; j++)
            if(curr_pal[i] == pal[j])
                break;

        if(j < nb_colors) {
            /* Found the color in our palette */
            map[i] = j;
            continue;
        }

        if(nb_colors == 15) {
            /* No colors left :(
             * Note that we limit to 15 colors to leave the 16th color for
             * transparent pixels. */
            return 0;
        }

        /* Add the new color to our palette */
        pal[nb_colors] = curr_pal[i];
        map[i] = nb_colors++;
    }

    return nb_colors;
}

static uint16_t argb8888_to_argb4444(uint32_t px) {
    return 0xf000 |
        ((px >> 12) & 0x0f00) |
        ((px >> 8) & 0x00f0) |
        ((px >> 4) & 0x000f);
}

static void vmu_pkg_load_palette(vmu_pkg_t *pkg, const uint32_t *pal,
                                 unsigned int nb_colors) {
    unsigned int i;

    for(i = 0; i < nb_colors; i++)
        pkg->icon_pal[i] = argb8888_to_argb4444(pal[i]);

    pkg->icon_pal[15] = 0x0; /* Transparent pixel */
}

int vmu_pkg_load_icon(vmu_pkg_t *pkg, const char *icon_fn) {
    uint8_t px, pxh, pxl, pal_map[16];
    unsigned int i, j, x, y, nb_colors = 0;
    struct bmp_dib_header dib;
    uint32_t palette[16], curr_palette[16];
    struct ico_header hdr;
    struct ico_dir *dir;
    uint8_t and_mask;
    uint8_t frame[512];
    file_t fd;

    if(!pkg->icon_cnt || !pkg->icon_data) {
        dbglog(DBG_ERROR, "vmu_pkg_load_icon: vmu_pkg_t icon not preallocated\n");
        return -1;
    }

    fd = fs_open(icon_fn, O_RDONLY);
    if(fd == -1)
        return fd;

    fs_read(fd, &hdr, sizeof(hdr));
    if(hdr.resv != 0 || hdr.type != 1) {
        dbglog(DBG_ERROR, "vmu_pkg_load_icon: Invalid .ico header\n");
        goto out_err_close;
    }

    /* The ICO has less frames than preallocated? No problem */
    if(hdr.nb_images < pkg->icon_cnt)
        pkg->icon_cnt = hdr.nb_images;

    /* The ICO has more frames than preallocated? Warn and continue */
    if(hdr.nb_images > pkg->icon_cnt) {
        dbglog(DBG_WARNING, "vmu_pkg_load_icon: .ico file has %u frames but we only have space for %u\n",
               hdr.nb_images, pkg->icon_cnt);
    }

    dir = alloca(pkg->icon_cnt * sizeof(*dir));

    for(i = 0; i < hdr.nb_images; i++) {
        fs_read(fd, &dir[i], sizeof(*dir));

        if(dir->width != 32 || dir->height != 32 || dir->bpp != 4) {
            dbglog(DBG_ERROR, "vmu_pkg_load_icon: Invalid .ico width, height or bpp\n");
            goto out_err_close;
        }
    }

    for(i = 0; i < (unsigned int)pkg->icon_cnt; i++) {
        fs_read(fd, &dib, sizeof(dib));

        if(dib.hdr_size != 40) {
            dbglog(DBG_ERROR, "vmu_pkg_load_icon: Invalid DIB header for frame %u\n", i);
            goto out_err_close;
        }

        if(dib.comp != 0) {
            dbglog(DBG_ERROR, "vmu_pkg_load_icon: Only uncompressed .ico are supported.\n");
            goto out_err_close;
        }

        fs_read(fd, curr_palette, sizeof(curr_palette));
        nb_colors = pal_get_map(palette, curr_palette, pal_map, nb_colors);
        if(!nb_colors) {
            dbglog(DBG_ERROR, "vmu_pkg_load_icon: .ico has > 15 colors.\n");
            goto out_err_close;
        }

        /* Read frame data */
        fs_read(fd, frame, sizeof(frame));

        /* Rewrite indices according to the map and the AND mask */
        for(y = 0; y < 32; y++) {
            for(x = 0; x < 16; x += 4) {
                fs_read(fd, &and_mask, sizeof(and_mask));

                for(j = 0; j < 4; j++) {
                    px = frame[y * 16 + x + j];
                    pxh = px >> 4;
                    pxl = px & 0x0f;

                    if(and_mask & BIT(7 - j * 2))
                        pxh = 15;
                    else
                        pxh = pal_map[pxh];
                    if(and_mask & BIT(6 - j * 2))
                        pxl = 15;
                    else
                        pxl = pal_map[pxl];

                    pkg->icon_data[i * 512 + 496 - 16 * y + x + j] = (pxh << 4) | pxl;
                }
            }
        }
    }

    vmu_pkg_load_palette(pkg, palette, nb_colors);

    fs_close(fd);

    return 0;

out_err_close:
    fs_close(fd);

    return -1;
}
