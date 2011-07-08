/*
 * Raw video muxing using libavformat
 * Copyright (C) 2010 Nicolas George <george@nsup.org>
 * Copyright (C) 2011 Rudolf Polzer <divVerent@xonotic.org>
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

#include <stdio.h>
#include <stdlib.h>
#include "mpcommon.h"
#include "fmt-conversion.h"
#include "libmpcodecs/mp_image.h"
#include "libmpcodecs/vfcap.h"
#include "subopt-helper.h"
#include "talloc.h"
#include "video_out.h"

#include "encode_lavc.h"

struct priv {
    struct encode_lavc_context *encode_ctx;
    uint8_t *buffer;
    size_t buffer_size;
    AVStream *stream;
    int have_first_packet;

    int harddup;

    int64_t lastpts;
    int64_t lastframepts;
    int64_t lastframepts_stream;
    mp_image_t *lastimg;
    int lastdisplaycount;

    AVRational worst_time_base;
    int worst_time_base_is_stream;
};

static int preinit(struct vo *vo, const char *arg)
{
    struct priv *vc;
    if (!encode_lavc_available(vo->encode_lavc_ctx)) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: the option -o (output file) must be specified\n");
        return -1;
    }
    vo->priv = talloc_zero(vo, struct priv);
    vc = vo->priv;
    vc->harddup = encode_lavc_testflag(vo->encode_lavc_ctx,
                                       ENCODE_LAVC_FLAG_HARDDUP);
    return 0;
}

static void draw_image(struct vo *vo, mp_image_t *mpi, double pts);
static void uninit(struct vo *vo)
{
    struct priv *vc = vo->priv;
    if (!vc)
        return;

    if(vc->lastpts >= 0 && vc->stream)
        draw_image(vo, NULL, MP_NOPTS_VALUE);

    if(vc->lastimg) {
        // palette hack
        if (vc->lastimg->imgfmt == IMGFMT_RGB8 || vc->lastimg->imgfmt == IMGFMT_BGR8) {
            vc->lastimg->planes[1] = NULL;
        }
        free_mp_image(vc->lastimg);
        vc->lastimg = NULL;
    }

    vo->priv = NULL;
}

static int config(struct vo *vo, uint32_t width, uint32_t height, uint32_t d_width,
        uint32_t d_height, uint32_t flags, char *title,
        uint32_t format)
{
    struct priv *vc = vo->priv;
    enum PixelFormat pix_fmt = imgfmt2pixfmt(format);
    AVRational display_aspect_ratio, image_aspect_ratio;
    AVRational aspect;

    if (!vc)
        return -1;

    display_aspect_ratio.num = d_width;
    display_aspect_ratio.den = d_height;
    image_aspect_ratio.num = width;
    image_aspect_ratio.den = height;
    aspect = av_div_q(display_aspect_ratio, image_aspect_ratio);

    if (vc->stream) {
        /* NOTE:
           in debug builds we get a "comparison between signed and unsigned"
           warning here. We choose to ignore that; just because ffmpeg currently
           uses a plain 'int' for these struct fields, it doesn't mean it always
           will */
        if (width == vc->stream->codec->width && height == vc->stream->codec->height) {
            if (aspect.num != vc->stream->codec->sample_aspect_ratio.num || aspect.den != vc->stream->codec->sample_aspect_ratio.den) {
                /* aspect-only changes are not critical */
                mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: unsupported pixel aspect ratio change from %d:%d to %d:%d\n", vc->stream->codec->sample_aspect_ratio.num, vc->stream->codec->sample_aspect_ratio.den, aspect.num, aspect.den);
            }
            return 0;
        }

        /* FIXME Is it possible with raw video? */
        mp_msg(MSGT_VO, MSGL_ERR,
                "vo-lavc: resolution changes not supported.\n");
        goto error;
    }

    vc->lastpts = MP_NOPTS_VALUE;
    vc->lastframepts = MP_NOPTS_VALUE;
    vc->lastframepts_stream = MP_NOPTS_VALUE;

    if (pix_fmt == PIX_FMT_NONE)
        goto error; /* imgfmt2pixfmt already prints something */

    vc->stream = encode_lavc_alloc_stream(vo->encode_lavc_ctx,
                                          AVMEDIA_TYPE_VIDEO);
    vc->stream->sample_aspect_ratio = vc->stream->codec->sample_aspect_ratio = aspect;
    vc->stream->codec->width = width;
    vc->stream->codec->height = height;
    vc->stream->codec->pix_fmt = pix_fmt;

    if (encode_lavc_open_codec(vo->encode_lavc_ctx, vc->stream) < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: unable to open encoder\n");
        goto error;
    }

    vc->buffer_size = 6 * width * height + 200;
    if (vc->buffer_size < FF_MIN_BUFFER_SIZE)
        vc->buffer_size = FF_MIN_BUFFER_SIZE;
    if (vc->buffer_size < sizeof(AVPicture))
        vc->buffer_size = sizeof(AVPicture);

    vc->buffer = talloc_size(vc, vc->buffer_size);

    vc->lastimg = alloc_mpi(width, height, format);

    // palette hack
    if (vc->lastimg->imgfmt == IMGFMT_RGB8 || vc->lastimg->imgfmt == IMGFMT_BGR8)
        vc->lastimg->planes[1] = talloc_zero_size(vc, 1024);

    return 0;

error:
    uninit(vo);
    return -1;
}

static int query_format(struct vo *vo, uint32_t format)
{
    enum PixelFormat pix_fmt = imgfmt2pixfmt(format);

    if (!vo->encode_lavc_ctx)
        return 0;

    return encode_lavc_supports_pixfmt(vo->encode_lavc_ctx, pix_fmt) ?
        VFCAP_CSP_SUPPORTED : 0;
}

static void write_packet(struct vo *vo, int size)
{
    struct priv *vc = vo->priv;
    AVPacket packet;

    if (size < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: error encoding\n");
        return;
    }

    if (size > 0) {
        av_init_packet(&packet);
        packet.stream_index = vc->stream->index;
        packet.data = vc->buffer;
        packet.size = size;
        if (vc->stream->codec->coded_frame->key_frame)
            packet.flags |= AV_PKT_FLAG_KEY;
        if (vc->stream->codec->coded_frame->pts != AV_NOPTS_VALUE) {
            packet.pts = av_rescale_q(vc->stream->codec->coded_frame->pts, vc->stream->codec->time_base, vc->stream->time_base);
        } else {
            mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: codec did not provide pts\n");
            packet.pts = av_rescale_q(vc->lastpts, vc->worst_time_base, vc->stream->time_base);
        }

        // HACK: libavformat calculates dts wrong if the initial packet
        // duration is not set, but ONLY if the time base is "high" and if we
        // have b-frames!
        if (!vc->have_first_packet)
            if (vc->stream->codec->has_b_frames || vc->stream->codec->max_b_frames)
                if (vc->stream->time_base.num*1000LL <= vc->stream->time_base.den)
                    packet.duration = max(1, av_rescale_q(1, vc->stream->codec->time_base, vc->stream->time_base));

        if (encode_lavc_write_frame(vo->encode_lavc_ctx, &packet) < 0) {
            mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: error writing\n");
            return;
        }

        vc->have_first_packet = 1;
    }
}

static int encode_video(struct vo *vo, AVFrame *frame)
{
    struct priv *vc = vo->priv;
    if (encode_lavc_oformat_flags(vo->encode_lavc_ctx) & AVFMT_RAWPICTURE) {
        if (!frame)
            return 0;
        memcpy(vc->buffer, frame, sizeof(AVPicture));
        mp_msg(MSGT_AO, MSGL_DBG2, "vo-lavc: got pts %f\n", frame->pts * (double) vc->stream->codec->time_base.num / (double) vc->stream->codec->time_base.den);
        return sizeof(AVPicture);
    } else {
        int size = avcodec_encode_video(vc->stream->codec, vc->buffer, vc->buffer_size, frame);
        if (frame)
            mp_msg(MSGT_AO, MSGL_DBG2, "vo-lavc: got pts %f; out size: %d\n", frame->pts * (double) vc->stream->codec->time_base.num / (double) vc->stream->codec->time_base.den, size);
        encode_lavc_write_stats(vo->encode_lavc_ctx, vc->stream);
        return size;
    }
}

static void draw_image(struct vo *vo, mp_image_t *mpi, double pts)
{
    struct priv *vc = vo->priv;
    int i, size;
    AVFrame frame;
    AVCodecContext *avc;
    int64_t framepts;

    if (pts == MP_NOPTS_VALUE)
        pts = vo_pts / 90000.0;

    if (!vc)
        return;
    if (!encode_lavc_start(vo->encode_lavc_ctx))
        return;
    if (encode_lavc_timesyncfailed(vo->encode_lavc_ctx)) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: Frame got dropped entirely because time sync did not run yet\n");
        return;
    }

    avc = vc->stream->codec;

    if (vc->worst_time_base.den == 0) {
        //if (avc->time_base.num / avc->time_base.den >= vc->stream->time_base.num / vc->stream->time_base.den)
        if (avc->time_base.num * (double) vc->stream->time_base.den >= vc->stream->time_base.num * (double) avc->time_base.den) {
            mp_msg(MSGT_VO, MSGL_V, "vo-lavc: NOTE: using codec time base (%d/%d) for frame dropping; the stream base (%d/%d) is not worse.\n", (int)avc->time_base.num, (int)avc->time_base.den, (int)vc->stream->time_base.num, (int)vc->stream->time_base.den);
            vc->worst_time_base = avc->time_base;
            vc->worst_time_base_is_stream = 0;
        } else {
            mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: NOTE: not using codec time base (%d/%d) for frame dropping; the stream base (%d/%d) is worse.\n", (int)avc->time_base.num, (int)avc->time_base.den, (int)vc->stream->time_base.num, (int)vc->stream->time_base.den);
            vc->worst_time_base = vc->stream->time_base;
            vc->worst_time_base_is_stream = 1;
        }

        // NOTE: we use the following "axiom" of av_rescale_q:
        // if time base A is worse than time base B, then
        //   av_rescale_q(av_rescale_q(x, A, B), B, A) == x
        // this can be proven as long as av_rescale_q rounds to nearest, which
        // it currently does

        // av_rescale_q(x, A, B) * B = "round x*A to nearest multiple of B"
        // and:
        //    av_rescale_q(av_rescale_q(x, A, B), B, A) * A
        // == "round av_rescale_q(x, A, B)*B to nearest multiple of A"
        // == "round 'round x*A to nearest multiple of B' to nearest multiple of A"
        //
        // assume this fails. Then there is a value of x*A, for which the
        // nearest multiple of B is outside the range [(x-0.5)*A, (x+0.5)*A[.
        // Absurd, as this range MUST contain at least one multiple of B.
    }

    // vc->lastpts is MP_NOPTS_VALUE, or the start time of vc->lastframe
    if (mpi) {
        framepts = floor((pts + encode_lavc_gettimesync(vo->encode_lavc_ctx,
                -pts) + encode_lavc_getoffset(vo->encode_lavc_ctx, vc->stream))
                * vc->worst_time_base.den / vc->worst_time_base.num + 0.5);
    } else {
        if (vc->lastpts == MP_NOPTS_VALUE)
            framepts = 0;
        else
            framepts = vc->lastpts + 1;
    }

    // never-drop mode
    if (encode_lavc_testflag(vo->encode_lavc_ctx, ENCODE_LAVC_FLAG_NEVERDROP)
            && framepts <= vc->lastpts) {
        mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: -oneverdrop increased pts by %d\n", (int) (vc->lastpts - framepts + 1));
        framepts = vc->lastpts + 1;
    }

    if (vc->lastpts != MP_NOPTS_VALUE) {
        avcodec_get_frame_defaults(&frame);

        // we have a valid image in lastimg
        while (vc->lastpts < framepts) {
            int64_t thisduration = (vc->harddup ? 1 : (framepts - vc->lastpts));

            avcodec_get_frame_defaults(&frame);

            // this is a nop, unless the worst time base is the STREAM time base
            frame.pts = av_rescale_q(vc->lastpts, vc->worst_time_base, avc->time_base);

            for (i = 0; i < 4; i++) {
                frame.data[i] = vc->lastimg->planes[i];
                frame.linesize[i] = vc->lastimg->stride[i];
            }
            frame.quality = vc->stream->quality;

            size = encode_video(vo, &frame);
            write_packet(vo, size);

            vc->lastpts += thisduration;
            ++vc->lastdisplaycount;
        }
    }

    if (!mpi) {
        // finish encoding
        do {
            size = encode_video(vo, NULL);
            write_packet(vo, size);
        } while (size > 0);
    } else {
        if (framepts >= vc->lastframepts) {
            if (vc->lastframepts != MP_NOPTS_VALUE && vc->lastdisplaycount != 1)
                mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: Frame at pts %d got displayed %d times\n", (int) vc->lastframepts, vc->lastdisplaycount);
            copy_mpi(vc->lastimg, mpi);

            // palette hack
            if (vc->lastimg->imgfmt == IMGFMT_RGB8 || vc->lastimg->imgfmt == IMGFMT_BGR8)
                memcpy(vc->lastimg->planes[1], mpi->planes[1], 1024);

            vc->lastframepts = vc->lastpts = framepts;
            if (encode_lavc_testflag(vo->encode_lavc_ctx,
                    ENCODE_LAVC_FLAG_COPYTS) && vc->lastpts < 0)
                vc->lastpts = -1;
            vc->lastdisplaycount = 0;
        } else {
            mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: Frame at pts %d got dropped entirely because pts went backwards\n", (int) framepts);
        }
    }
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    switch (request) {
        case VOCTRL_QUERY_FORMAT:
            return query_format(vo, *((uint32_t*)data));
        case VOCTRL_DRAW_IMAGE:
            draw_image(vo, (mp_image_t *)data, vo->next_pts);
            return 0;
    }
    return VO_NOTIMPL;
}

static void draw_osd(struct vo *vo, struct osd_state *osd)
{
}

static void flip_page_timed(struct vo *vo, unsigned int pts_us, int duration)
{
}

static void check_events(struct vo *vo)
{
}

const struct vo_driver video_out_lavc = {
    .is_new = true,
    .buffer_frames = false,
    .info = &(const struct vo_info_s){
        "video encoding using libavcodec",
        "lavc",
        "Nicolas George <george@nsup.org>, Rudolf Polzer <divVerent@xonotic.org>",
        ""
    },
    .preinit = preinit,
    .config = config,
    .control = control,
    .uninit = uninit,
    .check_events = check_events,
    .draw_osd = draw_osd,
    .flip_page_timed = flip_page_timed,
};

// vim: sw=4 ts=4 et
