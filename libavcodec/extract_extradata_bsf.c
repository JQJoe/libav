/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "bsf.h"
#include "h2645_parse.h"
#include "h264.h"
#include "hevc.h"
#include "vc1_common.h"

typedef struct ExtractExtradataContext {
    const AVClass *class;

    int (*extract)(AVBSFContext *ctx, AVPacket *pkt,
                   uint8_t **data, int *size);

    /* AVOptions */
    int remove;
} ExtractExtradataContext;

static int val_in_array(const int *arr, int len, int val)
{
    int i;
    for (i = 0; i < len; i++)
        if (arr[i] == val)
            return 1;
    return 0;
}

static int extract_extradata_h2645(AVBSFContext *ctx, AVPacket *pkt,
                                   uint8_t **data, int *size)
{
    static const int extradata_nal_types_hevc[] = {
        HEVC_NAL_VPS, HEVC_NAL_SPS, HEVC_NAL_PPS,
    };
    static const int extradata_nal_types_h264[] = {
        H264_NAL_SPS, H264_NAL_PPS,
    };

    ExtractExtradataContext *s = ctx->priv_data;

    H2645Packet h2645_pkt = { 0 };
    int extradata_size = 0;
    const int *extradata_nal_types;
    int nb_extradata_nal_types;
    int i, ret = 0;

    if (ctx->par_in->codec_id == AV_CODEC_ID_HEVC) {
        extradata_nal_types    = extradata_nal_types_hevc;
        nb_extradata_nal_types = FF_ARRAY_ELEMS(extradata_nal_types_hevc);
    } else {
        extradata_nal_types    = extradata_nal_types_h264;
        nb_extradata_nal_types = FF_ARRAY_ELEMS(extradata_nal_types_h264);
    }

    ret = ff_h2645_packet_split(&h2645_pkt, pkt->data, pkt->size,
                                ctx, 0, 0, ctx->par_in->codec_id);
    if (ret < 0)
        return ret;

    for (i = 0; i < h2645_pkt.nb_nals; i++) {
        H2645NAL *nal = &h2645_pkt.nals[i];
        if (val_in_array(extradata_nal_types, nb_extradata_nal_types, nal->type))
            extradata_size += nal->raw_size + 3;
    }

    if (extradata_size) {
        AVBufferRef *filtered_buf;
        uint8_t *extradata, *filtered_data;

        if (s->remove) {
            filtered_buf = av_buffer_alloc(pkt->size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!filtered_buf)
                goto fail;
            filtered_data = filtered_buf->data;
        }

        extradata = av_malloc(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!extradata) {
            av_buffer_unref(&filtered_buf);
            goto fail;
        }

        *data = extradata;
        *size = extradata_size;

        for (i = 0; i < h2645_pkt.nb_nals; i++) {
            H2645NAL *nal = &h2645_pkt.nals[i];
            if (val_in_array(extradata_nal_types, nb_extradata_nal_types,
                             nal->type)) {
                AV_WB24(extradata, 1); // startcode
                memcpy(extradata + 3, nal->raw_data, nal->raw_size);
                extradata += 3 + nal->raw_size;
            } else if (s->remove) {
                AV_WB24(filtered_data, 1); // startcode
                memcpy(filtered_data + 3, nal->raw_data, nal->raw_size);
                filtered_data += 3 + nal->raw_size;
            }
        }

        if (s->remove) {
            av_buffer_unref(&pkt->buf);
            pkt->buf  = filtered_buf;
            pkt->data = filtered_buf->data;
            pkt->size = filtered_data - filtered_buf->data;
        }
    }

fail:
    ff_h2645_packet_uninit(&h2645_pkt);
    return ret;
}

static int extract_extradata_vc1(AVBSFContext *ctx, AVPacket *pkt,
                                 uint8_t **data, int *size)
{
    ExtractExtradataContext *s = ctx->priv_data;
    uint32_t state = UINT32_MAX;
    int has_extradata = 0, extradata_size = 0;
    int i;

    for (i = 0; i < pkt->size; i++) {
        state = (state << 8) | pkt->data[i];
        if (IS_MARKER(state)) {
            if (state == VC1_CODE_SEQHDR || state == VC1_CODE_ENTRYPOINT) {
                has_extradata = 1;
            } else if (has_extradata) {
                extradata_size = i - 3;
                break;
            }
        }
    }

    if (extradata_size) {
        *data = av_malloc(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!*data)
            return AVERROR(ENOMEM);

        memcpy(*data, pkt->data, extradata_size);
        *size = extradata_size;

        if (s->remove) {
            pkt->data += extradata_size;
            pkt->size -= extradata_size;
        }
    }

    return 0;
}

static int extract_extradata_mpeg124(AVBSFContext *ctx, AVPacket *pkt,
                                     uint8_t **data, int *size)
{
    ExtractExtradataContext *s = ctx->priv_data;
    int is_mpeg12 = ctx->par_in->codec_id == AV_CODEC_ID_MPEG1VIDEO ||
                    ctx->par_in->codec_id == AV_CODEC_ID_MPEG2VIDEO;
    uint32_t state = UINT32_MAX;
    int i;

    for (i = 0; i < pkt->size; i++) {
        state = (state << 8) | pkt->data[i];
        if ((is_mpeg12 && state != 0x1B3 && state != 0x1B5 && state < 0x200 && state >= 0x100) ||
            (!is_mpeg12 && (state == 0x1B3 || state == 0x1B6))) {
            if (i > 3) {
                *size = i - 3;
                *data = av_malloc(*size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (!*data)
                    return AVERROR(ENOMEM);

                memcpy(*data, pkt->data, *size);

                if (s->remove) {
                    pkt->data += *size;
                    pkt->size -= *size;
                }
            }
            break;
        }
    }
    return 0;
}

static const struct {
    enum AVCodecID id;
    int (*extract)(AVBSFContext *ctx, AVPacket *pkt,
                   uint8_t **data, int *size);
} extract_tab[] = {
    { AV_CODEC_ID_CAVS,       extract_extradata_mpeg124 },
    { AV_CODEC_ID_H264,       extract_extradata_h2645   },
    { AV_CODEC_ID_HEVC,       extract_extradata_h2645   },
    { AV_CODEC_ID_MPEG1VIDEO, extract_extradata_mpeg124 },
    { AV_CODEC_ID_MPEG2VIDEO, extract_extradata_mpeg124 },
    { AV_CODEC_ID_MPEG4,      extract_extradata_mpeg124 },
    { AV_CODEC_ID_VC1,        extract_extradata_vc1     },
};

static int extract_extradata_init(AVBSFContext *ctx)
{
    ExtractExtradataContext *s = ctx->priv_data;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(extract_tab); i++) {
        if (extract_tab[i].id == ctx->par_in->codec_id) {
            s->extract = extract_tab[i].extract;
            break;
        }
    }
    if (!s->extract)
        return AVERROR_BUG;

    return 0;
}

static int extract_extradata_filter(AVBSFContext *ctx, AVPacket *out)
{
    ExtractExtradataContext *s = ctx->priv_data;
    AVPacket *in;
    uint8_t *extradata = NULL;
    int extradata_size;
    int ret = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    ret = s->extract(ctx, in, &extradata, &extradata_size);
    if (ret < 0)
        goto fail;

    if (extradata) {
        ret = av_packet_add_side_data(in, AV_PKT_DATA_NEW_EXTRADATA,
                                      extradata, extradata_size);
        if (ret < 0) {
            av_freep(&extradata);
            goto fail;
        }
    }

    av_packet_move_ref(out, in);

fail:
    av_packet_free(&in);
    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_CAVS,
    AV_CODEC_ID_H264,
    AV_CODEC_ID_HEVC,
    AV_CODEC_ID_MPEG1VIDEO,
    AV_CODEC_ID_MPEG2VIDEO,
    AV_CODEC_ID_MPEG4,
    AV_CODEC_ID_VC1,
    AV_CODEC_ID_NONE,
};

#define OFFSET(x) offsetof(ExtractExtradataContext, x)
static const AVOption options[] = {
    { "remove", "remove the extradata from the bitstream", OFFSET(remove), AV_OPT_TYPE_INT,
        { .i64 = 0 }, 0, 1 },
    { NULL },
};

static const AVClass extract_extradata_class = {
    .class_name = "extract_extradata",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const AVBitStreamFilter ff_extract_extradata_bsf = {
    .name           = "extract_extradata",
    .codec_ids      = codec_ids,
    .priv_data_size = sizeof(ExtractExtradataContext),
    .priv_class     = &extract_extradata_class,
    .init           = extract_extradata_init,
    .filter         = extract_extradata_filter,
};
