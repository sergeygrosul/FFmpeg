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

#include "hevcdec.h"
#include "hwconfig.h"
#include "v4l2_request.h"
//#include "hevc-ctrls.h"

#define MAX_SLICES 16


const AVHWAccel ff_hevc_v4l2request_hwaccel = {
    .name           = "hevc_v4l2request",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .pix_fmt        = AV_PIX_FMT_DRM_PRIME,
//    .start_frame    = v4l2_request_hevc_start_frame,
//    .decode_slice   = v4l2_request_hevc_decode_slice,
//    .end_frame      = v4l2_request_hevc_end_frame,
//    .frame_priv_data_size = sizeof(V4L2RequestControlsHEVC),
//    .init           = v4l2_request_hevc_init,
//    .uninit         = ff_v4l2_request_uninit,
//    .priv_data_size = sizeof(V4L2RequestContextHEVC),
//    .frame_params   = ff_v4l2_request_frame_params,
    .caps_internal  = HWACCEL_CAP_ASYNC_SAFE,
};
