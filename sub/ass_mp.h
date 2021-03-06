/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
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
 * with libass; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPLAYER_ASS_MP_H
#define MPLAYER_ASS_MP_H

#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "subreader.h"

#include "libvo/csputils.h"

#ifdef CONFIG_ASS
#include <ass/ass.h>
#include <ass/ass_types.h>

struct MPOpts;
struct mp_eosd_res;

ASS_Track *mp_ass_default_track(ASS_Library *library, struct MPOpts *opts);
ASS_Track *mp_ass_read_subdata(ASS_Library *library, struct MPOpts *opts,
                               sub_data *subdata, double fps);
ASS_Track *mp_ass_read_stream(ASS_Library *library, struct MPOpts *opts,
                              const char *fname, char *charset);

struct MPOpts;
void mp_ass_configure(ASS_Renderer *priv, struct MPOpts *opts,
                      struct mp_eosd_res *dim, bool unscaled);
void mp_ass_configure_fonts(ASS_Renderer *priv);
ASS_Library *mp_ass_init(struct MPOpts *opts);

struct mp_csp_details mp_ass_get_colorspace(ASS_Track *track);

#else /* CONFIG_ASS */

/* Needed for EOSD code using this type to compile */

typedef struct ass_image {
    int w, h;
    int stride;
    unsigned char *bitmap;
    uint32_t color;
    int dst_x, dst_y;
    struct ass_image *next;
} ASS_Image;

#endif

#endif                          /* MPLAYER_ASS_MP_H */
