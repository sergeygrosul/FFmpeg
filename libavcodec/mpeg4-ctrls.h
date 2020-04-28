/* SPDX-License-Identifier: GPL-2.0 */
/*
 * These are the MPEG4 state controls for use with stateless MPEG-4
 * codec drivers.
 *
 * It turns out that these structs are not stable yet and will undergo
 * more changes. So keep them private until they are stable and ready to
 * become part of the official public API.
 */

#ifndef _MPEG4_CTRLS_H_
#define _MPEG4_CTRLS_H_

#define V4L2_PIX_FMT_MPEG4_SLICE v4l2_fourcc('S', 'M', 'P', '4')

#define V4L2_CID_MPEG_VIDEO_MPEG4_SLICE_PARAMS		(V4L2_CID_MPEG_BASE+252)
#define V4L2_CID_MPEG_VIDEO_MPEG4_QUANTIZATION		(V4L2_CID_MPEG_BASE+253)

/* enum v4l2_ctrl_type type values */
#define V4L2_CTRL_TYPE_MPEG4_SLICE_PARAMS 0x0107
#define	V4L2_CTRL_TYPE_MPEG4_QUANTIZATION 0x0108

#define V4L2_MPEG4_PIC_FLAG_SHORT_VIDEO_HEADER		0x001
#define V4L2_MPEG4_PIC_FLAG_INTERLACED			0x002
#define V4L2_MPEG4_PIC_FLAG_OBMC_DISABLE		0x004
#define V4L2_MPEG4_PIC_FLAG_QUANT_TYPE			0x008
#define V4L2_MPEG4_PIC_FLAG_QUARTER_SAMPLE		0x010
#define V4L2_MPEG4_PIC_FLAG_DATA_PARTITIONED		0x020
#define V4L2_MPEG4_PIC_FLAG_REVERSIBLE_VLC		0x040
#define V4L2_MPEG4_PIC_FLAG_RESYNC_MARKER_DISABLE	0x080
#define V4L2_MPEG4_PIC_FLAG_ROUNDING_TYPE		0x100
#define V4L2_MPEG4_PIC_FLAG_TOP_FIELD_FIRST		0x200
#define V4L2_MPEG4_PIC_FLAG_ALTERNATE_VERT_SCAN		0x400

struct v4l2_mpeg4_picture {
	__u16	width;
	__u16	height;
	__u8	chroma_format;
	__u8	sprite_enable;
	__u8	sprite_warping_accuracy;
	__u8	num_sprite_warping_points;
	__s16	sprite_trajectory_du[3];
	__s16	sprite_trajectory_dv[3];
	__u8	quant_precision;
	__u8	vop_coding_type;
	__u8	bwd_ref_coding_type;
	__u8	intra_dc_vlc_thr;
	__u8	fcode_fwd;
	__u8	fcode_bwd;
	__u16	time_inc_resolution;
	__u8	num_gobs_in_vop;
	__u8	num_mb_in_gob;
	__s16	trb;
	__s16	trd;

	__u32	flags;
};

struct v4l2_ctrl_mpeg4_slice_params {
	__u32	bit_size;
	__u32	data_bit_offset;
	__u64	backward_ref_ts;
	__u64	forward_ref_ts;

	struct v4l2_mpeg4_picture picture;

	__u32	quantiser_scale_code;
};

struct v4l2_ctrl_mpeg4_quantization {
	__u8	load_intra_quantiser_matrix;
	__u8	load_non_intra_quantiser_matrix;

	__u8	intra_quantiser_matrix[64];
	__u8	non_intra_quantiser_matrix[64];
};

#endif
