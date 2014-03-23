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
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "config.h"
#include "mp_msg.h"
#include "options.h"

#include "img_format.h"
#include "mp_image.h"
#include "vf.h"
#include "sub/dec_sub.h"

#include "libvo/csputils.h"
#include "libvo/fastmemcpy.h"

#include "m_option.h"
#include "m_struct.h"

#include "sub/ass_mp.h"

#define _r(c)  ((c)>>24)
#define _g(c)  (((c)>>16)&0xFF)
#define _b(c)  (((c)>>8)&0xFF)
#define _a(c)  ((c)&0xFF)
#define from_rgb(c, m, max) \
    ( ((m)[COL_R]*_r(c)*max/255 + (m)[COL_G]*_g(c)*max/255 + \
       (m)[COL_B]*_b(c)*max/255 + (m)[COL_C]*max) )
#define dither() (rand() / (RAND_MAX + 1.0))


static const struct vf_priv_s {
    int outh, outw;

    unsigned int outfmt;
    struct mp_csp_details video_colorspace;

    // 1 = auto-added filter: insert only if chain does not support EOSD already
    // 0 = insert always
    int auto_insert;

    struct osd_state *osd;
    double aspect_correction;

    unsigned char *planes[3];
    struct line_limits {
        uint16_t start;
        uint16_t end;
    } *line_limits;
} vf_priv_dflt;

static int config(struct vf_instance *vf,
                  int width, int height, int d_width, int d_height,
                  unsigned int flags, unsigned int outfmt)
{
    struct MPOpts *opts = vf->opts;
    if (outfmt == IMGFMT_IF09)
        return 0;

    vf->priv->outfmt = outfmt;
    vf->priv->outh = height + opts->ass_top_margin + opts->ass_bottom_margin;
    vf->priv->outw = width;

    if (!opts->screen_size_x && !opts->screen_size_y) {
        d_width = d_width * vf->priv->outw / width;
        d_height = d_height * vf->priv->outh / height;
    }

    int bpp = IMGFMT_IS_YUVP16(outfmt) ? 2 : 1;
    vf->priv->planes[1]   = malloc(vf->priv->outw * vf->priv->outh * bpp);
    vf->priv->planes[2]   = malloc(vf->priv->outw * vf->priv->outh * bpp);
    vf->priv->line_limits = malloc((vf->priv->outh + 1) / 2 * sizeof(*vf->priv->line_limits));

    vf->priv->aspect_correction = (double)width / height * d_height / d_width;

    return vf_next_config(vf, vf->priv->outw, vf->priv->outh, d_width,
			  d_height, flags, outfmt);
}

static void get_image(struct vf_instance *vf, mp_image_t *mpi)
{
    if (mpi->type == MP_IMGTYPE_IPB)
        return;
    if (mpi->flags & MP_IMGFLAG_PRESERVE)
        return;
    if (mpi->imgfmt != vf->priv->outfmt)
        return;                                     // colorspace differ

    // width never changes, always try full DR
    mpi->priv = vf->dmpi = vf_get_image(vf->next, mpi->imgfmt, mpi->type,
                                        mpi->flags | MP_IMGFLAG_READABLE,
                                        vf->priv->outw, vf->priv->outh);

    if ((vf->dmpi->flags & MP_IMGFLAG_DRAW_CALLBACK) &&
        !(vf->dmpi->flags & MP_IMGFLAG_DIRECT)) {
        mp_tmsg(MSGT_ASS, MSGL_INFO, "Full DR not possible, trying SLICES instead!\n");
        return;
    }

    int tmargin = vf->opts->ass_top_margin;
    // set up mpi as a cropped-down image of dmpi:
    if (mpi->flags & MP_IMGFLAG_PLANAR) {
        mpi->planes[0] = vf->dmpi->planes[0] +  tmargin * vf->dmpi->stride[0];
        mpi->planes[1] = vf->dmpi->planes[1] + (tmargin >> mpi->chroma_y_shift) * vf->dmpi->stride[1];
        mpi->planes[2] = vf->dmpi->planes[2] + (tmargin >> mpi->chroma_y_shift) * vf->dmpi->stride[2];
        mpi->stride[1] = vf->dmpi->stride[1];
        mpi->stride[2] = vf->dmpi->stride[2];
    } else {
        mpi->planes[0] = vf->dmpi->planes[0] + tmargin * vf->dmpi->stride[0];
    }
    mpi->stride[0] = vf->dmpi->stride[0];
    mpi->width = vf->dmpi->width;
    mpi->flags |= MP_IMGFLAG_DIRECT;
    mpi->flags &= ~MP_IMGFLAG_DRAW_CALLBACK;
//	vf->dmpi->flags&=~MP_IMGFLAG_DRAW_CALLBACK;
}

static void blank(mp_image_t *mpi, int y1, int y2)
{
    vf_mpi_clear(mpi, 0, y1, mpi->w, y2 - y1);
}

static int prepare_image(struct vf_instance *vf, mp_image_t *mpi)
{
    struct MPOpts *opts = vf->opts;
    int tmargin = opts->ass_top_margin;
    if (mpi->flags & MP_IMGFLAG_DIRECT
	|| mpi->flags & MP_IMGFLAG_DRAW_CALLBACK) {
        vf->dmpi = mpi->priv;
        if (!vf->dmpi) {
            mp_tmsg(MSGT_ASS, MSGL_WARN, "Why do we get NULL??\n");
	    return 0;
        }
        mpi->priv = NULL;
        // we've used DR, so we're ready...
        if (tmargin)
            blank(vf->dmpi, 0, tmargin);
        if (opts->ass_bottom_margin)
            blank(vf->dmpi, vf->priv->outh - opts->ass_bottom_margin,
                  vf->priv->outh);
        if (!(mpi->flags & MP_IMGFLAG_PLANAR))
            vf->dmpi->planes[1] = mpi->planes[1];             // passthrough rgb8 palette
        return 0;
    }

    // hope we'll get DR buffer:
    vf->dmpi = vf_get_image(vf->next, vf->priv->outfmt, MP_IMGTYPE_TEMP,
			    MP_IMGFLAG_ACCEPT_STRIDE | MP_IMGFLAG_READABLE,
                            vf->priv->outw, vf->priv->outh);

    // copy mpi->dmpi...
    if (mpi->flags & MP_IMGFLAG_PLANAR) {
        int bpp = IMGFMT_IS_YUVP16(vf->dmpi->imgfmt) ? 2 : 1;
        memcpy_pic(vf->dmpi->planes[0] + tmargin * vf->dmpi->stride[0],
                   mpi->planes[0],
		   mpi->w * bpp,
		   mpi->h,
                   vf->dmpi->stride[0],
		   mpi->stride[0]);
        memcpy_pic(vf->dmpi->planes[1] + (tmargin >> mpi->chroma_y_shift) * vf->dmpi->stride[1],
                   mpi->planes[1],
		   (mpi->w >> mpi->chroma_x_shift) * bpp,
		   mpi->h >> mpi->chroma_y_shift,
                   vf->dmpi->stride[1],
		   mpi->stride[1]);
        memcpy_pic(vf->dmpi->planes[2] + (tmargin >> mpi->chroma_y_shift) * vf->dmpi->stride[2],
                   mpi->planes[2],
		   (mpi->w >> mpi->chroma_x_shift) * bpp,
		   mpi->h >> mpi->chroma_y_shift,
                   vf->dmpi->stride[2],
		   mpi->stride[2]);
    } else {
        memcpy_pic(vf->dmpi->planes[0] + tmargin * vf->dmpi->stride[0],
                   mpi->planes[0],
		   mpi->w * (vf->dmpi->bpp / 8),
		   mpi->h,
                   vf->dmpi->stride[0],
		   mpi->stride[0]);
        vf->dmpi->planes[1] = mpi->planes[1];   // passthrough rgb8 palette
    }
    if (tmargin)
        blank(vf->dmpi, 0, tmargin);
    if (opts->ass_bottom_margin)
        blank(vf->dmpi, vf->priv->outh - opts->ass_bottom_margin,
              vf->priv->outh);
    return 0;
}

static void update_limits(struct vf_instance *vf, int starty, int endy,
                          int startx, int endx)
{
    starty >>= 1;
    endy = (endy + 1) >> 1;
    startx >>= 1;
    endx = (endx + 1) >> 1;
    for (int i = starty; i < endy; i++) {
        struct line_limits *ll = vf->priv->line_limits + i;
        if (startx < ll->start)
            ll->start = startx;
        if (endx > ll->end)
            ll->end = endx;
    }
}

/**
 * \brief Copy specified rows from render_context.dmpi to render_context.planes, upsampling to 4:4:4
 */
static void copy_from_image(struct vf_instance *vf)
{
    int pl;

    for (pl = 1; pl < 3; ++pl) {
        if (IMGFMT_IS_YUVP16(vf->dmpi->imgfmt)) {
            int dst_stride = vf->priv->outw * 2;
            int src_stride = vf->dmpi->stride[pl];

            uint16_t *src = vf->dmpi->planes[pl];
            uint16_t *dst = vf->priv->planes[pl];
            for (int i = 0; i < (vf->priv->outh + 1) / 2; i++) {
                struct line_limits *ll = vf->priv->line_limits + i;
                uint16_t *dst_next = dst + dst_stride / 2;
                for (int j = ll->start; j < ll->end; j++) {
                    uint16_t val = src[j];
                    dst[j << 1] = val;
                    dst[(j << 1) + 1] = val;
                    dst_next[j << 1] = val;
                    dst_next[(j << 1) + 1] = val;
                }
                src += src_stride / 2;
                dst = dst_next + dst_stride / 2;
            }
        } else {
            int dst_stride = vf->priv->outw;
            int src_stride = vf->dmpi->stride[pl];

            unsigned char *src = vf->dmpi->planes[pl];
            unsigned char *dst = vf->priv->planes[pl];
            for (int i = 0; i < (vf->priv->outh + 1) / 2; i++) {
                struct line_limits *ll = vf->priv->line_limits + i;
                unsigned char *dst_next = dst + dst_stride;
                for (int j = ll->start; j < ll->end; j++) {
                    unsigned char val = src[j];
                    dst[j << 1] = val;
                    dst[(j << 1) + 1] = val;
                    dst_next[j << 1] = val;
                    dst_next[(j << 1) + 1] = val;
                }
                src += src_stride;
                dst = dst_next + dst_stride;
            }
        }
    }
}

/**
 * \brief Copy all previously copied rows back to render_context.dmpi
 */
static void copy_to_image(struct vf_instance *vf)
{
    int pl;
    int i, j;
    for (pl = 1; pl < 3; ++pl) {
        if (IMGFMT_IS_YUVP16(vf->dmpi->imgfmt)) {
            int dst_stride = vf->dmpi->stride[pl];
            int src_stride = vf->priv->outw * 2;

            uint16_t *dst      = vf->dmpi->planes[pl];
            uint16_t *src      = vf->priv->planes[pl];
            uint16_t *src_next = vf->priv->planes[pl] + src_stride;
            for (i = 0; i < vf->dmpi->chroma_height; ++i) {
                for (j = vf->priv->line_limits[i].start; j < vf->priv->line_limits[i].end; j++) {
                    unsigned val = 0;
                    val += src[j << 1];
                    val += src[(j << 1) + 1];
                    val += src_next[j << 1];
                    val += src_next[(j << 1) + 1];
                    dst[j] = val >> 2;
                }
                dst += dst_stride / 2;
                src      = src_next + src_stride / 2;
                src_next = src + src_stride / 2;
            }
        } else {
            int dst_stride = vf->dmpi->stride[pl];
            int src_stride = vf->priv->outw;

            unsigned char *dst      = vf->dmpi->planes[pl];
            unsigned char *src      = vf->priv->planes[pl];
            unsigned char *src_next = vf->priv->planes[pl] + src_stride;
            for (i = 0; i < vf->dmpi->chroma_height; ++i) {
                for (j = vf->priv->line_limits[i].start; j < vf->priv->line_limits[i].end; j++) {
                    unsigned val = 0;
                    val += src[j << 1];
                    val += src[(j << 1) + 1];
                    val += src_next[j << 1];
                    val += src_next[(j << 1) + 1];
                    dst[j] = val >> 2;
                }
                dst += dst_stride;
                src      = src_next + src_stride;
                src_next = src + src_stride;
            }
        }
    }
}

static void my_draw_bitmap(struct vf_instance *vf, unsigned char *bitmap,
			   int bitmap_w, int bitmap_h, int stride,
			   int dst_x, int dst_y, unsigned color, float rgb2yuv[3][4])
{
    // These should stay double to preserve precision in the inner loop
    double y = from_rgb(color, rgb2yuv[0], 255);
    double u = from_rgb(color, rgb2yuv[1], 255);
    double v = from_rgb(color, rgb2yuv[2], 255);
    unsigned char opacity = 255 - _a(color);
    unsigned char *src, *dsty, *dstu, *dstv;
    int i, j;
    mp_image_t *dmpi = vf->dmpi;

    src = bitmap;
    dsty = dmpi->planes[0] + dst_x + dst_y * dmpi->stride[0];
    dstu = vf->priv->planes[1] + dst_x + dst_y * vf->priv->outw;
    dstv = vf->priv->planes[2] + dst_x + dst_y * vf->priv->outw;
    for (i = 0; i < bitmap_h; ++i) {
        for (j = 0; j < bitmap_w; ++j) {
            unsigned k = src[j] * opacity;
            dsty[j] = (k * y + (65025 - k) * dsty[j]) / 65025 + dither();
            dstu[j] = (k * u + (65025 - k) * dstu[j]) / 65025 + dither();
            dstv[j] = (k * v + (65025 - k) * dstv[j]) / 65025 + dither();
        }
        src  += stride;
        dsty += dmpi->stride[0];
        dstu += vf->priv->outw;
        dstv += vf->priv->outw;
    }
}

static void my_draw_bitmap_16(struct vf_instance *vf, unsigned char *bitmap,
               int bitmap_w, int bitmap_h, int stride,
               int dst_x, int dst_y, unsigned color, float rgb2yuv[3][4])
{
    mp_image_t *dmpi = vf->dmpi;
    unsigned max = (1 << IMGFMT_YUVP16_DEPTH(dmpi->imgfmt)) - 1;
    // These should stay double to preserve precision in the inner loop
    double y = from_rgb(color, rgb2yuv[0], max);
    double u = from_rgb(color, rgb2yuv[1], max);
    double v = from_rgb(color, rgb2yuv[2], max);
    unsigned char opacity = 255 - _a(color);
    unsigned char *src;
    uint16_t *dsty, *dstu, *dstv;
    int i, j;

    src = bitmap;
    dsty = (uint16_t *)(dmpi->planes[0] + dst_x * 2 + dst_y * dmpi->stride[0]);
    dstu = (uint16_t *)vf->priv->planes[1] + dst_x + dst_y * vf->priv->outw;
    dstv = (uint16_t *)vf->priv->planes[2] + dst_x + dst_y * vf->priv->outw;
    for (i = 0; i < bitmap_h; ++i) {
        for (j = 0; j < bitmap_w; ++j) {
            unsigned k = src[j] * opacity;
            dsty[j] = (k * y + (65025 - k) * dsty[j]) / 65025 + dither();
            dstu[j] = (k * u + (65025 - k) * dstu[j]) / 65025 + dither();
            dstv[j] = (k * v + (65025 - k) * dstv[j]) / 65025 + dither();
        }
        src  += stride;
        dsty += dmpi->stride[0] / 2;
        dstu += vf->priv->outw;
        dstv += vf->priv->outw;
    }
}

static int render_frame(struct vf_instance *vf, mp_image_t *mpi,
			const ASS_Image *img, float rgb2yuv[3][4])
{
    if (img) {
        for (int i = 0; i < (vf->priv->outh + 1) / 2; i++)
            vf->priv->line_limits[i] = (struct line_limits){65535, 0};
        for (const ASS_Image *im = img; im; im = im->next)
            update_limits(vf, im->dst_y, im->dst_y + im->h,
			  im->dst_x, im->dst_x + im->w);
        copy_from_image(vf);
        while (img) {
            if (IMGFMT_IS_YUVP16(vf->dmpi->imgfmt))
                my_draw_bitmap_16(vf, img->bitmap, img->w, img->h, img->stride,
                                  img->dst_x, img->dst_y, img->color, rgb2yuv);
            else
                my_draw_bitmap(vf, img->bitmap, img->w, img->h, img->stride,
                               img->dst_x, img->dst_y, img->color, rgb2yuv);
            img = img->next;
        }
        copy_to_image(vf);
    }
    return 0;
}

static int put_image(struct vf_instance *vf, mp_image_t *mpi, double pts)
{
    struct vf_priv_s *priv = vf->priv;
    struct MPOpts *opts = vf->opts;
    struct osd_state *osd = priv->osd;
    ASS_Image *images = 0;
    float rgb2yuv[3][4];
    if (pts != MP_NOPTS_VALUE) {
        osd->dim = (struct mp_eosd_res){ .w = vf->priv->outw,
                                         .h = vf->priv->outh,
                                         .mt = opts->ass_top_margin,
                                         .mb = opts->ass_bottom_margin };
        osd->normal_scale = vf->priv->aspect_correction;
        osd->vsfilter_scale = 1;
        osd->sub_pts = pts + opts->sub_delay - osd->sub_offset;
        osd->support_rgba = false;
        struct sub_bitmaps b;
        sub_get_bitmaps(osd, &b);
        images = b.imgs;
        if (b.colorspace.format == MP_CSP_AUTO)
            b.colorspace = vf->priv->video_colorspace;
        struct mp_csp_params csp_params = { .colorspace = b.colorspace,
                                            .brightness = 0,
                                            .contrast = 1,
                                            .hue = 0,
                                            .saturation = 1 };
        if (IMGFMT_IS_YUVP16(vf->priv->outfmt)) {
            csp_params.texture_bits =
            csp_params.input_bits = IMGFMT_YUVP16_DEPTH(vf->priv->outfmt);
        } else
            csp_params.texture_bits = csp_params.input_bits = 8;
        mp_get_rgb2yuv_coeffs(&csp_params, rgb2yuv);
    }

    prepare_image(vf, mpi);
    render_frame(vf, mpi, images, rgb2yuv);

    return vf_next_put_image(vf, vf->dmpi, pts);
}

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
    switch (fmt) {
    case IMGFMT_YV12:
    case IMGFMT_I420:
    case IMGFMT_IYUV:
    case IMGFMT_420P9:
    case IMGFMT_420P10:
    case IMGFMT_420P16:
        return vf_next_query_format(vf, vf->priv->outfmt);
    }
    return 0;
}

static int control(vf_instance_t *vf, int request, void *data)
{
    switch (request) {
    case VFCTRL_SET_OSD_OBJ:
        vf->priv->osd = data;
        break;
    case VFCTRL_SET_YUV_COLORSPACE:
        vf->priv->video_colorspace = *(struct mp_csp_details *)data;
        break;
    case VFCTRL_INIT_EOSD:
        return CONTROL_TRUE;
    case VFCTRL_DRAW_EOSD:
        return CONTROL_TRUE;
    }
    return vf_next_control(vf, request, data);
}

static void uninit(struct vf_instance *vf)
{
    free(vf->priv->planes[1]);
    free(vf->priv->planes[2]);
    free(vf->priv->line_limits);
    free(vf->priv);
}

static const unsigned int fmt_list[] = {
    IMGFMT_YV12,
    IMGFMT_I420,
    IMGFMT_IYUV,
    IMGFMT_420P9,
    IMGFMT_420P10,
    IMGFMT_420P16,
    0
};

static int vf_open(vf_instance_t *vf, char *args)
{
    int flags;
    vf->priv->outfmt = vf_match_csp(&vf->next, fmt_list, IMGFMT_YV12);
    if (vf->priv->outfmt)
        flags = vf_next_query_format(vf, vf->priv->outfmt);
    if (!vf->priv->outfmt) {
        uninit(vf);
        return 0;
    } else if (vf->priv->auto_insert && flags & VFCAP_EOSD) {
        uninit(vf);
        return -1;
    }

    if (vf->priv->auto_insert)
        mp_msg(MSGT_ASS, MSGL_INFO, "[ass] auto-open\n");

    vf->config = config;
    vf->query_format = query_format;
    vf->uninit    = uninit;
    vf->control   = control;
    vf->get_image = get_image;
    vf->put_image = put_image;
    vf->default_caps = VFCAP_EOSD | VFCAP_EOSD_FILTER;
    return 1;
}

#define ST_OFF(f) M_ST_OFF(struct vf_priv_s, f)
static const m_option_t vf_opts_fields[] = {
    {"auto", ST_OFF(auto_insert), CONF_TYPE_FLAG, 0, 0, 1, NULL},
    {NULL, NULL, 0, 0, 0, 0, NULL}
};

static const m_struct_t vf_opts = {
    "ass",
    sizeof(struct vf_priv_s),
    &vf_priv_dflt,
    vf_opts_fields
};

const vf_info_t vf_info_ass = {
    "Render ASS/SSA subtitles",
    "ass",
    "Evgeniy Stepanov",
    "",
    vf_open,
    &vf_opts
};
