/* KallistiOS ##version##

   util/vmu_fb.c
   Copyright (C) 2023 Paul Cercueil
   Copyright (C) 2024 Falco Girgis

*/

#include <stdbool.h>
#include <string.h>

#include <dc/math.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_fb.h>

/* Linux 4x6 font: lib/fonts/font_mini_4x6.c
 *
 * Created by Kenneth Albanowski.
 * No rights reserved, released to the public domain.
 */
static const char fontdata_4x6[] = {
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0x00, 0x00, 0x00, 0x44, 0x40, 0x40, 0xaa, 0x00,
    0x00, 0xaf, 0xfa, 0x00, 0x46, 0xec, 0x40, 0xa2,
    0x48, 0xa0, 0x69, 0x6a, 0xd0, 0x24, 0x00, 0x00,
    0x24, 0x44, 0x20, 0x42, 0x22, 0x40, 0x0e, 0xee,
    0x00, 0x04, 0xe4, 0x00, 0x00, 0x04, 0x80, 0x00,
    0xe0, 0x00, 0x00, 0x00, 0x40, 0x02, 0x48, 0x00,
    0x4a, 0xaa, 0x40, 0x4c, 0x44, 0xe0, 0xc2, 0x48,
    0xe0, 0xe2, 0x62, 0xe0, 0xaa, 0xe2, 0x20, 0xe8,
    0xe2, 0xe0, 0xe8, 0xea, 0xe0, 0xe2, 0x22, 0x20,
    0xea, 0xea, 0xe0, 0xea, 0xe2, 0x20, 0x00, 0x40,
    0x40, 0x00, 0x40, 0x48, 0x24, 0x84, 0x20, 0x0e,
    0x0e, 0x00, 0x84, 0x24, 0x80, 0xe2, 0x60, 0x40,
    0x4e, 0xe8, 0x40, 0x4a, 0xea, 0xa0, 0xca, 0xca,
    0xc0, 0x68, 0x88, 0x60, 0xca, 0xaa, 0xc0, 0xe8,
    0xe8, 0xe0, 0xe8, 0xe8, 0x80, 0x68, 0xea, 0x60,
    0xaa, 0xea, 0xa0, 0xe4, 0x44, 0xe0, 0x22, 0x2a,
    0x40, 0xaa, 0xca, 0xa0, 0x88, 0x88, 0xe0, 0xae,
    0xea, 0xa0, 0xae, 0xee, 0xa0, 0x4a, 0xaa, 0x40,
    0xca, 0xc8, 0x80, 0x4a, 0xae, 0x60, 0xca, 0xec,
    0xa0, 0x68, 0x42, 0xc0, 0xe4, 0x44, 0x40, 0xaa,
    0xaa, 0x60, 0xaa, 0xa4, 0x40, 0xaa, 0xee, 0xa0,
    0xaa, 0x4a, 0xa0, 0xaa, 0x44, 0x40, 0xe2, 0x48,
    0xe0, 0x64, 0x44, 0x60, 0x08, 0x42, 0x00, 0x62,
    0x22, 0x60, 0x4a, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0x84, 0x00, 0x00, 0x00, 0x6a, 0xe0, 0x88, 0xca,
    0xc0, 0x00, 0x68, 0x60, 0x22, 0x6a, 0x60, 0x0e,
    0xe8, 0x60, 0x24, 0xe4, 0x40, 0x06, 0xa6, 0xe0,
    0x88, 0xca, 0xa0, 0x40, 0x44, 0x40, 0x40, 0x44,
    0x80, 0x08, 0xac, 0xa0, 0x0c, 0x44, 0xe0, 0x00,
    0xee, 0xa0, 0x00, 0xca, 0xa0, 0x04, 0xaa, 0x40,
    0x00, 0xca, 0xc8, 0x00, 0x6a, 0x62, 0x0c, 0xa8,
    0x80, 0x06, 0xc2, 0xc0, 0x04, 0xe4, 0x40, 0x00,
    0xaa, 0x60, 0x00, 0xae, 0x40, 0x00, 0xae, 0xe0,
    0x00, 0xa4, 0xa0, 0x00, 0xae, 0x2c, 0x0e, 0x6c,
    0xe0, 0x24, 0xc4, 0x20, 0x44, 0x44, 0x40, 0x84,
    0x64, 0x80, 0x5a, 0x00, 0x00, 0x4a, 0xae, 0x00,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0x06, 0xc6, 0x00, 0x0c, 0x6c, 0x00,
    0x82, 0x82, 0x82, 0xa5, 0xa5, 0xa5, 0xdb, 0xdb,
    0xdb, 0x44, 0x44, 0x44, 0x44, 0xc4, 0x44, 0x44,
    0xcc, 0x44, 0x66, 0xe6, 0x66, 0x00, 0xe6, 0x66,
    0x00, 0xcc, 0x44, 0x66, 0xee, 0x66, 0x66, 0x66,
    0x66, 0x00, 0xee, 0x66, 0x66, 0xee, 0x00, 0x66,
    0xe0, 0x00, 0x44, 0xcc, 0x00, 0x00, 0xc4, 0x44,
    0x44, 0x70, 0x00, 0x44, 0xf0, 0x00, 0x00, 0xf4,
    0x44, 0x44, 0x74, 0x44, 0x00, 0xf0, 0x00, 0x44,
    0xf4, 0x44, 0x44, 0x77, 0x44, 0x66, 0x76, 0x66,
    0x66, 0x77, 0x00, 0x00, 0x77, 0x66, 0x66, 0xff,
    0x00, 0x00, 0xff, 0x66, 0x66, 0x77, 0x66, 0x00,
    0xff, 0x00, 0x66, 0xff, 0x66, 0x44, 0xff, 0x00,
    0x66, 0xf0, 0x00, 0x00, 0xff, 0x44, 0x00, 0xf6,
    0x66, 0x66, 0x70, 0x00, 0x44, 0x77, 0x00, 0x00,
    0x77, 0x44, 0x00, 0x76, 0x66, 0x66, 0xf6, 0x66,
    0x44, 0xff, 0x44, 0x44, 0xc0, 0x00, 0x00, 0x74,
    0x44, 0xff, 0xff, 0xff, 0x00, 0x0f, 0xff, 0xcc,
    0xcc, 0xcc, 0x33, 0x33, 0x33, 0xff, 0xf0, 0x00,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0,
    0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee,
    0xe0, 0xee, 0xee, 0xe0, 0xee, 0xee, 0xe0, 0xee,
    0xee, 0xe0, 0x00, 0x66, 0x00, 0xee, 0xee, 0xe0,
};

static const vmufb_font_t vmufb_font4x6 = {
    .id = 0,
    .w = 4,
    .h = 6,
    .stride = 3,
    .fontdata = fontdata_4x6,
};

static const vmufb_font_t *default_font = &vmufb_font4x6;

static uint64_t extract_bits(const uint8_t *data,
                             unsigned int offt, unsigned int w) {
    uint32_t tmp, lsb, nb_bits;
    uint64_t bits = 0;

    /* This algorithm will extract "w" bits starting from bit "offt", and
       place them right-adjusted in "bits".

       Since we manipulate 8 bits at a time, and neither "w" nor "offt" are
       required to be byte-aligned, we need to compute a mask of valid bits
       for each byte that is processed. */
    while(w) {
        tmp = data[offt / 8];

        if(8 - (offt & 0x7) > w)
            lsb = 8 - (offt & 0x7) - w;
        else
            lsb = 0;

        nb_bits = 8 - (offt & 0x7) - lsb;
        bits <<= nb_bits;

        tmp &= GENMASK(7 - (offt & 0x7), lsb);

        bits |= tmp >> lsb;

        offt += nb_bits;
        w -= nb_bits;
    }

    return bits;
}

static void insert_bits(uint8_t *data,
                        unsigned int offt, unsigned int w, uint64_t bits) {
    uint32_t tmp, lsb, nb_bits, mask;

    while(w) {
        tmp = data[offt / 8];

        if(8 - (offt & 0x7) > w)
            lsb = 8 - (offt & 0x7) - w;
        else
            lsb = 0;

        nb_bits = 8 - (offt & 0x7) - lsb;
        mask = GENMASK(7 - (offt & 0x7), lsb);
        tmp &= ~mask;

        tmp |= ((bits >> (w - nb_bits)) << lsb) & mask;

        data[offt / 8] = tmp;

        offt += nb_bits;
        w -= nb_bits;
    }
}

static void vmufb_paint_area_strided(vmufb_t *fb,
                                     unsigned int x, unsigned int y,
                                     unsigned int w, unsigned int h,
                                     unsigned int stride, const uint8_t *data) {
    unsigned int i;
    uint64_t bits;

    for(i = 0; i < h; i++) {
        bits = extract_bits(data, i * stride, w);
        insert_bits((uint8_t *)fb->data, (y + i) * VMU_SCREEN_WIDTH + x, w, bits);
    }
}

void vmufb_paint_area(vmufb_t *fb,
                      unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h,
                      const char *data) {
    vmufb_paint_area_strided(fb, x, y, w, h, w, (const uint8_t *)data);
}

void vmufb_paint_xbm(vmufb_t *fb,
                     unsigned int x, unsigned int y,
                     unsigned int w, unsigned int h,
                     const uint8_t *xbm_data) {
    uint8_t buf[48 * 32];
    unsigned int i, wb = (w + 7) / 8;

    for(i = 0; i < h * wb; i++)
        buf[i] = bit_reverse(xbm_data[i]) >> 24;

    vmufb_paint_area_strided(fb, x, y, w, h, wb * 8, buf);
}

void vmufb_clear(vmufb_t *fb) {
    memset(fb->data, 0, sizeof(fb->data));
}

void vmufb_clear_area(vmufb_t *fb,
                      unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h) {
    uint32_t tmp[VMU_SCREEN_WIDTH] = {};

    vmufb_paint_area(fb, x, y, w, h, (const char *) tmp);
}

void vmufb_present(const vmufb_t *fb, maple_device_t *dev) {
    /* Check for controller containing VMU (should always be same port, unit 0) */
    maple_device_t *cont = maple_enum_dev(dev->port, 0);

    /* If the VMU connector and controller connector face opposite directions, 
       no flipping necessary (example: VMU in a lightgun). */
    if(cont && (cont->info.functions & MAPLE_FUNC_CONTROLLER) &&
       (cont->info.connector_direction != dev->info.connector_direction))
        vmu_draw_lcd(dev, fb->data);
    
    /* If we somehow found no corresponding controller, or connectors face the same direction,
       we rotate the image 180 degrees (example: VMU in a standard controller). */
    else 
        vmu_draw_lcd_rotated(dev, fb->data);
}

void vmufb_print_string_into(vmufb_t *fb,
                             const vmufb_font_t *font,
                             unsigned int x, unsigned int y,
                             unsigned int w, unsigned int h,
                             unsigned int line_spacing,
                             const char *str) {
    unsigned int xorig = x, yorig = y;
    font = font? font : default_font;

    for(; *str; str++) {
        switch (*str) {
        case '\n':
            x = xorig;
            y += line_spacing + font->h;
            continue;
        default:
            break;
        }

        if(x > xorig + w - font->w) {
            /* We run out of horizontal space - put character on new line */
            x = xorig;
            y += line_spacing + font->h;
        }

        if(y > yorig + h - font->h) {
            /* We ran out of space */
            break;
        }

        vmufb_paint_area(fb, x, y, font->w, font->h,
                         &font->fontdata[*str * font->stride]);

        x += font->w;
    }
}

const vmufb_font_t *vmu_set_font(const vmufb_font_t *font) {
    const vmufb_font_t *temp = default_font;
    default_font = (font) ? font : &vmufb_font4x6;

    return temp;
}

const vmufb_font_t *vmu_get_font(void) {
    return default_font;
}
