/*
 * This file is part of MPlayer.
 * Copyright (C) 2011 Rudolf Polzer <divVerent@xonotic.org>
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

#ifndef MPLAYER_ENCODE_LAVC_H
#define MPLAYER_ENCODE_LAVC_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>

#include "mp_core.h"

#include "options.h"

#define ENCODE_LAVC_FLAG_HARDDUP 1
#define ENCODE_LAVC_FLAG_COPYTS 2
#define ENCODE_LAVC_FLAG_NEVERDROP 4

struct encode_lavc_context;

struct encode_lavc_context *encode_lavc_init(struct MPContext *mpctx, struct encode_output_conf *options);
void encode_lavc_finish(struct encode_lavc_context *ctx);
AVStream *encode_lavc_alloc_stream(struct encode_lavc_context *ctx, enum AVMediaType mt);
void encode_lavc_write_stats(struct encode_lavc_context *ctx, AVStream *stream);
int encode_lavc_write_frame(struct encode_lavc_context *ctx, AVPacket *packet);
int encode_lavc_supports_pixfmt(struct encode_lavc_context *ctx, enum PixelFormat format);
AVCodec *encode_lavc_get_codec(struct encode_lavc_context *ctx, AVStream *stream);
int encode_lavc_open_codec(struct encode_lavc_context *ctx, AVStream *stream);
int encode_lavc_available(struct encode_lavc_context *ctx);
void encode_lavc_failtimesync(struct encode_lavc_context *ctx);
int encode_lavc_timesyncfailed(struct encode_lavc_context *ctx);
void encode_lavc_settimesync(struct encode_lavc_context *ctx, double a_minus_v, double dt);
double encode_lavc_gettimesync(struct encode_lavc_context *ctx, double initial_a_minus_v);
int encode_lavc_start(struct encode_lavc_context *ctx); // returns 1 on success
int encode_lavc_oformat_flags(struct encode_lavc_context *ctx);

enum encode_lavc_showhelp_type {
    ENCODE_LAVC_SHOWHELP_F,
    ENCODE_LAVC_SHOWHELP_FOPTS,
    ENCODE_LAVC_SHOWHELP_VC,
    ENCODE_LAVC_SHOWHELP_VCOPTS,
    ENCODE_LAVC_SHOWHELP_AC,
    ENCODE_LAVC_SHOWHELP_ACOPTS
};
void encode_lavc_showhelp(enum encode_lavc_showhelp_type t);
int encode_lavc_testflag(struct encode_lavc_context *ctx, int flag);
double encode_lavc_getoffset(struct encode_lavc_context *ctx, AVStream *stream);
const char *encode_lavc_getstatus(struct encode_lavc_context *ctx, float relative_position, float playback_time);

#endif
