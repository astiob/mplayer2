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

#include <libavformat/avformat.h>

#include "config.h"
#include "mp_taglists.h"

struct tag {
    enum AVCodecID id;
    unsigned int tag;
};

static const struct tag mp_wav_tags[] = {
    { AV_CODEC_ID_ADPCM_4XM,         MKTAG('4', 'X', 'M', 'A')},
    { AV_CODEC_ID_ADPCM_ADX,         MKTAG('S', 'a', 'd', 'x')},
    { AV_CODEC_ID_ADPCM_EA,          MKTAG('A', 'D', 'E', 'A')},
    { AV_CODEC_ID_ADPCM_EA_MAXIS_XA, MKTAG('A', 'D', 'X', 'A')},
    { AV_CODEC_ID_ADPCM_IMA_WS,      MKTAG('A', 'I', 'W', 'S')},
    { AV_CODEC_ID_ADPCM_THP,         MKTAG('T', 'H', 'P', 'A')},
    { AV_CODEC_ID_ADPCM_XA,          MKTAG('P', 'S', 'X', 'A')},
    { AV_CODEC_ID_AMR_NB,            MKTAG('n', 'b',   0,   0)},
    { AV_CODEC_ID_COOK,              MKTAG('c', 'o', 'o', 'k')},
    { AV_CODEC_ID_DSICINAUDIO,       MKTAG('D', 'C', 'I', 'A')},
    { AV_CODEC_ID_EAC3,              MKTAG('E', 'A', 'C', '3')},
    { AV_CODEC_ID_INTERPLAY_DPCM,    MKTAG('I', 'N', 'P', 'A')},
    { AV_CODEC_ID_MLP,               MKTAG('M', 'L', 'P', ' ')},
    { AV_CODEC_ID_MP1,               0x50},
    { AV_CODEC_ID_MP4ALS,            MKTAG('A', 'L', 'S', ' ')},
    { AV_CODEC_ID_MUSEPACK7,         MKTAG('M', 'P', 'C', ' ')},
    { AV_CODEC_ID_MUSEPACK8,         MKTAG('M', 'P', 'C', '8')},
    { AV_CODEC_ID_NELLYMOSER,        MKTAG('N', 'E', 'L', 'L')},
    { AV_CODEC_ID_PCM_LXF,           MKTAG('P', 'L', 'X', 'F')},
    { AV_CODEC_ID_QCELP,             MKTAG('Q', 'c', 'l', 'p')},
    { AV_CODEC_ID_QDM2,              MKTAG('Q', 'D', 'M', '2')},
    { AV_CODEC_ID_RA_144,            MKTAG('1', '4', '_', '4')},
    { AV_CODEC_ID_RA_288,            MKTAG('2', '8', '_', '8')},
    { AV_CODEC_ID_ROQ_DPCM,          MKTAG('R', 'o', 'Q', 'A')},
    { AV_CODEC_ID_SHORTEN,           MKTAG('s', 'h', 'r', 'n')},
    { AV_CODEC_ID_SPEEX,             MKTAG('s', 'p', 'x', ' ')},
    { AV_CODEC_ID_TTA,               MKTAG('T', 'T', 'A', '1')},
    { AV_CODEC_ID_TWINVQ,            MKTAG('T', 'W', 'I', '2')},
    { AV_CODEC_ID_WAVPACK,           MKTAG('W', 'V', 'P', 'K')},
    { AV_CODEC_ID_WESTWOOD_SND1,     MKTAG('S', 'N', 'D', '1')},
    { AV_CODEC_ID_XAN_DPCM,          MKTAG('A', 'x', 'a', 'n')},
    { 0, 0 },
};

static const struct tag mp_codecid_override_tags[] = {
    { AV_CODEC_ID_AAC,               MKTAG('M', 'P', '4', 'A')},
    { AV_CODEC_ID_AAC_LATM,          MKTAG('M', 'P', '4', 'L')},
    { AV_CODEC_ID_AC3,               0x2000},
    { AV_CODEC_ID_ADPCM_IMA_AMV,     MKTAG('A', 'M', 'V', 'A')},
    { AV_CODEC_ID_BINKAUDIO_DCT,     MKTAG('B', 'A', 'U', '1')},
    { AV_CODEC_ID_BINKAUDIO_RDFT,    MKTAG('B', 'A', 'U', '2')},
    { AV_CODEC_ID_DTS,               0x2001},
    { AV_CODEC_ID_DVVIDEO,           MKTAG('d', 'v', 's', 'd')},
    { AV_CODEC_ID_EAC3,              MKTAG('E', 'A', 'C', '3')},
    { AV_CODEC_ID_H264,              MKTAG('H', '2', '6', '4')},
    { AV_CODEC_ID_MPEG4,             MKTAG('M', 'P', '4', 'V')},
    { AV_CODEC_ID_PCM_BLURAY,        MKTAG('B', 'P', 'C', 'M')},
    { AV_CODEC_ID_PCM_S8,            MKTAG('t', 'w', 'o', 's')},
    { AV_CODEC_ID_PCM_U8,            1},
    { AV_CODEC_ID_PCM_S16BE,         MKTAG('t', 'w', 'o', 's')},
    { AV_CODEC_ID_PCM_S16LE,         1},
    { AV_CODEC_ID_PCM_S24BE,         MKTAG('i', 'n', '2', '4')},
    { AV_CODEC_ID_PCM_S24LE,         1},
    { AV_CODEC_ID_PCM_S32BE,         MKTAG('i', 'n', '3', '2')},
    { AV_CODEC_ID_PCM_S32LE,         1},
    { AV_CODEC_ID_MP2,               0x50},
    { AV_CODEC_ID_MPEG2VIDEO,        MKTAG('M', 'P', 'G', '2')},
    { AV_CODEC_ID_TRUEHD,            MKTAG('T', 'R', 'H', 'D')},
    { 0, 0 },
};

static const struct tag mp_bmp_tags[] = {
    { AV_CODEC_ID_AMV,               MKTAG('A', 'M', 'V', 'V')},
    { AV_CODEC_ID_ANM,               MKTAG('A', 'N', 'M', ' ')},
    { AV_CODEC_ID_AVS,               MKTAG('A', 'V', 'S', ' ')},
    { AV_CODEC_ID_BETHSOFTVID,       MKTAG('B', 'E', 'T', 'H')},
    { AV_CODEC_ID_BFI,               MKTAG('B', 'F', 'I', 'V')},
    { AV_CODEC_ID_C93,               MKTAG('C', '9', '3', 'V')},
    { AV_CODEC_ID_CDGRAPHICS,        MKTAG('C', 'D', 'G', 'R')},
    { AV_CODEC_ID_DNXHD,             MKTAG('A', 'V', 'd', 'n')},
    { AV_CODEC_ID_DSICINVIDEO,       MKTAG('D', 'C', 'I', 'V')},
    { AV_CODEC_ID_DXA,               MKTAG('D', 'X', 'A', '1')},
    { AV_CODEC_ID_FLIC,              MKTAG('F', 'L', 'I', 'C')},
    { AV_CODEC_ID_IDCIN,             MKTAG('I', 'D', 'C', 'I')},
    { AV_CODEC_ID_INTERPLAY_VIDEO,   MKTAG('I', 'N', 'P', 'V')},
    { AV_CODEC_ID_JV,                MKTAG('F', 'F', 'J', 'V')},
    { AV_CODEC_ID_MDEC,              MKTAG('M', 'D', 'E', 'C')},
    { AV_CODEC_ID_MOTIONPIXELS,      MKTAG('M', 'V', 'I', '1')},
    { AV_CODEC_ID_NUV,               MKTAG('N', 'U', 'V', '1')},
    { AV_CODEC_ID_RL2,               MKTAG('R', 'L', '2', 'V')},
    { AV_CODEC_ID_ROQ,               MKTAG('R', 'o', 'Q', 'V')},
    { AV_CODEC_ID_RV10,              MKTAG('R', 'V', '1', '0')},
    { AV_CODEC_ID_RV20,              MKTAG('R', 'V', '2', '0')},
    { AV_CODEC_ID_RV30,              MKTAG('R', 'V', '3', '0')},
    { AV_CODEC_ID_RV40,              MKTAG('R', 'V', '4', '0')},
    { AV_CODEC_ID_SVQ3,              MKTAG('S', 'V', 'Q', '3')},
    { AV_CODEC_ID_TGV,               MKTAG('f', 'V', 'G', 'T')},
    { AV_CODEC_ID_THP,               MKTAG('T', 'H', 'P', 'V')},
    { AV_CODEC_ID_TIERTEXSEQVIDEO,   MKTAG('T', 'S', 'E', 'Q')},
    { AV_CODEC_ID_TXD,               MKTAG('T', 'X', 'D', 'V')},
    { AV_CODEC_ID_VP6A,              MKTAG('V', 'P', '6', 'A')},
    { AV_CODEC_ID_VMDVIDEO,          MKTAG('V', 'M', 'D', 'V')},
    { AV_CODEC_ID_WS_VQA,            MKTAG('V', 'Q', 'A', 'V')},
    { AV_CODEC_ID_XAN_WC3,           MKTAG('W', 'C', '3', 'V')},
    { 0, 0 },
};

static unsigned int codec_get_tag(const struct tag *tags, enum AVCodecID id)
{
    while (tags->id != AV_CODEC_ID_NONE) {
        if (tags->id == id)
            return tags->tag;
        tags++;
    }
    return 0;
}

unsigned int mp_taglist_override(enum AVCodecID id)
{
    return codec_get_tag(mp_codecid_override_tags, id);
}

unsigned int mp_taglist_video(enum AVCodecID id)
{
    const struct AVCodecTag *tags[] = {avformat_get_riff_video_tags(), NULL };
    unsigned int tag = av_codec_get_tag(tags, id);
    if (tag)
        return tag;
    return codec_get_tag(mp_bmp_tags, id);
}

unsigned int mp_taglist_audio(enum AVCodecID id)
{
    const struct AVCodecTag *tags[] = {avformat_get_riff_audio_tags(), NULL };
    unsigned int tag = av_codec_get_tag(tags, id);
    if (tag)
        return tag;
    return codec_get_tag(mp_wav_tags, id);
}
