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


#include "encode_lavc.h"
#include "mp_msg.h"
#include "libmpcodecs/vfcap.h"
#include "options.h"
#include "osdep/timer.h"
#include "libvo/video_out.h"
#include "talloc.h"
#include "stream/stream.h"

static int set_to_avdictionary(void *ctx, AVDictionary **dictp, void *octx,
                               const char *str, const char *key_val_sep,
                               const char *pairs_sep)
{
    int good = 0;
    int errorcode = 0;
    const AVOption *o;

    while (*str) {
        char *key = av_get_token(&str, key_val_sep);
        char *val;

        if (*key && strspn(str, key_val_sep)) {
            str++;
            val = av_get_token(&str, pairs_sep);
        } else {
            av_log(ctx, AV_LOG_ERROR, "Missing key or no key/value "
                   "separator found after key '%s'\n", key);
            av_free(key);
            if (!errorcode)
                errorcode = AVERROR(EINVAL);
            if (*str)
                ++str;
            continue;
        }

        av_log(ctx, AV_LOG_DEBUG, "Setting value '%s' for key '%s'\n",
               val, key);

        if((o = av_opt_find(octx, key, NULL, 0, AV_OPT_SEARCH_CHILDREN))) {
            if (av_dict_set(dictp, key, *val ? val : NULL, (o->type == FF_OPT_TYPE_FLAGS && (val[0] == '+' || val[0] == '-')) ? AV_DICT_APPEND : 0) >= 0)
                ++good;
            else
                errorcode = AVERROR(EINVAL);
        } else {
            errorcode = AVERROR(ENOENT);
        }

        av_free(key);
        av_free(val);

        if (*str)
            ++str;
    }
    return errorcode ? errorcode : good;
}

static bool value_has_flag(const char *value, const char *flag)
{
    bool state = true;
    bool ret = false;
    while(*value)
    {
        size_t l = strcspn(value, "+-");
        if(l == 0)
        {
            state = (*value == '+');
            ++value;
        }
        else
        {
            if(l == strlen(flag))
                if(!memcmp(value, flag, l))
                    ret = state;
            value += l;
        }
    }
    return ret;
}

int encode_lavc_available(struct encode_lavc_context *ctx)
{
    return ctx && ctx->avc;
}

int encode_lavc_oformat_flags(struct encode_lavc_context *ctx)
{
    return ctx->avc ? ctx->avc->oformat->flags : 0;
}

struct encode_lavc_context *encode_lavc_init(struct encode_output_conf *options)
{
    struct encode_lavc_context *ctx;

    if (!options->file)
        return NULL;

    ctx = talloc_zero(NULL, struct encode_lavc_context);
    encode_lavc_discontinuity(ctx);
    ctx->options = options;

    ctx->avc = avformat_alloc_context();

    if (!(ctx->avc->oformat = av_guess_format(ctx->options->format,
                                              ctx->options->file, NULL))) {
        mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: format not found\n");
        encode_lavc_finish(ctx);
        abort(); // XXXXXXXXXXXXXXXXXXXXXXX
        return NULL;
    }

    av_strlcpy(ctx->avc->filename, ctx->options->file,
               sizeof(ctx->avc->filename));

    ctx->foptions = NULL;
    if (ctx->options->fopts) {
        char **p;
        for (p = ctx->options->fopts; *p; ++p) {
            if (set_to_avdictionary(ctx->avc, &ctx->foptions, ctx->avc, *p, "=", "")
                    <= 0)
                mp_msg(MSGT_VO, MSGL_WARN,
                       "encode-lavc: could not set option %s\n", *p);
        }
    }

    if (ctx->options->vcodec) {
        ctx->vc = avcodec_find_encoder_by_name(ctx->options->vcodec);
        if (!ctx->vc) {
            mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: video codec not found\n");
            encode_lavc_finish(ctx);
            abort(); // XXXXXXXXXXXXXXXXXXXXXXX
            return NULL;
        }
    } else
        ctx->vc = avcodec_find_encoder(av_guess_codec(ctx->avc->oformat, NULL,
                               ctx->avc->filename, NULL, AVMEDIA_TYPE_VIDEO));

    if (ctx->options->acodec) {
        ctx->ac = avcodec_find_encoder_by_name(ctx->options->acodec);
        if (!ctx->ac) {
            mp_msg(MSGT_VO, MSGL_ERR, "ao-lavc: audio codec not found\n");
            encode_lavc_finish(ctx);
            abort(); // XXXXXXXXXXXXXXXXXXXXXXX
            return NULL;
        }
    } else
        ctx->ac = avcodec_find_encoder(av_guess_codec(ctx->avc->oformat, NULL,
                               ctx->avc->filename, NULL, AVMEDIA_TYPE_AUDIO));

    /* taken from ffmpeg unchanged
     * TODO turn this into an option if anyone needs this */
    ctx->avc->preload   = 0.5 * AV_TIME_BASE;
    ctx->avc->max_delay = 0.7 * AV_TIME_BASE;

    ctx->abytes = 0;
    ctx->vbytes = 0;
    ctx->frames = 0;

    return ctx;
}

int encode_lavc_start(struct encode_lavc_context *ctx)
{
    AVDictionaryEntry *de;

    if (ctx->header_written < 0)
        return 0;
    if (ctx->header_written > 0)
        return 1;

    ctx->header_written = -1;

    if (!(ctx->avc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ctx->avc->pb, ctx->avc->filename, URL_WRONLY) < 0) {
            mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: could not open '%s'\n",
                   ctx->avc->filename);
            encode_lavc_finish(ctx);
            abort(); // XXXXXXXXXXXXXXXXXXXXXXX
            return 0;
        }
    }

    ctx->t0 = GetTimerMS();

    if (avformat_write_header(ctx->avc, &ctx->foptions) < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: could not write header\n");
        encode_lavc_finish(ctx);
        abort(); // XXXXXXXXXXXXXXXXXXXXXXX
        return 0;
    }

    for (de = NULL; (de = av_dict_get(ctx->foptions, "", de,
                                      AV_DICT_IGNORE_SUFFIX));)
        av_log(ctx->avc, AV_LOG_ERROR, "Key '%s' not found.\n", de->key);
    av_dict_free(&ctx->foptions);

    ctx->header_written = 1;
    return 1;
}

void encode_lavc_finish(struct encode_lavc_context *ctx)
{
    unsigned i;

    if (!ctx)
        return;

    if (ctx->avc) {
        if (ctx->header_written > 0)
            av_write_trailer(ctx->avc);  // this is allowed to fail

        for (i = 0; i < ctx->avc->nb_streams; i++) {
            switch (ctx->avc->streams[i]->codec->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (ctx->twopass_bytebuffer_v) {
                    char *stats = ctx->avc->streams[i]->codec->stats_out;
                    if (stats)
                        stream_write_buffer(ctx->twopass_bytebuffer_v,
                                            stats, strlen(stats));
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (ctx->twopass_bytebuffer_a) {
                    char *stats = ctx->avc->streams[i]->codec->stats_out;
                    if (stats)
                        stream_write_buffer(ctx->twopass_bytebuffer_a,
                                            stats, strlen(stats));
                }
                break;
            default:
                break;
            }
            avcodec_close(ctx->avc->streams[i]->codec);
            talloc_free(ctx->avc->streams[i]->codec->stats_in);
            av_free(ctx->avc->streams[i]->codec);
            av_free(ctx->avc->streams[i]->info);
            av_free(ctx->avc->streams[i]);
        }

        if (ctx->twopass_bytebuffer_v) {
            free_stream(ctx->twopass_bytebuffer_v);
            ctx->twopass_bytebuffer_v = NULL;
        }

        if (ctx->twopass_bytebuffer_a) {
            free_stream(ctx->twopass_bytebuffer_a);
            ctx->twopass_bytebuffer_a = NULL;
        }

        mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: encoded %lld bytes\n",
               ctx->vbytes);
        mp_msg(MSGT_AO, MSGL_INFO, "ao-lavc: encoded %lld bytes\n",
               ctx->abytes);
        if (ctx->avc->pb) {
            mp_msg(MSGT_AO, MSGL_INFO,
                   "encode-lavc: muxing overhead %lld bytes\n",
                   (long long) (avio_tell(ctx->avc->pb) - ctx->vbytes
                                                        - ctx->abytes));
            avio_close(ctx->avc->pb);
        }

        av_free(ctx->avc);
    }

    talloc_free(ctx);
}

static void encode_2pass_prepare(struct encode_lavc_context *ctx, AVDictionary **dictp, void *octx,
                                 AVStream *stream, struct stream **bytebuf,
                                 int msgt, const char *prefix)
{
    if (!*bytebuf) {
        char buf[sizeof(ctx->avc->filename) + 12];
        AVDictionaryEntry *de = av_dict_get(ctx->voptions, "flags", NULL, 0);

        snprintf(buf, sizeof(buf), "%s-%s-pass1.log", ctx->avc->filename,
                 prefix);
        buf[sizeof(buf) - 1] = 0;

        if (value_has_flag(de ? de->value : "", "pass2")) {
            if (!(*bytebuf = open_stream(buf, NULL, NULL))) {
                mp_msg(msgt, MSGL_WARN, "%s: could not open '%s', "
                       "disabling 2-pass encoding at pass 2\n", prefix, buf);
                stream->codec->flags &= ~CODEC_FLAG_PASS2;
                set_to_avdictionary(stream->codec, dictp, octx, "flags=-pass2", "=", "");
            } else {
                struct bstr content = stream_read_complete(*bytebuf, NULL,
                                                           1000000000, 1);
                if (content.start == NULL) {
                    mp_msg(msgt, MSGL_WARN, "%s: could not read '%s', "
                           "disabling 2-pass encoding at pass 1\n",
                           prefix, ctx->avc->filename);
                } else {
                    content.start[content.len] = 0;
                    stream->codec->stats_in = content.start;
                }
                free_stream(*bytebuf);
                *bytebuf = NULL;
            }
        }

        if (value_has_flag(de ? de->value : "", "pass1")) {
            if (!(*bytebuf = open_output_stream(buf, NULL))) {
                mp_msg(msgt, MSGL_WARN, "%s: could not open '%s', disabling "
                       "2-pass encoding at pass 1\n",
                       prefix, ctx->avc->filename);
                set_to_avdictionary(stream->codec, dictp, octx, "flags=-pass1", "=", "");
            }
        }
    }
}

AVStream *encode_lavc_alloc_stream(struct encode_lavc_context *ctx,
                                   enum AVMediaType mt)
{
    AVDictionaryEntry *de;
    AVStream *stream;
    char **p;
    int i;
    AVCodecContext *dummy;

    if (ctx->header_written)
        return NULL;

    for (i = 0; i < ctx->avc->nb_streams; ++i)
        if (ctx->avc->streams[i]->codec->codec_type == mt)
            // already have a stream of that type, this cannot really happen
            return NULL;

    stream = av_new_stream(ctx->avc, 0);
    if (!stream)
        return stream;

    if (ctx->timebase.den == 0) {
        AVRational r;

        if (ctx->options->fps > 0)
            r = av_d2q(ctx->options->fps, ctx->options->fps * 1001 + 2);
        else if (ctx->options->autofps && vo_fps > 0) {
            r = av_d2q(vo_fps, vo_fps * 1001 + 2);
            mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: option -ofps not specified "
                   "but -oautofps is active, using guess of %u/%u\n",
                   (unsigned)r.num, (unsigned)r.den);
        } else {
            // we want to handle:
            //      1/25
            //   1001/24000
            //   1001/30000
            // for this we would need 120000fps...
            // however, mpeg-4 only allows 16bit values
            // so let's take 1001/30000 out
            r.num = 24000;
            r.den = 1;
            mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: option -ofps not specified "
                   "and fps could not be inferred, using guess of %u/%u\n",
                   (unsigned)r.num, (unsigned)r.den);
        }

        if (ctx->vc && ctx->vc->supported_framerates)
            r = ctx->vc->supported_framerates[av_find_nearest_q_idx(r,
                    ctx->vc->supported_framerates)];

        ctx->timebase.num = r.den;
        ctx->timebase.den = r.num;
    }

    switch (mt) {
    case AVMEDIA_TYPE_VIDEO:
        if (!ctx->vc) {
            mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: encoder not found\n");
            encode_lavc_finish(ctx);
            abort(); // XXXXXXXXXXXXXXXXXXXXXXX
            return NULL;
        }
        avcodec_get_context_defaults3(stream->codec, ctx->vc);

        // stream->time_base = ctx->timebase;
        // doing this breaks mpeg2ts in ffmpeg
        // which doesn't properly force the time base to be 90000
        // furthermore, ffmpeg.c doesn't do this either and works

        stream->codec->codec_id = ctx->vc->id;
        stream->codec->time_base = ctx->timebase;
        stream->codec->codec_type = AVMEDIA_TYPE_VIDEO;

        dummy = avcodec_alloc_context3(ctx->vc);
        dummy->codec = ctx->vc; // FIXME remove this once we can, caused by a bug in libav, elenril is aware of this

        ctx->voptions = NULL;

        // libx264: default to preset=medium
        if (!strcmp(ctx->vc->name, "libx264"))
            set_to_avdictionary(stream->codec, &ctx->voptions, dummy, "preset=medium", "=", "");

        if (ctx->options->vopts)
            for (p = ctx->options->vopts; *p; ++p)
                if (set_to_avdictionary(stream->codec, &ctx->voptions, dummy,
                        *p, "=", "") <= 0)
                    mp_msg(MSGT_VO, MSGL_WARN,
                           "vo-lavc: could not set option %s\n", *p);

        de = av_dict_get(ctx->voptions, "global_quality", NULL, 0);
        if(de)
            set_to_avdictionary(stream->codec, &ctx->voptions, dummy, "flags=+qscale", "=", "");

        if (ctx->avc->oformat->flags & AVFMT_GLOBALHEADER)
            set_to_avdictionary(stream->codec, &ctx->voptions, dummy, "flags=+global_header", "=", "");

        encode_2pass_prepare(ctx, &ctx->voptions, dummy, stream, &ctx->twopass_bytebuffer_v, MSGT_VO,
                             "vo-lavc");

        av_free(dummy);
        break;

    case AVMEDIA_TYPE_AUDIO:
        if (!ctx->ac) {
            mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: encoder not found\n");
            encode_lavc_finish(ctx);
            abort(); // XXXXXXXXXXXXXXXXXXXXXXX
            return NULL;
        }
        avcodec_get_context_defaults3(stream->codec, ctx->ac);

        stream->codec->codec_id = ctx->ac->id;
        stream->codec->time_base = ctx->timebase;
        stream->codec->codec_type = AVMEDIA_TYPE_AUDIO;

        dummy = avcodec_alloc_context3(ctx->ac);
        dummy->codec = ctx->ac; // FIXME remove this once we can, caused by a bug in libav, elenril is aware of this

        ctx->aoptions = NULL;

        if (ctx->options->aopts)
            for (p = ctx->options->aopts; *p; ++p)
                if (set_to_avdictionary(stream->codec, &ctx->aoptions, dummy,
                        *p, "=", "") <= 0)
                    mp_msg(MSGT_VO, MSGL_WARN,
                           "vo-lavc: could not set option %s\n", *p);

        de = av_dict_get(ctx->aoptions, "global_quality", NULL, 0);
        if(de)
            set_to_avdictionary(stream->codec, &ctx->aoptions, dummy, "flags=+qscale", "=", "");

        if (ctx->avc->oformat->flags & AVFMT_GLOBALHEADER)
            set_to_avdictionary(stream->codec, &ctx->aoptions, dummy, "flags=+global_header", "=", "");

        encode_2pass_prepare(ctx, &ctx->aoptions, dummy, stream, &ctx->twopass_bytebuffer_a, MSGT_AO,
                             "ao-lavc");

        av_free(dummy);
        break;

    default:
        mp_msg(MSGT_VO, MSGL_ERR,
               "encode-lavc: requested invalid stream type\n");
        encode_lavc_finish(ctx);
        abort();     // XXXXXXXXXXXXXXXXXXXXXXX
        return NULL;
    }

    return stream;
}

AVCodec *encode_lavc_get_codec(struct encode_lavc_context *ctx,
                               AVStream *stream)
{
    switch (stream->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return ctx->vc;
    case AVMEDIA_TYPE_AUDIO:
        return ctx->ac;
    default:
        break;
    }
    return NULL;
}

int encode_lavc_open_codec(struct encode_lavc_context *ctx, AVStream *stream)
{
    AVDictionaryEntry *de;
    int ret;

    switch (stream->codec->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            ret = avcodec_open2(stream->codec, ctx->vc, &ctx->voptions);

            // complain about all remaining options, then free the dict
            for (de = NULL; (de = av_dict_get(ctx->voptions, "", de,
                                              AV_DICT_IGNORE_SUFFIX));)
                av_log(ctx->avc, AV_LOG_ERROR, "Key '%s' not found.\n", de->key);
            av_dict_free(&ctx->voptions);

            break;
        case AVMEDIA_TYPE_AUDIO:
            ret = avcodec_open2(stream->codec, ctx->ac, &ctx->aoptions);

            // complain about all remaining options, then free the dict
            for (de = NULL; (de = av_dict_get(ctx->aoptions, "", de,
                                              AV_DICT_IGNORE_SUFFIX));)
                av_log(ctx->avc, AV_LOG_ERROR, "Key '%s' not found.\n", de->key);
            av_dict_free(&ctx->aoptions);

            break;
        default:
            ret = -1;
            break;
    }

    return ret;
}

void encode_lavc_write_stats(struct encode_lavc_context *ctx, AVStream *stream)
{
    switch (stream->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (ctx->twopass_bytebuffer_v)
            if (stream->codec->stats_out)
                stream_write_buffer(ctx->twopass_bytebuffer_v,
                                    stream->codec->stats_out,
                                    strlen(stream->codec->stats_out));
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (ctx->twopass_bytebuffer_a)
            if (stream->codec->stats_out)
                stream_write_buffer(ctx->twopass_bytebuffer_a,
                                    stream->codec->stats_out,
                                    strlen(stream->codec->stats_out));
        break;
    default:
        break;
    }
}

int encode_lavc_write_frame(struct encode_lavc_context *ctx, AVPacket *packet)
{
    int r;

    if (ctx->header_written <= 0)
        return -1;

    mp_msg(MSGT_VO, MSGL_DBG2,
           "encode-lavc: write frame: stream %d ptsi %d (%f) size %d\n",
           (int)packet->stream_index,
           (int)packet->pts,
           packet->pts * (double)ctx->avc->streams[packet->stream_index]->
                   time_base.num / (double)ctx->avc->streams[packet->
                   stream_index]->time_base.den,
           (int)packet->size);

    switch (ctx->avc->streams[packet->stream_index]->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        ctx->vbytes += packet->size;
        ++ctx->frames;
        break;
    case AVMEDIA_TYPE_AUDIO:
        ctx->abytes += packet->size;
        break;
    default:
        break;
    }

    r = av_interleaved_write_frame(ctx->avc, packet);

    return r;
}

int encode_lavc_supports_pixfmt(struct encode_lavc_context *ctx,
                                enum PixelFormat pix_fmt)
{
    if (!ctx->vc)
        return 0;
    if (pix_fmt == PIX_FMT_NONE)
        return 0;

    if (!ctx->vc->pix_fmts) {
        return VFCAP_CSP_SUPPORTED;
    } else {
        const enum PixelFormat *p;
        for (p = ctx->vc->pix_fmts; *p >= 0; ++p) {
            if (pix_fmt == *p)
                return VFCAP_CSP_SUPPORTED;
        }
    }
    return 0;
}

void encode_lavc_discontinuity(struct encode_lavc_context *ctx)
{
    if (!ctx)
        return;
    ctx->audio_pts_offset = MP_NOPTS_VALUE;
    ctx->last_video_in_pts = MP_NOPTS_VALUE;
}

static void encode_lavc_printoptions(void *obj, const char *indent,
                                     const char *subindent, const char *unit,
                                     int filter_and, int filter_eq)
{
    const AVOption *opt = NULL;
    char optbuf[32];
    while ((opt = av_next_option(obj, opt))) {
        // if flags are 0, it simply hasn't been filled in yet and may be
        // potentially useful
        if (opt->flags)
            if ((opt->flags & filter_and) != filter_eq)
                continue;
        /* Don't print CONST's on level one.
         * Don't print anything but CONST's on level two.
         * Only print items from the requested unit.
         */
        if (!unit && opt->type == FF_OPT_TYPE_CONST)
            continue;
        else if (unit && opt->type != FF_OPT_TYPE_CONST)
            continue;
        else if (unit && opt->type == FF_OPT_TYPE_CONST
                 && strcmp(unit, opt->unit))
            continue;
        else if (unit && opt->type == FF_OPT_TYPE_CONST)
            mp_msg(MSGT_VO, MSGL_INFO, "%s", subindent);
        else
            mp_msg(MSGT_VO, MSGL_INFO, "%s", indent);

        switch (opt->type) {
        case FF_OPT_TYPE_FLAGS:
            snprintf(optbuf, sizeof(optbuf), "%s=<flags>", opt->name);
            break;
        case FF_OPT_TYPE_INT:
            snprintf(optbuf, sizeof(optbuf), "%s=<int>", opt->name);
            break;
        case FF_OPT_TYPE_INT64:
            snprintf(optbuf, sizeof(optbuf), "%s=<int64>", opt->name);
            break;
        case FF_OPT_TYPE_DOUBLE:
            snprintf(optbuf, sizeof(optbuf), "%s=<double>", opt->name);
            break;
        case FF_OPT_TYPE_FLOAT:
            snprintf(optbuf, sizeof(optbuf), "%s=<float>", opt->name);
            break;
        case FF_OPT_TYPE_STRING:
            snprintf(optbuf, sizeof(optbuf), "%s=<string>", opt->name);
            break;
        case FF_OPT_TYPE_RATIONAL:
            snprintf(optbuf, sizeof(optbuf), "%s=<rational>", opt->name);
            break;
        case FF_OPT_TYPE_BINARY:
            snprintf(optbuf, sizeof(optbuf), "%s=<binary>", opt->name);
            break;
        case FF_OPT_TYPE_CONST:
            snprintf(optbuf, sizeof(optbuf), "  [+-]%s", opt->name);
            break;
        default:
            snprintf(optbuf, sizeof(optbuf), "%s", opt->name);
            break;
        }
        optbuf[sizeof(optbuf) - 1] = 0;
        mp_msg(MSGT_VO, MSGL_INFO, "%-32s ", optbuf);
        if (opt->help)
            mp_msg(MSGT_VO, MSGL_INFO, " %s", opt->help);
        mp_msg(MSGT_VO, MSGL_INFO, "\n");
        if (opt->unit && opt->type != FF_OPT_TYPE_CONST)
            encode_lavc_printoptions(obj, indent, subindent, opt->unit,
                                     filter_and, filter_eq);
    }
}

bool encode_lavc_showhelp(struct MPOpts *opts)
{
    bool help_output = false;
#define CHECKS(str) ((str) && strcmp((str), "help") == 0 ? (help_output |= 1) : 0)
#define CHECKV(strv) ((strv) && (strv)[0] && strcmp((strv)[0], "help") == 0 ? (help_output |= 1) : 0)
    if (CHECKS(opts->encode_output.format)) {
        AVOutputFormat *c = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output formats:\n");
        while ((c = av_oformat_next(c)))
            mp_msg(MSGT_VO, MSGL_INFO, "  -of %-13s %s\n", c->name,
                   c->long_name ? c->long_name : "");
        av_free(c);
    }
    if (CHECKV(opts->encode_output.fopts)) {
        AVFormatContext *c = avformat_alloc_context();
        AVOutputFormat *format = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output format ctx->options:\n");
        encode_lavc_printoptions(c, "  -ofopts ", "          ", NULL,
                                 AV_OPT_FLAG_ENCODING_PARAM,
                                 AV_OPT_FLAG_ENCODING_PARAM);
        av_free(c);
        while ((format = av_oformat_next(format))) {
            if (format->priv_class) {
                mp_msg(MSGT_VO, MSGL_INFO, "Additionally, for -of %s:\n",
                       format->name);
                encode_lavc_printoptions(&format->priv_class, "  -ofopts ",
                                         "          ", NULL,
                                         AV_OPT_FLAG_ENCODING_PARAM,
                                         AV_OPT_FLAG_ENCODING_PARAM);
            }
        }
    }
    if (CHECKV(opts->encode_output.vopts)) {
        AVCodecContext *c = avcodec_alloc_context3(NULL);
        AVCodec *codec = NULL;
        mp_msg(MSGT_VO, MSGL_INFO,
               "Available output video codec ctx->options:\n");
        encode_lavc_printoptions(c, "  -ovcopts ", "           ", NULL,
                AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM,
                AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM);
        av_free(c);
        while ((codec = av_codec_next(codec))) {
            if (!codec->encode)
                continue;
            if (codec->type != AVMEDIA_TYPE_VIDEO)
                continue;
            if (codec->priv_class) {
                mp_msg(MSGT_VO, MSGL_INFO, "Additionally, for -ovc %s:\n",
                       codec->name);
                encode_lavc_printoptions(&codec->priv_class, "  -ovcopts ",
                        "           ", NULL,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM);
            }
        }
    }
    if (CHECKV(opts->encode_output.aopts)) {
        AVCodecContext *c = avcodec_alloc_context3(NULL);
        AVCodec *codec = NULL;
        mp_msg(MSGT_VO, MSGL_INFO,
               "Available output audio codec ctx->options:\n");
        encode_lavc_printoptions(c, "  -oacopts ", "           ", NULL,
                AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM,
                AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM);
        av_free(c);
        while ((codec = av_codec_next(codec))) {
            if (!codec->encode)
                continue;
            if (codec->type != AVMEDIA_TYPE_AUDIO)
                continue;
            if (codec->priv_class) {
                mp_msg(MSGT_VO, MSGL_INFO, "Additionally, for -oac %s:\n",
                       codec->name);
                encode_lavc_printoptions(&codec->priv_class, "  -oacopts ",
                        "           ", NULL,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM);
            }
        }
    }
    if (CHECKS(opts->encode_output.vcodec)) {
        AVCodec *c = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output video codecs:\n");
        while ((c = av_codec_next(c))) {
            if (!c->encode)
                continue;
            if (c->type != AVMEDIA_TYPE_VIDEO)
                continue;
            mp_msg(MSGT_VO, MSGL_INFO, "  -ovc %-12s %s\n", c->name,
                   c->long_name ? c->long_name : "");
            if (c->priv_class)
                encode_lavc_printoptions(&c->priv_class, "    -ovcopts ",
                        "             ", NULL,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM);
        }
        av_free(c);
    }
    if (CHECKS(opts->encode_output.acodec)) {
        AVCodec *c = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output audio codecs:\n");
        while ((c = av_codec_next(c))) {
            if (!c->encode)
                continue;
            if (c->type != AVMEDIA_TYPE_AUDIO)
                continue;
            mp_msg(MSGT_VO, MSGL_INFO, "  -oac %-12s %s\n", c->name,
                   c->long_name ? c->long_name : "");
            if (c->priv_class)
                encode_lavc_printoptions(&c->priv_class, "    -oacopts ",
                        "             ", NULL,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM,
                        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM);
        }
        av_free(c);
    }
    return help_output;
}

double encode_lavc_getoffset(struct encode_lavc_context *ctx, AVStream *stream)
{
    switch (stream->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return ctx->options->voffset;
    case AVMEDIA_TYPE_AUDIO:
        return ctx->options->aoffset;
    default:
        break;
    }
    return 0;
}

int encode_lavc_getstatus(struct encode_lavc_context *ctx,
                          char *buf, int bufsize,
                          float relative_position, float playback_time)
{
    float minutes, megabytes, fps, x;
    float f = FFMAX(0.0001, relative_position);
    if (!ctx)
        return -1;
    minutes = (GetTimerMS() - ctx->t0) / 60000.0 * (1-f) / f;
    megabytes = ctx->avc->pb ? (avio_size(ctx->avc->pb) / 1048576.0 / f) : 0;
    fps = ctx->frames / ((GetTimerMS() - ctx->t0) / 1000.0);
    x = playback_time / ((GetTimerMS() - ctx->t0) / 1000.0);
    if (ctx->frames)
        snprintf(buf, bufsize, "{%.1f%% %.1fmin %.1ffps %.1fMB}",
                 relative_position * 100.0, minutes, fps, megabytes);
    else
        snprintf(buf, bufsize, "{%.1f%% %.1fmin %.2fx %.1fMB}",
                 relative_position * 100.0, minutes, x, megabytes);
    buf[bufsize - 1] = 0;
    return 0;
}

// vim: ts=4 sw=4 et
