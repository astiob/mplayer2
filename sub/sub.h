/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPLAYER_SUB_H
#define MPLAYER_SUB_H

#include <stdbool.h>

#include "subreader.h"
#include "dec_sub.h"

typedef struct mp_osd_bbox_s {
    int x1,y1,x2,y2;
} mp_osd_bbox_t;

#define OSDTYPE_OSD 1
#define OSDTYPE_SUBTITLE 2
#define OSDTYPE_PROGBAR 3
#define OSDTYPE_SPU 4
#define OSDTYPE_DVDNAV 5
#define OSDTYPE_TELETEXT 6

#define OSDFLAG_VISIBLE 1
#define OSDFLAG_CHANGED 2
#define OSDFLAG_BBOX 4
#define OSDFLAG_OLD_BBOX 8
#define OSDFLAG_FORCE_UPDATE 16

#define MAX_UCS 1600
#define MAX_UCSLINES 16

typedef struct mp_osd_obj_s {
    struct mp_osd_obj_s* next;
    unsigned char type;
    unsigned char alignment; // 2 bits: x;y percentages, 2 bits: x;y relative to parent; 2 bits: alignment left/right/center
    unsigned short flags;
    int x,y;
    int dxs,dys;
    mp_osd_bbox_t bbox; // bounding box
    mp_osd_bbox_t old_bbox; // the renderer will save bbox here
    union {
	struct {
	    void* sub;			// value of vo_sub at last update
	    int utbl[MAX_UCS+1];	// subtitle text
	    int xtbl[MAX_UCSLINES];	// x positions
	    int lines;			// no. of lines
	} subtitle;
	struct {
	    int elems;
	} progbar;
    } params;
    int stride;

    int allocated;
    unsigned char *alpha_buffer;
    unsigned char *bitmap_buffer;

    struct ass_track *osd_track;
} mp_osd_obj_t;

extern subtitle* vo_sub;

extern void* vo_osd_teletext_page;
extern int vo_osd_teletext_half;
extern int vo_osd_teletext_mode;
extern int vo_osd_teletext_format;

extern int vo_osd_progbar_type;
extern int vo_osd_progbar_value;   // 0..255

extern void* vo_spudec;
extern void* vo_vobsub;

#define OSD_PLAY 0x01
#define OSD_PAUSE 0x02
#define OSD_STOP 0x03
#define OSD_REW 0x04
#define OSD_FFW 0x05
#define OSD_CLOCK 0x06
#define OSD_CONTRAST 0x07
#define OSD_SATURATION 0x08
#define OSD_VOLUME 0x09
#define OSD_BRIGHTNESS 0x0A
#define OSD_HUE 0x0B
#define OSD_BALANCE 0x0C
#define OSD_PANSCAN 0x50

#define OSD_PB_START 0x10
#define OSD_PB_0 0x11
#define OSD_PB_END 0x12
#define OSD_PB_1 0x13

/* now in textform */
extern char * const sub_osd_names[];
extern char * const sub_osd_names_short[];

extern char *sub_cp;
extern int sub_pos;
extern int sub_width_p;
extern int sub_alignment;
extern int sub_bg_color; /* subtitles background color */
extern int sub_bg_alpha;
extern int spu_alignment;
extern int spu_aamode;
extern float spu_gaussvar;

extern char *subtitle_font_encoding;
extern float text_font_scale_factor;
extern float osd_font_scale_factor;
extern float subtitle_font_radius;
extern float subtitle_font_thickness;
extern int subtitle_autoscale;

extern char *font_name;
extern char *sub_font_name;
extern float font_factor;
extern float sub_fps;

extern int sub_justify;

void osd_draw_text(struct osd_state *osd, int dxs, int dys,
                   void (*draw_alpha)(void *ctx, int x0, int y0, int w, int h,
                                      unsigned char* src, unsigned char *srca,
                                      int stride),
                   void *ctx);
void osd_draw_text_ext(struct osd_state *osd, int dxs, int dys,
                       int left_border, int top_border, int right_border,
                       int bottom_border, int orig_w, int orig_h,
                       void (*draw_alpha)(void *ctx, int x0, int y0, int w,
                                          int h, unsigned char* src,
                                          unsigned char *srca,
                                          int stride),
                       void *ctx);

struct osd_state *osd_create(struct MPOpts *opts, struct ass_library *asslib);
void osd_set_text(struct osd_state *osd, const char *text);
int osd_update(struct osd_state *osd, int dxs, int dys);
int vo_osd_changed(int new_value);
int vo_osd_check_range_update(int,int,int,int);
void osd_free(struct osd_state *osd);

#ifdef CONFIG_DVDNAV
#include <inttypes.h>
void osd_set_nav_box (uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey);
#endif


// used only by osd_libass.c
void osd_alloc_buf(mp_osd_obj_t* obj);

// defined in osd_libass.c or osd_dummy.c
void vo_update_text_osd(struct osd_state *osd, mp_osd_obj_t *obj);
void vo_update_text_teletext(struct osd_state *osd, mp_osd_obj_t *obj);
void vo_update_text_progbar(struct osd_state *osd, mp_osd_obj_t *obj);
void vo_update_text_sub(struct osd_state *osd, mp_osd_obj_t *obj);
void osd_get_function_sym(char *buffer, size_t buffer_size, int osd_function);
void osd_init_backend(struct osd_state *osd);
void osd_destroy_backend(struct osd_state *osd);

#endif /* MPLAYER_SUB_H */
