/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "h263.h"
#include "hwaccel.h"
#include "mpegvideo.h"
#include "mpeg4video.h"
#include "v4l2_request.h"
#include "mpeg4-ctrls.h"

typedef struct V4L2RequestControlsMPEG4 {
    struct v4l2_ctrl_mpeg4_slice_params slice_params;
    struct v4l2_ctrl_mpeg4_quantization quantization;
} V4L2RequestControlsMPEG4;

/** Reconstruct bitstream intra_dc_vlc_thr */
static int mpeg4_get_intra_dc_vlc_thr(Mpeg4DecContext *s)
{
    switch (s->intra_dc_threshold) {
        case 99: return 0;
        case 13: return 1;
        case 15: return 2;
        case 17: return 3;
        case 19: return 4;
        case 21: return 5;
        case 23: return 6;
        case 0:  return 7;
    }
    return 0;
}

static int v4l2_request_mpeg4_start_frame(AVCodecContext *avctx,
                                          av_unused const uint8_t *buffer,
                                          av_unused uint32_t size)
{
    Mpeg4DecContext *ctx = avctx->priv_data;
    const MpegEncContext *s = avctx->priv_data;
    V4L2RequestControlsMPEG4 *controls = s->current_picture_ptr->hwaccel_picture_private;
    int i;

    controls->slice_params = (struct v4l2_ctrl_mpeg4_slice_params) {
        .bit_size = 0,
        .data_bit_offset = 0,

        .quantiser_scale_code = s->qscale,

        .picture = {
            .width = s->width,
            .height = s->height,
            .chroma_format = CHROMA_420,
            .sprite_enable = ctx->vol_sprite_usage,
            .sprite_warping_accuracy = s->sprite_warping_accuracy,
            .num_sprite_warping_points = ctx->num_sprite_warping_points,
            .quant_precision = s->quant_precision,
            .vop_coding_type = s->pict_type - AV_PICTURE_TYPE_I,
            .bwd_ref_coding_type =
                s->pict_type == AV_PICTURE_TYPE_B ? s->next_picture.f->pict_type - AV_PICTURE_TYPE_I : 0,
            .intra_dc_vlc_thr = mpeg4_get_intra_dc_vlc_thr(ctx),
            .fcode_fwd = s->f_code,
            .fcode_bwd = s->b_code,
            .time_inc_resolution = avctx->framerate.num,
            .num_gobs_in_vop = s->mb_width * H263_GOB_HEIGHT(s->height),
            .num_mb_in_gob =
                (s->mb_width * s->mb_height) / (s->mb_width * H263_GOB_HEIGHT(s->height)),
            .trb = s->pb_time,
            .trd = s->pp_time,
        },
    };

    for (i = 0; i < ctx->num_sprite_warping_points && i < 3; i++) {
        controls->slice_params.picture.sprite_trajectory_du[i] = ctx->sprite_traj[i][0];
        controls->slice_params.picture.sprite_trajectory_dv[i] = ctx->sprite_traj[i][1];
    }

    if (avctx->codec->id == AV_CODEC_ID_H263)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_SHORT_VIDEO_HEADER;
    if (!s->progressive_sequence)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_INTERLACED;
    controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_OBMC_DISABLE;
    if (s->mpeg_quant)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_QUANT_TYPE;
    if (s->quarter_sample)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_QUARTER_SAMPLE;
    if (s->data_partitioning)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_DATA_PARTITIONED;
    if (ctx->rvlc)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_REVERSIBLE_VLC;
    if (!ctx->resync_marker)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_RESYNC_MARKER_DISABLE;
    if (s->no_rounding)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_ROUNDING_TYPE;
    if (s->top_field_first)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_TOP_FIELD_FIRST;
    if (s->alternate_scan)
        controls->slice_params.picture.flags |= V4L2_MPEG4_PIC_FLAG_ALTERNATE_VERT_SCAN;

    switch (s->pict_type) {
    case AV_PICTURE_TYPE_B:
        controls->slice_params.backward_ref_ts = ff_v4l2_request_get_capture_timestamp(s->next_picture.f);
        // fall-through
    case AV_PICTURE_TYPE_P:
        controls->slice_params.forward_ref_ts = ff_v4l2_request_get_capture_timestamp(s->last_picture.f);
    }

    controls->quantization = (struct v4l2_ctrl_mpeg4_quantization) {
        .load_intra_quantiser_matrix = 1,
        .load_non_intra_quantiser_matrix = 1,
    };

    for (int i = 0; i < 64; i++) {
        int n = s->idsp.idct_permutation[ff_zigzag_direct[i]];
        controls->quantization.intra_quantiser_matrix[i] = s->intra_matrix[n];
        controls->quantization.non_intra_quantiser_matrix[i] = s->inter_matrix[n];
    }

    return ff_v4l2_request_reset_frame(avctx, s->current_picture_ptr->f);
}

static int v4l2_request_mpeg4_decode_slice(AVCodecContext *avctx, const uint8_t *buffer, uint32_t size)
{
    const MpegEncContext *s = avctx->priv_data;

    return ff_v4l2_request_append_output_buffer(avctx, s->current_picture_ptr->f, buffer, size);
}

static int v4l2_request_mpeg4_end_frame(AVCodecContext *avctx)
{
    const MpegEncContext *s = avctx->priv_data;
    V4L2RequestControlsMPEG4 *controls = s->current_picture_ptr->hwaccel_picture_private;
    V4L2RequestDescriptor *req = (V4L2RequestDescriptor*)s->current_picture_ptr->f->data[0];

    struct v4l2_ext_control control[] = {
        {
            .id = V4L2_CID_MPEG_VIDEO_MPEG4_SLICE_PARAMS,
            .ptr = &controls->slice_params,
            .size = sizeof(controls->slice_params),
        },
        {
            .id = V4L2_CID_MPEG_VIDEO_MPEG4_QUANTIZATION,
            .ptr = &controls->quantization,
            .size = sizeof(controls->quantization),
        },
    };

    controls->slice_params.bit_size = req->output.used * 8;

    return ff_v4l2_request_decode_frame(avctx, s->current_picture_ptr->f, control, FF_ARRAY_ELEMS(control));
}

static int v4l2_request_mpeg4_init(AVCodecContext *avctx)
{
    return ff_v4l2_request_init(avctx, V4L2_PIX_FMT_MPEG4_SLICE, 1024 * 1024, NULL, 0);
}

#if CONFIG_MPEG4_V4L2REQUEST_HWACCEL
const AVHWAccel ff_mpeg4_v4l2request_hwaccel = {
    .name           = "mpeg4_v4l2request",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MPEG4,
    .pix_fmt        = AV_PIX_FMT_DRM_PRIME,
    .start_frame    = v4l2_request_mpeg4_start_frame,
    .decode_slice   = v4l2_request_mpeg4_decode_slice,
    .end_frame      = v4l2_request_mpeg4_end_frame,
    .frame_priv_data_size = sizeof(V4L2RequestControlsMPEG4),
    .init           = v4l2_request_mpeg4_init,
    .uninit         = ff_v4l2_request_uninit,
    .priv_data_size = sizeof(V4L2RequestContext),
    .frame_params   = ff_v4l2_request_frame_params,
    .caps_internal  = HWACCEL_CAP_ASYNC_SAFE,
};
#endif

#if CONFIG_H263_V4L2REQUEST_HWACCEL
const AVHWAccel ff_h263_v4l2request_hwaccel = {
    .name           = "h263_v4l2request",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H263,
    .pix_fmt        = AV_PIX_FMT_DRM_PRIME,
    .start_frame    = v4l2_request_mpeg4_start_frame,
    .decode_slice   = v4l2_request_mpeg4_decode_slice,
    .end_frame      = v4l2_request_mpeg4_end_frame,
    .frame_priv_data_size = sizeof(V4L2RequestControlsMPEG4),
    .init           = v4l2_request_mpeg4_init,
    .uninit         = ff_v4l2_request_uninit,
    .priv_data_size = sizeof(V4L2RequestContext),
    .frame_params   = ff_v4l2_request_frame_params,
    .caps_internal  = HWACCEL_CAP_ASYNC_SAFE,
};
#endif
