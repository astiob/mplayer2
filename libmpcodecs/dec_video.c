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

#include "config.h"
#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#include "mp_msg.h"

#include "osdep/timer.h"
#include "osdep/shmem.h"

#include "stream/stream.h"
#include "libmpdemux/demuxer.h"
#include "libmpdemux/parse_es.h"

#include "codec-cfg.h"

#include "libvo/video_out.h"
#include "libvo/csputils.h"

#include "libmpdemux/stheader.h"
#include "vd.h"
#include "vf.h"

#include "dec_video.h"

// ===================================================================

extern double video_time_usage;
extern double vout_time_usage;

#include "cpudetect.h"

int field_dominance = -1;

int divx_quality = 0;

int get_video_quality_max(sh_video_t *sh_video)
{
    vf_instance_t *vf = sh_video->vfilter;
    if (vf) {
        int ret = vf->control(vf, VFCTRL_QUERY_MAX_PP_LEVEL, NULL);
        if (ret > 0) {
            mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "[PP] Using external postprocessing filter, max q = %d.\n", ret);
            return ret;
        }
    }
    const struct vd_functions *vd = sh_video->vd_driver;
    if (vd) {
        int ret = vd->control(sh_video, VDCTRL_QUERY_MAX_PP_LEVEL, NULL);
        if (ret > 0) {
            mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "[PP] Using codec's postprocessing, max q = %d.\n", ret);
            return ret;
        }
    }
    return 0;
}

void set_video_quality(sh_video_t *sh_video, int quality)
{
    vf_instance_t *vf = sh_video->vfilter;
    if (vf) {
        int ret = vf->control(vf, VFCTRL_SET_PP_LEVEL, (void *) (&quality));
        if (ret == CONTROL_TRUE)
            return;             // success
    }
    const struct vd_functions *vd = sh_video->vd_driver;
    if (vd)
        vd->control(sh_video, VDCTRL_SET_PP_LEVEL, (void *) (&quality));
}

int set_video_colors(sh_video_t *sh_video, const char *item, int value)
{
    vf_instance_t *vf = sh_video->vfilter;
    vf_equalizer_t data;

    data.item = item;
    data.value = value;

    mp_dbg(MSGT_DECVIDEO, MSGL_V, "set video colors %s=%d \n", item, value);
    if (vf) {
        int ret = vf->control(vf, VFCTRL_SET_EQUALIZER, &data);
        if (ret == CONTROL_TRUE)
            return 1;
    }
    /* try software control */
    const struct vd_functions *vd = sh_video->vd_driver;
    if (vd &&
        vd->control(sh_video, VDCTRL_SET_EQUALIZER, (void *)item, value)
            == CONTROL_OK)
        return 1;
    mp_tmsg(MSGT_DECVIDEO, MSGL_V, "Video attribute '%s' is not supported by selected vo & vd.\n",
           item);
    return 0;
}

int get_video_colors(sh_video_t *sh_video, const char *item, int *value)
{
    vf_instance_t *vf = sh_video->vfilter;
    vf_equalizer_t data;

    data.item = item;

    mp_dbg(MSGT_DECVIDEO, MSGL_V, "get video colors %s \n", item);
    if (vf) {
        int ret = vf->control(vf, VFCTRL_GET_EQUALIZER, &data);
        if (ret == CONTROL_TRUE) {
            *value = data.value;
            return 1;
        }
    }
    /* try software control */
    const struct vd_functions *vd = sh_video->vd_driver;
    if (vd)
        return vd->control(sh_video, VDCTRL_GET_EQUALIZER, (void *)item, value);
    return 0;
}

void get_detected_video_colorspace(struct sh_video *sh,
                                   struct mp_csp_details *csp,
                                   struct mp_csp_rgb *rgb)
{
    struct MPOpts *opts = sh->opts;
    struct vf_instance *vf = sh->vfilter;

    rgb->primaries = opts->requested_color_primaries;
    csp->format = opts->requested_colorspace;
    csp->levels_in = opts->requested_input_range;
    csp->levels_out = opts->requested_output_range;
    csp->chroma_loc = opts->requested_chroma_sample_location;

    if (rgb->primaries == MP_CPRIM_AUTO)
        rgb->primaries = sh->color_primaries;
    if (rgb->primaries == MP_CPRIM_AUTO)
        rgb->primaries = mp_csp_guess_color_primaries(vf->w, vf->h);

    if (csp->format == MP_CSP_AUTO)
        csp->format = sh->colorspace;
    if (csp->format == MP_CSP_AUTO)
        csp->format = mp_csp_guess_colorspace(vf->w, vf->h);

    if (csp->levels_in == MP_CSP_LEVELS_AUTO)
        csp->levels_in = sh->color_range;
    if (csp->levels_in == MP_CSP_LEVELS_AUTO)
        csp->levels_in = MP_CSP_LEVELS_TV;

    if (csp->levels_out == MP_CSP_LEVELS_AUTO)
        csp->levels_out = MP_CSP_LEVELS_PC;

    if (csp->chroma_loc == MP_CHROMA_LOC_AUTO)
        csp->chroma_loc = sh->chroma_sample_location;
    if (csp->chroma_loc == MP_CHROMA_LOC_AUTO)
        csp->chroma_loc = MP_CHROMA_LOC_CENTER;
}

void set_video_colorspace(struct sh_video *sh)
{
    struct vf_instance *vf = sh->vfilter;

    struct mp_csp_details requested_yuv;
    struct mp_csp_rgb requested_rgb;
    get_detected_video_colorspace(sh, &requested_yuv, &requested_rgb);
    vf->control(vf, VFCTRL_SET_YUV_COLORSPACE, &requested_yuv);
    vf->control(vf, VFCTRL_SET_RGB_COLORSPACE, &requested_rgb);

    struct mp_csp_details actual_yuv = MP_CSP_DETAILS_DEFAULTS;
    struct mp_csp_rgb actual_rgb = MP_CSP_RGB_DEFAULTS;
    vf->control(vf, VFCTRL_GET_YUV_COLORSPACE, &actual_yuv);
    vf->control(vf, VFCTRL_GET_RGB_COLORSPACE, &actual_rgb);

    int success = actual_yuv.format == requested_yuv.format
               && actual_yuv.levels_in == requested_yuv.levels_in
               && actual_yuv.levels_out == requested_yuv.levels_out
               && actual_yuv.chroma_loc == requested_yuv.chroma_loc
               && actual_rgb.primaries == requested_rgb.primaries;

    if (!success)
        mp_tmsg(MSGT_DECVIDEO, MSGL_WARN,
                "Colorspace details not fully supported by selected vo.\n");

    if (actual_yuv.format != requested_yuv.format
            && requested_yuv.format == MP_CSP_SMPTE_240M) {
        // BT.709 is pretty close, much better than BT.601
        requested_yuv.format = MP_CSP_BT_709;
        vf->control(vf, VFCTRL_SET_YUV_COLORSPACE, &requested_yuv);
    }

}

int set_rectangle(sh_video_t *sh_video, int param, int value)
{
    vf_instance_t *vf = sh_video->vfilter;
    int data[] = { param, value };

    mp_dbg(MSGT_DECVIDEO, MSGL_V, "set rectangle \n");
    if (vf) {
        int ret = vf->control(vf, VFCTRL_CHANGE_RECTANGLE, data);
        if (ret)
            return 1;
    }
    return 0;
}

void resync_video_stream(sh_video_t *sh_video)
{
    const struct vd_functions *vd = sh_video->vd_driver;
    if (vd)
        vd->control(sh_video, VDCTRL_RESYNC_STREAM, NULL);
    sh_video->prev_codec_reordered_pts = MP_NOPTS_VALUE;
    sh_video->prev_sorted_pts = MP_NOPTS_VALUE;
}

void video_reset_aspect(struct sh_video *sh_video)
{
    int r = sh_video->vd_driver->control(sh_video, VDCTRL_RESET_ASPECT, NULL);
    if (r != true)
        mpcodecs_config_vo(sh_video, sh_video->disp_w, sh_video->disp_h, 0);
}

int get_current_video_decoder_lag(sh_video_t *sh_video)
{
    const struct vd_functions *vd = sh_video->vd_driver;
    if (!vd)
        return -1;
    int ret = vd->control(sh_video, VDCTRL_QUERY_UNSEEN_FRAMES, NULL);
    if (ret >= 10)
        return ret - 10;
    return -1;
}

void uninit_video(sh_video_t *sh_video)
{
    if (!sh_video->initialized)
        return;
    mp_tmsg(MSGT_DECVIDEO, MSGL_V, "Uninit video: %s\n", sh_video->codec->drv);
    sh_video->vd_driver->uninit(sh_video);
    vf_uninit_filter_chain(sh_video->vfilter);
    sh_video->initialized = 0;
}

void vfm_help(void)
{
    int i;
    mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "Available (compiled-in) video codec families/drivers:\n");
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_VIDEO_DRIVERS\n");
    mp_msg(MSGT_DECVIDEO, MSGL_INFO, "   vfm:    info:  (comment)\n");
    for (i = 0; mpcodecs_vd_drivers[i] != NULL; i++)
        mp_msg(MSGT_DECVIDEO, MSGL_INFO, "%8s  %s (%s)\n",
               mpcodecs_vd_drivers[i]->info->short_name,
               mpcodecs_vd_drivers[i]->info->name,
               mpcodecs_vd_drivers[i]->info->comment);
}

static int init_video(sh_video_t *sh_video, char *codecname, char *vfm,
                      int status, stringset_t *selected)
{
    int force = 0;
    unsigned int orig_fourcc =
        sh_video->bih ? sh_video->bih->biCompression : 0;
    sh_video->codec = NULL;
    sh_video->vf_initialized = 0;
    if (codecname && codecname[0] == '+') {
        codecname = &codecname[1];
        force = 1;
    }

    while (1) {
        int i;
        int orig_w, orig_h;
        // restore original fourcc:
        if (sh_video->bih)
            sh_video->bih->biCompression = orig_fourcc;
        if (!
            (sh_video->codec =
             find_video_codec(sh_video->format,
                              sh_video->bih ? ((unsigned int *) &sh_video->
                                               bih->biCompression) : NULL,
                              sh_video->codec, force)))
            break;
        // ok we found one codec
        if (stringset_test(selected, sh_video->codec->name))
            continue;           // already tried & failed
        if (codecname && strcmp(sh_video->codec->name, codecname))
            continue;           // -vc
        if (vfm && strcmp(sh_video->codec->drv, vfm))
            continue;           // vfm doesn't match
        if (!force && sh_video->codec->status < status)
            continue;           // too unstable
        stringset_add(selected, sh_video->codec->name); // tagging it
        // ok, it matches all rules, let's find the driver!
        for (i = 0; mpcodecs_vd_drivers[i] != NULL; i++)
            if (!strcmp(mpcodecs_vd_drivers[i]->info->short_name,
                        sh_video->codec->drv))
                break;
        sh_video->vd_driver = mpcodecs_vd_drivers[i];
        if (!sh_video->vd_driver) {    // driver not available (==compiled in)
            mp_tmsg(MSGT_DECVIDEO, MSGL_WARN,
                   _("Requested video codec family [%s] (vfm=%s) not available.\nEnable it at compilation.\n"),
                   sh_video->codec->name, sh_video->codec->drv);
            continue;
        }
        orig_w = sh_video->bih ? sh_video->bih->biWidth : sh_video->disp_w;
        orig_h = sh_video->bih ? sh_video->bih->biHeight : sh_video->disp_h;
        sh_video->disp_w = orig_w;
        sh_video->disp_h = orig_h;
        // it's available, let's try to init!
        if (sh_video->codec->flags & CODECS_FLAG_ALIGN16) {
            // align width/height to n*16
            sh_video->disp_w = (sh_video->disp_w + 15) & (~15);
            sh_video->disp_h = (sh_video->disp_h + 15) & (~15);
        }
        if (sh_video->bih) {
            sh_video->bih->biWidth = sh_video->disp_w;
            sh_video->bih->biHeight = sh_video->disp_h;
        }

        // init()
        const struct vd_functions *vd = sh_video->vd_driver;
        mp_tmsg(MSGT_DECVIDEO, MSGL_V, "Opening video decoder: [%s] %s\n",
               vd->info->short_name, vd->info->name);
        // clear vf init error, it is no longer relevant
        if (sh_video->vf_initialized < 0)
            sh_video->vf_initialized = 0;
        if (!vd->init(sh_video)) {
            mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "Video decoder init failed for "
                    "codecs.conf entry \"%s\".\n", sh_video->codec->name);
            sh_video->disp_w = orig_w;
            sh_video->disp_h = orig_h;
            if (sh_video->bih) {
                sh_video->bih->biWidth = sh_video->disp_w;
                sh_video->bih->biHeight = sh_video->disp_h;
            }
            continue;           // try next...
        }
        // Yeah! We got it!
        sh_video->initialized = 1;
        sh_video->prev_codec_reordered_pts = MP_NOPTS_VALUE;
        sh_video->prev_sorted_pts = MP_NOPTS_VALUE;
        return 1;
    }
    return 0;
}

int init_best_video_codec(sh_video_t *sh_video, char **video_codec_list,
                          char **video_fm_list)
{
    char *vc_l_default[2] = { "", (char *) NULL };
    stringset_t selected;
    // hack:
    if (!video_codec_list)
        video_codec_list = vc_l_default;
    // Go through the codec.conf and find the best codec...
    sh_video->initialized = 0;
    stringset_init(&selected);
    while (!sh_video->initialized && *video_codec_list) {
        char *video_codec = *(video_codec_list++);
        if (video_codec[0]) {
            if (video_codec[0] == '-') {
                // disable this codec:
                stringset_add(&selected, video_codec + 1);
            } else {
                // forced codec by name:
                mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "Forced video codec: %s\n",
                       video_codec);
                init_video(sh_video, video_codec, NULL, -1, &selected);
            }
        } else {
            int status;
            // try in stability order: UNTESTED, WORKING, BUGGY. never try CRASHING.
            if (video_fm_list) {
                char **fmlist = video_fm_list;
                // try first the preferred codec families:
                while (!sh_video->initialized && *fmlist) {
                    char *video_fm = *(fmlist++);
                    mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "Trying to force video codec driver family %s...\n",
                           video_fm);
                    for (status = CODECS_STATUS__MAX;
                         status >= CODECS_STATUS__MIN; --status)
                        if (init_video
                            (sh_video, NULL, video_fm, status, &selected))
                            break;
                }
            }
            if (!sh_video->initialized)
                for (status = CODECS_STATUS__MAX; status >= CODECS_STATUS__MIN;
                     --status)
                    if (init_video(sh_video, NULL, NULL, status, &selected))
                        break;
        }
    }
    stringset_free(&selected);

    if (!sh_video->initialized) {
        mp_tmsg(MSGT_DECVIDEO, MSGL_ERR, "Cannot find codec matching selected -vo and video format 0x%X.\n",
               sh_video->format);
        return 0;               // failed
    }

    mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "Selected video codec: %s [%s]\n",
            sh_video->codecname ? sh_video->codecname : sh_video->codec->info,
            sh_video->vd_driver->info->print_name ?
            sh_video->vd_driver->info->print_name :
            sh_video->vd_driver->info->short_name);
    mp_tmsg(MSGT_DECVIDEO, MSGL_V,
            "Video codecs.conf entry: %s (%s)  vfm: %s\n",
            sh_video->codec->name, sh_video->codec->info, sh_video->codec->drv);
    return 1;                   // success
}

void *decode_video(sh_video_t *sh_video, struct demux_packet *packet,
                   unsigned char *start, int in_size,
                   int drop_frame, double pts)
{
    mp_image_t *mpi = NULL;
    unsigned int t = GetTimer();
    unsigned int t2;
    double tt;
    struct MPOpts *opts = sh_video->opts;

    if (opts->correct_pts && pts != MP_NOPTS_VALUE) {
        int delay = get_current_video_decoder_lag(sh_video);
        if (delay >= 0) {
            if (delay > sh_video->num_buffered_pts)
#if 0
                // this is disabled because vd_ffmpeg reports the same lag
                // after seek even when there are no buffered frames,
                // leading to incorrect error messages
                mp_msg(MSGT_DECVIDEO, MSGL_ERR, "Not enough buffered pts\n");
#else
                ;
#endif
            else
                sh_video->num_buffered_pts = delay;
        }
        if (sh_video->num_buffered_pts ==
            sizeof(sh_video->buffered_pts) / sizeof(double))
            mp_msg(MSGT_DECVIDEO, MSGL_ERR, "Too many buffered pts\n");
        else {
            int i, j;
            for (i = 0; i < sh_video->num_buffered_pts; i++)
                if (sh_video->buffered_pts[i] < pts)
                    break;
            for (j = sh_video->num_buffered_pts; j > i; j--)
                sh_video->buffered_pts[j] = sh_video->buffered_pts[j - 1];
            sh_video->buffered_pts[i] = pts;
            sh_video->num_buffered_pts++;
        }
    }

    if (sh_video->vd_driver->decode2) {
        mpi = sh_video->vd_driver->decode2(sh_video, packet, start, in_size,
                                           drop_frame, &pts);
    } else {
        mpi = sh_video->vd_driver->decode(sh_video, start, in_size,
                                          drop_frame);
        pts = MP_NOPTS_VALUE;
    }

    //------------------------ frame decoded. --------------------

#if HAVE_MMX
    // some codecs are broken, and doesn't restore MMX state :(
    // it happens usually with broken/damaged files.
    if (gCpuCaps.has3DNow) {
        __asm__ volatile("femms\n\t":::"memory");
    } else if (gCpuCaps.hasMMX) {
        __asm__ volatile("emms\n\t":::"memory");
    }
#endif

    t2 = GetTimer();
    t = t2 - t;
    tt = t * 0.000001f;
    video_time_usage += tt;

    if (!mpi || drop_frame)
        return NULL;            // error / skipped frame

    if (field_dominance == 0)
        mpi->fields |= MP_IMGFIELD_TOP_FIRST;
    else if (field_dominance == 1)
        mpi->fields &= ~MP_IMGFIELD_TOP_FIRST;

    double prevpts = sh_video->codec_reordered_pts;
    sh_video->prev_codec_reordered_pts = prevpts;
    sh_video->codec_reordered_pts = pts;
    if (prevpts != MP_NOPTS_VALUE && pts <= prevpts
        || pts == MP_NOPTS_VALUE)
        sh_video->num_reordered_pts_problems++;
    prevpts = sh_video->sorted_pts;
    if (opts->correct_pts) {
        if (sh_video->num_buffered_pts) {
            sh_video->num_buffered_pts--;
            sh_video->sorted_pts =
                sh_video->buffered_pts[sh_video->num_buffered_pts];
        } else {
            mp_msg(MSGT_CPLAYER, MSGL_ERR,
                   "No pts value from demuxer to use for frame!\n");
            sh_video->sorted_pts = MP_NOPTS_VALUE;
        }
    }
    pts = sh_video->sorted_pts;
    if (prevpts != MP_NOPTS_VALUE && pts <= prevpts
        || pts == MP_NOPTS_VALUE)
        sh_video->num_sorted_pts_problems++;
    return mpi;
}

int filter_video(sh_video_t *sh_video, void *frame, double pts)
{
    mp_image_t *mpi = frame;
    unsigned int t2 = GetTimer();
    vf_instance_t *vf = sh_video->vfilter;
    // apply video filters and call the leaf vo/ve
    int ret = vf->put_image(vf, mpi, pts);

    t2 = GetTimer() - t2;
    vout_time_usage += t2 * 0.000001;

    return ret;
}
