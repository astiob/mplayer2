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

static struct encode_output_conf *options;
static struct MPContext *mpctx;

// these are processed from the options
static AVFormatContext *avc;
static AVRational timebase;
static AVCodec *vc;
static AVCodec *ac;

// values created during encoding
static int header_written; // -1 means currently writing
static double timesync_delta;
static int timesync_available;
static size_t abytes;
static size_t vbytes;
static struct stream *twopass_bytebuffer_a;
static struct stream *twopass_bytebuffer_v;
static unsigned int t0;
static unsigned int frames;

static int set_avoptions(void *ctx, const void *privclass, void *privctx, const char *str, const char *key_val_sep, const char *pairs_sep, int dry_run)
{
    int good = 0;
    int errorcode = 0;

    if (!privclass)
        privctx = NULL;

    while (*str)
    {
        char *key = av_get_token(&str, key_val_sep);
        char *val;
        int ret;

        if (*key && strspn(str, key_val_sep)) {
            str++;
            val = av_get_token(&str, pairs_sep);
        } else {
            if (!dry_run)
                av_log(ctx, AV_LOG_ERROR, "Missing key or no key/value separator found after key '%s'\n", key);
            av_free(key);
            if (!errorcode)
                errorcode = AVERROR(EINVAL);
            if(*str)
                ++str;
            continue;
        }

        if (!dry_run)
            av_log(ctx, AV_LOG_DEBUG, "Setting value '%s' for key '%s'\n", val, key);

        ret = AVERROR(ENOENT);
        if (dry_run) {
            char buf[256];
            const AVOption *opt;
            const char *p = NULL;
            if (privctx)
                p = av_get_string(privctx, key, &opt, buf, sizeof(buf));
            if (p == NULL)
                p = av_get_string(ctx, key, &opt, buf, sizeof(buf));
            if (p)
                ret = 0;
        } else {
            if (privctx)
                ret = av_set_string3(privctx, key, val, 1, NULL);
            if (ret == AVERROR(ENOENT))
                ret = av_set_string3(ctx, key, val, 1, NULL);
            if (ret == AVERROR(ENOENT))
                av_log(ctx, AV_LOG_ERROR, "Key '%s' not found.\n", key);
        }

        av_free(key);
        av_free(val);

        if (ret < 0) {
            if (!errorcode)
                errorcode = ret;
        } else
            ++good;

        if(*str)
            ++str;
    }
    return errorcode ? errorcode : good;
}

int encode_lavc_available(void)
{
    return !!avc;
}

int encode_lavc_oformat_flags(void)
{
    return avc ? avc->oformat->flags : 0;
}

void encode_lavc_init(struct MPContext *mpctx_, struct encode_output_conf *options_)
{
    AVFormatParameters avp;

    if (!options_->file)
        return;

    mpctx = mpctx_;
    options = options_;

    avcodec_register_all();
    av_register_all();

    avc = avformat_alloc_context();

    if (!(avc->oformat = av_guess_format(options->format, options->file, NULL))) {
        mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: format not found\n");
        encode_lavc_finish();
        exit_player_with_rc(mpctx, EXIT_ERROR, 1);
        return;
    }

    av_strlcpy(avc->filename, options->file, sizeof(avc->filename));

    memset(&avp, 0, sizeof(avp));
    if (av_set_parameters(avc, &avp) < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: format cannot be initialized\n");
        encode_lavc_finish();
        exit_player_with_rc(mpctx, EXIT_ERROR, 1);
    }

    if (options->fopts) {
        char **p;
        for (p = options->fopts; *p; ++p) {
            if(set_avoptions(avc, avc->oformat->priv_class, avc->priv_data, *p, "=", "", 0) <= 0)
                mp_msg(MSGT_VO, MSGL_WARN, "encode-lavc: could not set option %s\n", *p);
        }
    }

    if (options->vcodec) {
        vc = avcodec_find_encoder_by_name(options->vcodec);
        if (!vc) {
            mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: video codec not found\n");
            encode_lavc_finish();
            exit_player_with_rc(mpctx, EXIT_ERROR, 1);
            return;
        }
    } else {
        vc = avcodec_find_encoder(av_guess_codec(avc->oformat, NULL, avc->filename, NULL, AVMEDIA_TYPE_VIDEO));
    }

    if (options->acodec) {
        ac = avcodec_find_encoder_by_name(options->acodec);
        if (!ac) {
            mp_msg(MSGT_VO, MSGL_ERR, "ao-lavc: audio codec not found\n");
            encode_lavc_finish();
            exit_player_with_rc(mpctx, EXIT_ERROR, 1);
            return;
        }
    } else {
        ac = avcodec_find_encoder(av_guess_codec(avc->oformat, NULL, avc->filename, NULL, AVMEDIA_TYPE_AUDIO));
    }

    /* taken from ffmpeg unchanged, TODO turn this into an option if anyone needs this */
    avc->preload   = 0.5 * AV_TIME_BASE;
    avc->max_delay = 0.7 * AV_TIME_BASE;

    abytes = 0;
    vbytes = 0;
    frames = 0;
}

int encode_lavc_start(void)
{
    if (!avc)
        return 0;
    if (header_written < 0)
        return 0;
    if (header_written > 0)
        return 1;

    header_written = -1;

    if (!(avc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&avc->pb, avc->filename, URL_WRONLY) < 0) {
            mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: could not open '%s'\n",
                    avc->filename);
            encode_lavc_finish();
            exit_player_with_rc(mpctx, EXIT_ERROR, 1);
            return 0;
        }
    }

    t0 = GetTimerMS();

    if (av_write_header(avc) < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: could not write header\n");
        encode_lavc_finish();
        exit_player_with_rc(mpctx, EXIT_ERROR, 1);
        return 0;
    }

    header_written = 1;
    return 1;
}

void encode_lavc_finish(void)
{
    unsigned i;

    if (!avc)
        return;

    if (header_written > 0)
        av_write_trailer(avc); // this is allowed to fail

    for (i = 0; i < avc->nb_streams; i++) {
        switch (avc->streams[i]->codec->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (twopass_bytebuffer_v)
                if (avc->streams[i]->codec->stats_out)
                    stream_write_buffer(twopass_bytebuffer_v, avc->streams[i]->codec->stats_out, strlen(avc->streams[i]->codec->stats_out));
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (twopass_bytebuffer_a)
                if (avc->streams[i]->codec->stats_out)
                    stream_write_buffer(twopass_bytebuffer_a, avc->streams[i]->codec->stats_out, strlen(avc->streams[i]->codec->stats_out));
            break;
        default:
            break;
        }
        avcodec_close(avc->streams[i]->codec);
	if (avc->streams[i]->codec->stats_in)
		talloc_free(avc->streams[i]->codec->stats_in);
        av_free(avc->streams[i]->codec);
        av_free(avc->streams[i]->info);
        av_free(avc->streams[i]);
    }

    if (twopass_bytebuffer_v) {
        free_stream(twopass_bytebuffer_v);
        twopass_bytebuffer_v = NULL;
    }

    if (twopass_bytebuffer_a) {
        free_stream(twopass_bytebuffer_a);
        twopass_bytebuffer_a = NULL;
    }

    mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: encoded %lu bytes\n", (unsigned long) vbytes);
    mp_msg(MSGT_AO, MSGL_INFO, "ao-lavc: encoded %lu bytes\n", (unsigned long) abytes);
    if(avc->pb) {
        mp_msg(MSGT_AO, MSGL_INFO, "encode-lavc: muxing overhead %ld bytes\n", (signed long) (avio_tell(avc->pb) - vbytes - abytes));
        avio_close(avc->pb);
    }

    av_free(avc);
    avc = NULL;

    abytes = 0;
    vbytes = 0;
}

/* Use TOOLS/encode_lavc.sh to update this table */
static const char *x264_presets[][2] = {
    {"ultrafast", "coder=0 flags=-loop cmp=+chroma partitions=-parti8x8-parti4x4-partp8x8-partb8x8 me_method=dia subq=0 me_range=16 g=250 keyint_min=25 sc_threshold=0 i_qfactor=0.71 b_strategy=0 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=0 refs=1 directpred=1 trellis=0 flags2=-bpyramid-mixed_refs-wpred-dct8x8+fastpskip-mbtree wpredp=0 aq_mode=0 rc_lookahead=0"},
    {"superfast", "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4-partp8x8-partb8x8 me_method=dia subq=1 me_range=16 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=1 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=3 refs=1 directpred=1 trellis=0 flags2=+bpyramid-mixed_refs+wpred+dct8x8+fastpskip-mbtree wpredp=0 rc_lookahead=0"},
    {"veryfast",  "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partb8x8 me_method=hex subq=2 me_range=16 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=1 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=3 refs=1 directpred=1 trellis=0 flags2=+bpyramid-mixed_refs+wpred+dct8x8+fastpskip wpredp=0 rc_lookahead=10"},
    {"faster",    "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partb8x8 me_method=hex subq=4 me_range=16 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=1 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=3 refs=2 directpred=1 trellis=1 flags2=+bpyramid-mixed_refs+wpred+dct8x8+fastpskip wpredp=1 rc_lookahead=20"},
    {"fast",      "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partb8x8 me_method=hex subq=6 me_range=16 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=1 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=3 refs=2 directpred=1 trellis=1 flags2=+bpyramid+mixed_refs+wpred+dct8x8+fastpskip wpredp=2 rc_lookahead=30"},
    {"medium",    "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partb8x8 me_method=hex subq=7 me_range=16 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=1 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=3 refs=3 directpred=1 trellis=1 flags2=+bpyramid+mixed_refs+wpred+dct8x8+fastpskip wpredp=2"},
    {"slow",      "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partb8x8 me_method=umh subq=8 me_range=16 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=2 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=3 refs=5 directpred=3 trellis=1 flags2=+bpyramid+mixed_refs+wpred+dct8x8+fastpskip wpredp=2 rc_lookahead=50"},
    {"slower",    "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partp4x4+partb8x8 me_method=umh subq=9 me_range=16 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=2 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=3 refs=8 directpred=3 trellis=2 flags2=+bpyramid+mixed_refs+wpred+dct8x8+fastpskip wpredp=2 rc_lookahead=60"},
    {"veryslow",  "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partp4x4+partb8x8 me_method=umh subq=10 me_range=24 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=2 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=8 refs=16 directpred=3 trellis=2 flags2=+bpyramid+mixed_refs+wpred+dct8x8+fastpskip wpredp=2 rc_lookahead=60"},
    {"placebo",   "coder=1 flags=+loop cmp=+chroma partitions=+parti8x8+parti4x4+partp8x8+partp4x4+partb8x8 me_method=tesa subq=10 me_range=24 g=250 keyint_min=25 sc_threshold=40 i_qfactor=0.71 b_strategy=2 qcomp=0.6 qmin=10 qmax=51 qdiff=4 bf=16 refs=16 directpred=3 trellis=2 flags2=+bpyramid+mixed_refs+wpred+dct8x8-fastpskip wpredp=2 rc_lookahead=60"},
};

/* Tunes values are taken from x264 sources */
// XXX:"grain"       tuning not fully supported yet (no access to --no-dct-decimate)
// XXX:"zerolatency" tuning not fully supported yet (no access to --sync-lookahead --sliced-threads)
static const char *x264_tunes[][2] = {
    {"film",        "deblockalpha=-1 deblockbeta=-1 psy_trellis=0.15"},
    {"animation",   "deblockalpha=1 deblockbeta=1 psy_rd=0.4 aq_strength=0.6"},
    {"grain",       "aq_strength=0.5 deblockalpha=-2 deblockbeta=-2 i_qfactor=0.9090909 b_qfactor=1.1 psy_trellis=0.25 qcomp=0.8"},
    {"stillimage",  "aq_strength=1.2 deblockalpha=-3 deblockbeta=-3 psy_rd=2.0 psy_trellis=0.7"},
    {"psnr",        "aq_mode=0 flags2=-psy"},
    {"ssim",        "aq_mode=2 flags2=-psy"},
    {"fastdecode",  "coder=0 flags=-loop flags2=-wpred wpredp=0"},
    {"zerolatency", "bf=0 flags2=-mbtree rc_lookahead=0"},
};

static const char *x264_profiles[][2] = {
    {"baseline", "coder=0 bf=0 flags2=-wpred-dct8x8 wpredp=0"},
    {"main",     "flags2=-dct8x8"},
    {"high",     ""},
};

#define CHECK_PRESET(array, step) do {\
    for (int i = 0; i < FF_ARRAY_ELEMS(array); i++) {\
        if (strcmp(preset, array[i][0]) == 0) {\
            commands    = array[i][1];\
            commandstep = step;\
            break;\
        }\
    }\
} while (0)

#define STEP_MIN 1
#define STEP_PRESET 1
#define STEP_TUNE 2
#define STEP_PROFILE 3
#define STEP_MAX 3
// step is: -1 = never apply, 0 = always apply, otherwise a number from STEP_MIN to STEP_MAX
static int encode_lavc_apply_preset(AVCodecContext *cc, AVCodec *codec, const char *preset, int step)
{
    const char *commands = NULL;
    int commandstep = 0;
    int boost_bf_and_refs = 0;

    // TODO also provide presets for other codecs?
    if (!strcmp(codec->name, "libx264")) {
        if (!strcmp(preset, "animation"))
            boost_bf_and_refs = 1;

        CHECK_PRESET(x264_presets,  1);
        CHECK_PRESET(x264_tunes,    2);
        CHECK_PRESET(x264_profiles, 3);
    }

    if (!commandstep)
        return 0;

    if (step < 0)
        return -1; // step < 0 means do not apply, just query

    if (step && step != commandstep)
        return -1; // -1 means not applied

    if (set_avoptions(cc, codec->priv_class, cc->priv_data, commands, "=", " ", 0) <= 0) {
        // can't really happen
        mp_msg(MSGT_VO, MSGL_WARN, "encode-lavc: could not set preset %s\n", preset);
    }
    if (boost_bf_and_refs) {
        int64_t o;
        o = av_get_int(cc, "max_b_frames", NULL);
        av_set_int(cc, "max_b_frames", o + 2);
        o = av_get_int(cc, "refs", NULL);
        if (cc->refs > 1)
            av_set_int(cc, "refs", o * 2);
        else
            av_set_int(cc, "refs", 1);
    }

    return 1;
}

static void encode_2pass_prepare(AVStream *stream, struct stream **bytebuf, int msgt, const char *prefix)
{
    if (!*bytebuf) {
        char buf[sizeof(avc->filename)+12];

        snprintf(buf, sizeof(buf), "%s-%s-pass1.log", avc->filename, prefix);
        buf[sizeof(buf)-1] = 0;

        if (stream->codec->flags & CODEC_FLAG_PASS2) {
            if (!(*bytebuf = open_stream(buf, NULL, NULL))) {
                mp_msg(msgt, MSGL_WARN, "%s: could not open '%s', disabling 2-pass encoding at pass 2\n",
                        prefix, buf);
                stream->codec->flags &= ~CODEC_FLAG_PASS2;
            } else {
                struct bstr content = stream_read_complete(*bytebuf, NULL, 1000000000, 1);
                if (content.start == NULL) {
                    mp_msg(msgt, MSGL_WARN, "%s: could not read '%s', disabling 2-pass encoding at pass 1\n",
                        prefix, avc->filename);
                } else {
                    content.start[content.len] = 0;
                    stream->codec->stats_in = content.start;
                }
                free_stream(*bytebuf);
                *bytebuf = NULL;
            }
        }

        if (stream->codec->flags & CODEC_FLAG_PASS1) {
            if (!(*bytebuf = open_output_stream(buf, NULL))) {
                mp_msg(msgt, MSGL_WARN, "%s: could not open '%s', disabling 2-pass encoding at pass 1\n",
                        prefix, avc->filename);
                stream->codec->flags &= ~CODEC_FLAG_PASS1;
            }
        }
    }
}

// like in ffmpeg.c
#define QSCALE_NONE -99999
AVStream *encode_lavc_alloc_stream(enum AVMediaType mt)
{
    AVStream *stream;
    char **p;
    int i;

    if (!avc)
        return NULL;

    if (header_written)
        return NULL;

    for (i = 0; i < avc->nb_streams; ++i)
        if (avc->streams[i]->codec->codec_type == mt)
            return NULL; // already have a stream of that type, this cannot really happen

    stream = av_new_stream(avc, 0);
    if (!stream)
        return stream;

    if(timebase.den == 0) {
        AVRational r;

        if (options->fps > 0) {
            r = av_d2q(options->fps, options->fps * 1001 + 2);
        } else if (options->autofps && vo_fps > 0) {
            r = av_d2q(vo_fps, vo_fps * 1001 + 2);
            mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: option -ofps not specified but -oautofps is active, using guess of %u/%u\n", (unsigned)r.num, (unsigned)r.den);
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
            mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: option -ofps not specified and fps could not be inferred, using guess of %u/%u\n", (unsigned)r.num, (unsigned)r.den);
        }

        if (vc && vc->supported_framerates)
            r = vc->supported_framerates[av_find_nearest_q_idx(r, vc->supported_framerates)];

        timebase.num = r.den;
        timebase.den = r.num;
    }

    switch (mt) {
    case AVMEDIA_TYPE_VIDEO:
        if (!vc) {
            mp_msg(MSGT_VO, MSGL_ERR, "vo-lavc: encoder not found\n");
            encode_lavc_finish();
            exit_player_with_rc(mpctx, EXIT_ERROR, 1);
            return NULL;
        }

        // stream->time_base = timebase;
        // doing this breaks mpeg2ts in ffmpeg
        // which doesn't properly force the time base to be 90000
        // furthermore, ffmpeg.c doesn't do this either and works

        avcodec_get_context_defaults3(stream->codec, vc);
        stream->codec->codec_id = vc->id;
        stream->codec->time_base = timebase;
        stream->codec->global_quality = 0;
        stream->codec->codec_type = AVMEDIA_TYPE_VIDEO;

        if (set_avoptions(stream->codec, vc->priv_class, stream->codec->priv_data, "preset=medium", "=", "", 1) <= 0)
            encode_lavc_apply_preset(stream->codec, vc, "medium", 0);
        else if(set_avoptions(stream->codec, vc->priv_class, stream->codec->priv_data, "preset=medium", "=", "", 0) <= 0)
            mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: could not set option preset=medium\n");

        if (options->vopts) {
            // fake ffmpeg's preset/tune/profile options if needed
            const char *preset = NULL;
            const char *tune = NULL;
            const char *profile = NULL;
            // if a legacy libx264 codec is detected, activate the hardcoded system
            if (!strcmp(vc->name, "libx264") && (set_avoptions(stream->codec, vc->priv_class, stream->codec->priv_data, "preset=medium", "=", "", 1) <= 0)) {
                for (p = options->vopts; *p; ++p) {
                    if (!strncmp(*p, "preset=", 7))
                        preset = *p + 7;
                    if (!strncmp(*p, "tune=", 5))
                        tune = *p + 5;
                    if (!strncmp(*p, "profile=", 8))
                        profile = *p + 8;
                }
            }
            if(preset)
                mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: version of ffmpeg/libav/ffmpeg-mt does not support preset=, using a hardcoded legacy preset instead\n");
            if(tune)
                mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: version of ffmpeg/libav/ffmpeg-mt does not support tune=, using a hardcoded legacy tune instead\n");
            if(profile)
                mp_msg(MSGT_VO, MSGL_INFO, "vo-lavc: version of ffmpeg/libav/ffmpeg-mt does not support profile=, using a hardcoded legacy profile instead\n");
            if (preset)
                if (!encode_lavc_apply_preset(stream->codec, vc, preset, STEP_PRESET))
                    mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: could not find preset %s\n", preset);
            if (tune) {
                const char *str = tune;
                while (*str)
                {
                    char *key = av_get_token(&str, ",./-+");
                    if (*key) {
                        if (!encode_lavc_apply_preset(stream->codec, vc, key, STEP_TUNE))
                            mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: could not find tune %s\n", preset);
                    } else {
                        mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: empty tune name?\n");
                    }
                    if (*str)
                        ++str;
                }
            }
            for (p = options->vopts; *p; ++p) {
                if (!strncmp(*p, "preset=", 7))
                    if (preset)
                        continue;
                if (!strncmp(*p, "tune=", 5))
                    if (tune)
                        continue;
                if (!strncmp(*p, "profile=", 8))
                    if (profile)
                        continue;
                if (set_avoptions(stream->codec, vc->priv_class, stream->codec->priv_data, *p, "=", "", 0) <= 0)
                    mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: could not set option %s\n", *p);
            }
            if (profile)
                if (!encode_lavc_apply_preset(stream->codec, vc, profile, STEP_PROFILE))
                    mp_msg(MSGT_VO, MSGL_WARN, "vo-lavc: could not find profile %s\n", profile);
        }

        if (stream->codec->global_quality != 0)
            stream->codec->flags |= CODEC_FLAG_QSCALE;

        encode_2pass_prepare(stream, &twopass_bytebuffer_v, MSGT_VO, "vo-lavc");
        break;

    case AVMEDIA_TYPE_AUDIO:
        if (!ac) {
            mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: encoder not found\n");
            encode_lavc_finish();
            exit_player_with_rc(mpctx, EXIT_ERROR, 1);
            return NULL;
        }

        avcodec_get_context_defaults3(stream->codec, ac);
        stream->codec->codec_id = ac->id;
        stream->codec->time_base = timebase;
        stream->codec->global_quality = QSCALE_NONE;
        stream->codec->codec_type = AVMEDIA_TYPE_AUDIO;

        if (options->aopts)
            for (p = options->aopts; *p; ++p)
                if (set_avoptions(stream->codec, ac->priv_class, stream->codec->priv_data, *p, "=", "", 0) <= 0)
                    mp_msg(MSGT_VO, MSGL_WARN, "ao-lavc: could not set option %s\n", *p);

        if (stream->codec->global_quality != QSCALE_NONE)
            stream->codec->flags |= CODEC_FLAG_QSCALE;
        else
            stream->codec->global_quality = 0; // "unset"

        encode_2pass_prepare(stream, &twopass_bytebuffer_a, MSGT_AO, "ao-lavc");
        break;
    default:
        mp_msg(MSGT_VO, MSGL_ERR, "encode-lavc: requested invalid stream type\n");
        encode_lavc_finish();
        exit_player_with_rc(mpctx, EXIT_ERROR, 1);
        return NULL;
    }

    if (avc->oformat->flags & AVFMT_GLOBALHEADER)
        stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return stream;
}

AVCodec *encode_lavc_get_codec(AVStream *stream)
{
    switch (stream->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return vc;
    case AVMEDIA_TYPE_AUDIO:
        return ac;
    default:
        break;
    }
    return NULL;
}

int encode_lavc_open_codec(AVStream *stream)
{
    AVCodec *c = encode_lavc_get_codec(stream);
    if (c)
        return avcodec_open(stream->codec, c);
    else
        return -1;
}

void encode_lavc_write_stats(AVStream *stream)
{
    switch (stream->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (twopass_bytebuffer_v)
            if (stream->codec->stats_out)
                stream_write_buffer(twopass_bytebuffer_v, stream->codec->stats_out, strlen(stream->codec->stats_out));
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (twopass_bytebuffer_a)
            if (stream->codec->stats_out)
                stream_write_buffer(twopass_bytebuffer_a, stream->codec->stats_out, strlen(stream->codec->stats_out));
        break;
    default:
        break;
    }
}

int encode_lavc_write_frame(AVPacket *packet)
{
    int r;

    if (!avc)
        return -1;

    if (header_written <= 0)
        return -1;

    mp_msg(MSGT_VO, MSGL_DBG2, "encode-lavc: write frame: stream %d ptsi %d (%f) size %d\n",
        (int)packet->stream_index,
        (int)packet->pts,
        packet->pts * (double)avc->streams[packet->stream_index]->time_base.num / (double)avc->streams[packet->stream_index]->time_base.den,
        (int)packet->size);

    switch (avc->streams[packet->stream_index]->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        vbytes += packet->size;
        ++frames;
        break;
    case AVMEDIA_TYPE_AUDIO:
        abytes += packet->size;
        break;
    default:
        break;
    }

    r = av_interleaved_write_frame(avc, packet);

    return r;
}

int encode_lavc_supports_pixfmt(enum PixelFormat pix_fmt)
{
    if (!vc)
        return 0;
    if (pix_fmt == PIX_FMT_NONE)
        return 0;

    if (!vc->pix_fmts) {
        return VFCAP_CSP_SUPPORTED;
    } else {
        const enum PixelFormat *p;
        for (p = vc->pix_fmts; *p >= 0; ++p) {
            if (pix_fmt == *p)
                return VFCAP_CSP_SUPPORTED;
        }
    }
    return 0;
}

void encode_lavc_failtimesync(void)
{
    if (encode_lavc_testflag(ENCODE_LAVC_FLAG_COPYTS) || !avc || avc->nb_streams < 2)
        return;

    if (timesync_available > 0)
        timesync_available = -1;
}

int encode_lavc_timesyncfailed(void)
{
    if (encode_lavc_testflag(ENCODE_LAVC_FLAG_COPYTS) || !avc || avc->nb_streams < 2)
        return 0;

    return timesync_available < 0;
}

void encode_lavc_settimesync(double a_minus_v, double dt)
{
    double factor = dt * 1;
    double diff = fabs(a_minus_v - timesync_delta);

    if (encode_lavc_testflag(ENCODE_LAVC_FLAG_COPYTS) || !avc || avc->nb_streams < 2)
        return;

    // correct large diffs immediately
    if (diff > 1 || timesync_available <= 0) {
        mp_msg(MSGT_AO, MSGL_WARN, "encode-lavc: settimesync: %sjump from %f to %f\n", timesync_available == 0 ? "initial " : timesync_available < 0 ? "forced " : "discontinuity ", timesync_delta, a_minus_v);
        timesync_delta = a_minus_v;
        timesync_available = 1;
    } else {
        mp_msg(MSGT_AO, MSGL_DBG3, "encode-lavc: settimesync: adjust from %f to %f\n", timesync_delta, a_minus_v);
        timesync_delta = a_minus_v * factor + timesync_delta * (1 - factor);
    }
}

double encode_lavc_gettimesync(double initial_a_minus_v)
{
    if (encode_lavc_testflag(ENCODE_LAVC_FLAG_COPYTS) || !avc || avc->nb_streams < 2)
        return 0;

    if (timesync_available <= 0) {

        // if we have no audio stream, better pass through video pts as is instead of "syncing" by setting the initial pts to 0
        int i;
        for (i = 0; i < avc->nb_streams; ++i)
            if (avc->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
                break;
        if (i >= avc->nb_streams)
            initial_a_minus_v = 0;

        mp_msg(MSGT_AO, MSGL_INFO, "encode-lavc: settimesync: init from %f to %f\n", timesync_delta, initial_a_minus_v);
        timesync_delta = initial_a_minus_v;
        timesync_available = 1;
    }
    return timesync_delta;
}

static void encode_lavc_printoptions(void *obj, const char *indent, const char *subindent, const char *unit, int filter_and, int filter_eq)
{
    const AVOption *opt = NULL;
    char optbuf[32];
    while ((opt = av_next_option(obj, opt))) {
        if (opt->flags) // if flags are 0, it simply hasn't been filled in yet and may be potentially useful
            if ((opt->flags & filter_and) != filter_eq)
                continue;
        /* Don't print CONST's on level one.
         * Don't print anything but CONST's on level two.
         * Only print items from the requested unit.
         */
        if (!unit && opt->type==FF_OPT_TYPE_CONST)
            continue;
        else if (unit && opt->type!=FF_OPT_TYPE_CONST)
            continue;
        else if (unit && opt->type==FF_OPT_TYPE_CONST && strcmp(unit, opt->unit))
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
        optbuf[sizeof(optbuf)-1] = 0;
        mp_msg(MSGT_VO, MSGL_INFO, "%-32s ", optbuf);
        if (opt->help)
            mp_msg(MSGT_VO, MSGL_INFO, " %s", opt->help);
        mp_msg(MSGT_VO, MSGL_INFO, "\n");
        if (opt->unit && opt->type != FF_OPT_TYPE_CONST) {
            encode_lavc_printoptions(obj, indent, subindent, opt->unit, filter_and, filter_eq);
        }
    }
}

void encode_lavc_showhelp(enum encode_lavc_showhelp_type t)
{
    static int registered = 0;
    if (!registered) {
        avcodec_register_all();
        av_register_all();
        registered = 1;
    }
    switch (t) {
    case ENCODE_LAVC_SHOWHELP_F: {
        AVOutputFormat *c = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output formats:\n");
        while ((c = av_oformat_next(c))) {
            mp_msg(MSGT_VO, MSGL_INFO, "  -of %-13s %s\n", c->name, c->long_name ? c->long_name : "");
        }
        av_free(c);
        break;
    }
    case ENCODE_LAVC_SHOWHELP_FOPTS: {
        AVFormatContext *c = avformat_alloc_context();
        AVOutputFormat *format = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output format options:\n");
        encode_lavc_printoptions(c, "  -ofopts ", "          ", NULL, AV_OPT_FLAG_ENCODING_PARAM, AV_OPT_FLAG_ENCODING_PARAM);
        av_free(c);
        while ((format = av_oformat_next(format))) {
            if (format->priv_class) {
                mp_msg(MSGT_VO, MSGL_INFO, "Additionally, for -of %s:\n", format->name);
                encode_lavc_printoptions(&format->priv_class, "  -ofopts ", "          ", NULL, AV_OPT_FLAG_ENCODING_PARAM, AV_OPT_FLAG_ENCODING_PARAM);
            }
        }
        break;
    }
    case ENCODE_LAVC_SHOWHELP_VCOPTS: {
        AVCodecContext *c = avcodec_alloc_context();
        AVCodec *codec = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output video codec options:\n");
        encode_lavc_printoptions(c, "  -ovcopts ", "           ", NULL, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM);
        av_free(c);
        while ((codec = av_codec_next(codec))) {
            if (!codec->encode)
                continue;
            if (codec->type != AVMEDIA_TYPE_VIDEO)
                continue;
            if (codec->priv_class) {
                mp_msg(MSGT_VO, MSGL_INFO, "Additionally, for -ovc %s:\n", codec->name);
                encode_lavc_printoptions(&codec->priv_class, "  -ovcopts ", "           ", NULL, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM);
            }
        }
        mp_msg(MSGT_VO, MSGL_INFO, "Additionally, for -ovc libx264:\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=ultrafast                  preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=superfast                  preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=veryfast                   preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=faster                     preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=fast                       preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=medium                     preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=slow                       preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=slower                     preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=veryslow                   preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts preset=placebo                    preset for encoding speed\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=film                         tuning for input source\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=animation                    tuning for input source\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=grain                        tuning for input source\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=stillimage                   tuning for input source\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=psnr                         tuning for input source\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=ssim                         tuning for input source\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=fastdecode                   tuning for output device\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts tune=zerolatency                  tuning for output device\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts profile=baseline                  profile for decoder requirements\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts profile=main                      profile for decoder requirements\n");
        mp_msg(MSGT_VO, MSGL_INFO, "  -ovcopts profile=high                      profile for decoder requirements\n");
        break;
    }
    case ENCODE_LAVC_SHOWHELP_ACOPTS: {
        AVCodecContext *c = avcodec_alloc_context();
        AVCodec *codec = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output audio codec options:\n");
        encode_lavc_printoptions(c, "  -oacopts ", "           ", NULL, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM);
        av_free(c);
        while ((codec = av_codec_next(codec))) {
            if (!codec->encode)
                continue;
            if (codec->type != AVMEDIA_TYPE_AUDIO)
                continue;
            if (codec->priv_class) {
                mp_msg(MSGT_VO, MSGL_INFO, "Additionally, for -oac %s:\n", codec->name);
                encode_lavc_printoptions(&codec->priv_class, "  -oacopts ", "           ", NULL, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM);
            }
        }
        break;
    }
    case ENCODE_LAVC_SHOWHELP_VC: {
        AVCodec *c = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output video codecs:\n");
        while ((c = av_codec_next(c))) {
            if (!c->encode)
                continue;
            if (c->type != AVMEDIA_TYPE_VIDEO)
                continue;
            mp_msg(MSGT_VO, MSGL_INFO, "  -ovc %-12s %s\n", c->name, c->long_name ? c->long_name : "");
            if (c->priv_class)
                encode_lavc_printoptions(&c->priv_class, "    -ovcopts ", "             ", NULL, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM);
        }
        av_free(c);
        break;
    }
    case ENCODE_LAVC_SHOWHELP_AC: {
        AVCodec *c = NULL;
        mp_msg(MSGT_VO, MSGL_INFO, "Available output audio codecs:\n");
        while ((c = av_codec_next(c))) {
            if (!c->encode)
                continue;
            if (c->type != AVMEDIA_TYPE_AUDIO)
                continue;
            mp_msg(MSGT_VO, MSGL_INFO, "  -oac %-12s %s\n", c->name, c->long_name ? c->long_name : "");
            if (c->priv_class)
                encode_lavc_printoptions(&c->priv_class, "    -oacopts ", "             ", NULL, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM);
        }
        av_free(c);
        break;
    }
    }
    return;
}

int encode_lavc_testflag(int flag)
{
    if (!options)
        return 0;
    switch(flag)
    {
    case ENCODE_LAVC_FLAG_HARDDUP:
        return options->harddup;
    case ENCODE_LAVC_FLAG_COPYTS:
        return options->copyts;
    case ENCODE_LAVC_FLAG_NEVERDROP:
        return options->neverdrop;
    default:
        break;
    }
    return 0;
}

double encode_lavc_getoffset(AVStream *stream)
{
    switch (stream->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return options->voffset;
    case AVMEDIA_TYPE_AUDIO:
        return options->aoffset;
    default:
        break;
    }
    return 0;
}

const char *encode_lavc_getstatus(float relative_position, float playback_time)
{
    static char buf[80];
    float minutes, megabytes, fps, x;
    float f = max(0.0001, relative_position);
    if (!avc)
        return NULL;
    minutes = (GetTimerMS() - t0) / 60000.0 * (1-f) / f;
    megabytes = avc->pb ? (avio_size(avc->pb) / 1048576.0 / f) : 0;
    fps = frames / ((GetTimerMS() - t0) / 1000.0);
    x = playback_time / ((GetTimerMS() - t0) / 1000.0);
    if (frames)
        snprintf(buf, sizeof(buf), "{%.1f%% %.1fmin %.1ffps %.1fMB}", relative_position * 100.0, minutes, fps, megabytes);
    else
        snprintf(buf, sizeof(buf), "{%.1f%% %.1fmin %.2fx %.1fMB}", relative_position * 100.0, minutes, x, megabytes);
    buf[sizeof(buf)-1] = 0;
    return buf;
}

// vim: ts=4 sw=4 et
