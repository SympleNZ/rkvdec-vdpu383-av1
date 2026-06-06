// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder VDPU383 H264 backend
 *
 * Copyright (C) 2024 Collabora, Ltd.
 *  Detlev Casanova <detlev.casanova@collabora.com>
 */

#include <media/v4l2-h264.h>
#include <media/v4l2-mem2mem.h>

#include <linux/iopoll.h>
#include <linux/moduleparam.h>
#include <linux/iommu.h>

/*
 * 2026-06-05 Step-A probe: filterd (RCB slot 6) perturbation.
 * -1 = disabled (default; behaves exactly like the unmodified driver).
 * >=0 = before each decode kick, read back slot 6 (count bytes == fill,
 *       i.e. survivors from the previous decode) then prefill slot 6 with
 *       the low byte of this value. Distinguishes "HW reads stale/uninit
 *       slot-6 content" (survivors > 0, output contaminated by fill) from
 *       "HW overwrites slot 6 with its own data" (survivors ~0, output
 *       unaffected -> read-before-write race, content-independent).
 */
static int h264_rcb_dbg = -1;
module_param(h264_rcb_dbg, int, 0644);
MODULE_PARM_DESC(h264_rcb_dbg, "H264 filterd RCB (slot 6) debug fill byte (-1=off)");

/*
 * 2026-06-05 Step-A.5 probe: disable reg010 block auto-gating for H.264.
 * Mainline enables ALL auto-gating bits; aggressive power-gating of the
 * deblock/RCB sub-blocks mid-op is a candidate cause of the intermittent
 * read-before-write race on the filterd context. 0 = default (gating on).
 *   1 = disable filterd + rcb gating only;  2 = disable ALL gating.
 */
static int h264_nogate;
module_param(h264_nogate, int, 0644);
MODULE_PARM_DESC(h264_nogate, "H264 disable block auto-gating (0=off,1=filterd+rcb,2=all)");

/*
 * 2026-06-05 Step-B lead: MPP's vdpu383_h264d_rcb_calc sets RCB_FLTD_ON_COL
 * (reg158) and RCB_FLTD_UPSC_ON_COL (reg160) to 0 for H.264 (non-tiled,
 * frame-level row deblock). Our codec-agnostic vdpu383_rcb_sizes table writes
 * non-zero sizes to those slots (9 filterd_tile_col, 10 AV1-only upscale).
 * h264_rcb_zcol=1 zeroes reg158+reg160 to match MPP; =2 also matches by also
 * zeroing slot 8 (reg156) for an extra A/B point. 0 = default (unchanged).
 */
static int h264_rcb_zcol;
module_param(h264_rcb_zcol, int, 0644);
MODULE_PARM_DESC(h264_rcb_zcol, "H264 zero RCB col regs to match MPP (0=off,1=reg158+160,2=+reg156)");

/*
 * 2026-06-05 Step-B bundle: match MPP's ctrl_regs (reg13/20/21/28/29) which our
 * kernel leaves at 0/per-pixel. reg29.addr_align_type / reg28.rd_latency_id are
 * AXI read/write timing knobs that could change the deblock read-before-write
 * race margin (our HEVC works without them, but HEVC doesn't expose the race).
 * 0 = default (unchanged).
 */
static int h264_mpp_ctrl;
module_param(h264_mpp_ctrl, int, 0644);
MODULE_PARM_DESC(h264_mpp_ctrl, "H264 match MPP ctrl regs reg13/20/21/28/29 (0=off,1=on)");

/*
 * 2026-06-05 Step-C: match MPP's EXACT RCB size registers (reg140-160) for
 * 1080p H.264. Our codec-agnostic table writes ALLOCATED buffer sizes (4-6x
 * larger than MPP's computed bit_sizes). MPP runtime values captured from the
 * BSP dump build (vdpu38x_rcb_reg_info_update). The HW may use these size regs
 * as the deblock-context row stride; this is the one RCB area not yet matched.
 * Offsets keep our (larger) contiguous layout — safe, only the size value
 * changes. 0 = default (unchanged).
 */
static int h264_mpp_rcb;
module_param(h264_mpp_rcb, int, 0644);
MODULE_PARM_DESC(h264_mpp_rcb, "H264 set RCB size regs to MPP 1080p values (0=off,1=on)");

/*
 * 2026-06-05 Lead 1: pre-kick barrier + IOMMU IOTLB flush. MPP (BSP) does
 * mpp_iommu_flush_tlb() + wmb() + buffer sync before the HW kick; our mainline
 * path writes regs via memcpy_toio then writel(DEC_ENABLE) directly. Tests
 * whether a memory-visibility / TLB-coherency gap at kick time is what lets the
 * deblock read-before-write race occur. 0 = default (unchanged).
 *   1 = wmb() only;  2 = wmb() + iommu_flush_iotlb_all().
 */
static int h264_kick_flush;
module_param(h264_kick_flush, int, 0644);
MODULE_PARM_DESC(h264_kick_flush, "H264 pre-kick barrier/flush (0=off,1=wmb,2=wmb+iotlb)");

/*
 * 2026-06-05 Lead 3 (the strong one): clear the decoder's internal CACHE0/1/2
 * before the kick. VP9 does this (regs+0x410/0x41c, mainline offsets) and MPP/BSP
 * clears all three before every decode; the H.264 backend clears NOTHING. A stale
 * internal HW cache from a prior decode is a textbook cause of an intermittent,
 * content-independent deblock-context hazard (and explains why the RCB-buffer
 * prefill in Step A had no effect — that's a DMA buffer, not the HW cache).
 *   1 = clear CACHE0;  2 = clear CACHE0+1+2 (BSP-style). 0 = default (unchanged).
 */
static int h264_cache_clr;
module_param(h264_cache_clr, int, 0644);
MODULE_PARM_DESC(h264_cache_clr, "H264 clear decoder cache pre-kick (0=off,1=CACHE0,2=all3)");

#include "rkvdec-rcb.h"
#include "rkvdec-cabac.h"
#include "rkvdec-vdpu383-regs.h"
#include "rkvdec-h264-common.h"

struct rkvdec_sps {
	u16 seq_parameter_set_id:			4;
	u16 profile_idc:				8;
	u16 constraint_set3_flag:			1;
	u16 chroma_format_idc:				2;
	u16 bit_depth_luma:				3;
	u16 bit_depth_chroma:				3;
	u16 qpprime_y_zero_transform_bypass_flag:	1;
	u16 log2_max_frame_num_minus4:			4;
	u16 max_num_ref_frames:				5;
	u16 pic_order_cnt_type:				2;
	u16 log2_max_pic_order_cnt_lsb_minus4:		4;
	u16 delta_pic_order_always_zero_flag:		1;

	u16 pic_width_in_mbs:				16;
	u16 pic_height_in_mbs:				16;

	u16 frame_mbs_only_flag:			1;
	u16 mb_adaptive_frame_field_flag:		1;
	u16 direct_8x8_inference_flag:			1;
	u16 mvc_extension_enable:			1;
	u16 num_views:					2;
	u16 view_id0:                                   10;
	u16 view_id1:                                   10;
} __packed;

struct rkvdec_pps {
	u32 pic_parameter_set_id:				8;
	u32 pps_seq_parameter_set_id:				5;
	u32 entropy_coding_mode_flag:				1;
	u32 bottom_field_pic_order_in_frame_present_flag:	1;
	u32 num_ref_idx_l0_default_active_minus1:		5;
	u32 num_ref_idx_l1_default_active_minus1:		5;
	u32 weighted_pred_flag:					1;
	u32 weighted_bipred_idc:				2;
	u32 pic_init_qp_minus26:				7;
	u32 pic_init_qs_minus26:				6;
	u32 chroma_qp_index_offset:				5;
	u32 deblocking_filter_control_present_flag:		1;
	u32 constrained_intra_pred_flag:			1;
	u32 redundant_pic_cnt_present:				1;
	u32 transform_8x8_mode_flag:				1;
	u32 second_chroma_qp_index_offset:			5;
	u32 scaling_list_enable_flag:				1;
	u32 is_longterm:					16;
	u32 voidx:						16;

	// dpb
	u32 pic_field_flag:                                     1;
	u32 pic_associated_flag:                                1;
	u32 cur_top_field:					32;
	u32 cur_bot_field:					32;

	u32 top_field_order_cnt0:				32;
	u32 bot_field_order_cnt0:				32;
	u32 top_field_order_cnt1:				32;
	u32 bot_field_order_cnt1:				32;
	u32 top_field_order_cnt2:				32;
	u32 bot_field_order_cnt2:				32;
	u32 top_field_order_cnt3:				32;
	u32 bot_field_order_cnt3:				32;
	u32 top_field_order_cnt4:				32;
	u32 bot_field_order_cnt4:				32;
	u32 top_field_order_cnt5:				32;
	u32 bot_field_order_cnt5:				32;
	u32 top_field_order_cnt6:				32;
	u32 bot_field_order_cnt6:				32;
	u32 top_field_order_cnt7:				32;
	u32 bot_field_order_cnt7:				32;
	u32 top_field_order_cnt8:				32;
	u32 bot_field_order_cnt8:				32;
	u32 top_field_order_cnt9:				32;
	u32 bot_field_order_cnt9:				32;
	u32 top_field_order_cnt10:				32;
	u32 bot_field_order_cnt10:				32;
	u32 top_field_order_cnt11:				32;
	u32 bot_field_order_cnt11:				32;
	u32 top_field_order_cnt12:				32;
	u32 bot_field_order_cnt12:				32;
	u32 top_field_order_cnt13:				32;
	u32 bot_field_order_cnt13:				32;
	u32 top_field_order_cnt14:				32;
	u32 bot_field_order_cnt14:				32;
	u32 top_field_order_cnt15:				32;
	u32 bot_field_order_cnt15:				32;

	u32 ref_field_flags:					16;
	u32 ref_topfield_used:					16;
	u32 ref_botfield_used:					16;
	u32 ref_colmv_use_flag:					16;

	u32 reserved0:						30;
	u32 reserved[3];
} __packed;

struct rkvdec_sps_pps {
	struct rkvdec_sps sps;
	struct rkvdec_pps pps;
} __packed;

/* Data structure describing auxiliary buffer format. */
struct rkvdec_h264_priv_tbl {
	s8 cabac_table[4][464][2];
	struct rkvdec_h264_scaling_list scaling_list;
	struct rkvdec_sps_pps param_set[256];
	struct rkvdec_rps rps;
} __packed;

struct rkvdec_h264_ctx {
	struct rkvdec_aux_buf priv_tbl;
	struct rkvdec_h264_reflists reflists;
	struct vdpu383_regs_h26x regs;
};

static noinline_for_stack void set_field_order_cnt(struct rkvdec_pps *pps, const struct v4l2_h264_dpb_entry *dpb)
{
	pps->top_field_order_cnt0 = dpb[0].top_field_order_cnt;
	pps->bot_field_order_cnt0 = dpb[0].bottom_field_order_cnt;
	pps->top_field_order_cnt1 = dpb[1].top_field_order_cnt;
	pps->bot_field_order_cnt1 = dpb[1].bottom_field_order_cnt;
	pps->top_field_order_cnt2 = dpb[2].top_field_order_cnt;
	pps->bot_field_order_cnt2 = dpb[2].bottom_field_order_cnt;
	pps->top_field_order_cnt3 = dpb[3].top_field_order_cnt;
	pps->bot_field_order_cnt3 = dpb[3].bottom_field_order_cnt;
	pps->top_field_order_cnt4 = dpb[4].top_field_order_cnt;
	pps->bot_field_order_cnt4 = dpb[4].bottom_field_order_cnt;
	pps->top_field_order_cnt5 = dpb[5].top_field_order_cnt;
	pps->bot_field_order_cnt5 = dpb[5].bottom_field_order_cnt;
	pps->top_field_order_cnt6 = dpb[6].top_field_order_cnt;
	pps->bot_field_order_cnt6 = dpb[6].bottom_field_order_cnt;
	pps->top_field_order_cnt7 = dpb[7].top_field_order_cnt;
	pps->bot_field_order_cnt7 = dpb[7].bottom_field_order_cnt;
	pps->top_field_order_cnt8 = dpb[8].top_field_order_cnt;
	pps->bot_field_order_cnt8 = dpb[8].bottom_field_order_cnt;
	pps->top_field_order_cnt9 = dpb[9].top_field_order_cnt;
	pps->bot_field_order_cnt9 = dpb[9].bottom_field_order_cnt;
	pps->top_field_order_cnt10 = dpb[10].top_field_order_cnt;
	pps->bot_field_order_cnt10 = dpb[10].bottom_field_order_cnt;
	pps->top_field_order_cnt11 = dpb[11].top_field_order_cnt;
	pps->bot_field_order_cnt11 = dpb[11].bottom_field_order_cnt;
	pps->top_field_order_cnt12 = dpb[12].top_field_order_cnt;
	pps->bot_field_order_cnt12 = dpb[12].bottom_field_order_cnt;
	pps->top_field_order_cnt13 = dpb[13].top_field_order_cnt;
	pps->bot_field_order_cnt13 = dpb[13].bottom_field_order_cnt;
	pps->top_field_order_cnt14 = dpb[14].top_field_order_cnt;
	pps->bot_field_order_cnt14 = dpb[14].bottom_field_order_cnt;
	pps->top_field_order_cnt15 = dpb[15].top_field_order_cnt;
	pps->bot_field_order_cnt15 = dpb[15].bottom_field_order_cnt;
}

static noinline_for_stack void set_dec_params(struct rkvdec_pps *pps, const struct v4l2_ctrl_h264_decode_params *dec_params)
{
	const struct v4l2_h264_dpb_entry *dpb = dec_params->dpb;

	for (int i = 0; i < ARRAY_SIZE(dec_params->dpb); i++) {
		if (dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM)
			pps->is_longterm |= (1 << i);
		pps->ref_field_flags |=
		 (!!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_FIELD)) << i;
		pps->ref_colmv_use_flag |=
		 (!!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)) << i;
		pps->ref_topfield_used |=
		 (!!(dpb[i].fields & V4L2_H264_TOP_FIELD_REF)) << i;
		pps->ref_botfield_used |=
			(!!(dpb[i].fields & V4L2_H264_BOTTOM_FIELD_REF)) << i;
	}
	pps->pic_field_flag =
		!!(dec_params->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC);
	pps->pic_associated_flag =
		!!(dec_params->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD);

	pps->cur_top_field = dec_params->top_field_order_cnt;
	pps->cur_bot_field = dec_params->bottom_field_order_cnt;
}

static void assemble_hw_pps(struct rkvdec_ctx *ctx,
			    struct rkvdec_h264_run *run)
{
	struct rkvdec_h264_ctx *h264_ctx = ctx->priv;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_ctrl_h264_decode_params *dec_params = run->decode_params;
	const struct v4l2_h264_dpb_entry *dpb = dec_params->dpb;
	struct rkvdec_h264_priv_tbl *priv_tbl = h264_ctx->priv_tbl.cpu;
	struct rkvdec_sps_pps *hw_ps;
	u32 pic_width, pic_height;

	/*
	 * HW read the SPS/PPS information from PPS packet index by PPS id.
	 * offset from the base can be calculated by PPS_id * 32 (size per PPS
	 * packet unit). so the driver copy SPS/PPS information to the exact PPS
	 * packet unit for HW accessing.
	 */
	hw_ps = &priv_tbl->param_set[pps->pic_parameter_set_id];
	memset(hw_ps, 0, sizeof(*hw_ps));

	/* write sps */
	hw_ps->sps.seq_parameter_set_id = sps->seq_parameter_set_id;
	hw_ps->sps.profile_idc = sps->profile_idc;
	hw_ps->sps.constraint_set3_flag = !!(sps->constraint_set_flags & (1 << 3));
	hw_ps->sps.chroma_format_idc = sps->chroma_format_idc;
	hw_ps->sps.bit_depth_luma = sps->bit_depth_luma_minus8;
	hw_ps->sps.bit_depth_chroma = sps->bit_depth_chroma_minus8;
	hw_ps->sps.qpprime_y_zero_transform_bypass_flag =
		!!(sps->flags & V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS);
	hw_ps->sps.log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
	hw_ps->sps.max_num_ref_frames = sps->max_num_ref_frames;
	hw_ps->sps.pic_order_cnt_type = sps->pic_order_cnt_type;
	hw_ps->sps.log2_max_pic_order_cnt_lsb_minus4 =
		sps->log2_max_pic_order_cnt_lsb_minus4;
	hw_ps->sps.delta_pic_order_always_zero_flag =
		!!(sps->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO);
	hw_ps->sps.mvc_extension_enable = 0;
	hw_ps->sps.num_views = 0;

	/*
	 * Use the SPS values since they are already in macroblocks
	 * dimensions, height can be field height (halved) if
	 * V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY is not set and also it allows
	 * decoding smaller images into larger allocation which can be used
	 * to implementing SVC spatial layer support.
	 */
	pic_width = 16 * (sps->pic_width_in_mbs_minus1 + 1);
	pic_height = 16 * (sps->pic_height_in_map_units_minus1 + 1);
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		pic_height *= 2;
	if (!!(dec_params->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC))
		pic_height /= 2;

	hw_ps->sps.pic_width_in_mbs = pic_width;
	hw_ps->sps.pic_height_in_mbs = pic_height;

	hw_ps->sps.frame_mbs_only_flag =
		!!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY);
	hw_ps->sps.mb_adaptive_frame_field_flag =
		!!(sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD);
	hw_ps->sps.direct_8x8_inference_flag =
		!!(sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE);

	/* write pps */
	hw_ps->pps.pic_parameter_set_id = pps->pic_parameter_set_id;
	hw_ps->pps.pps_seq_parameter_set_id = pps->seq_parameter_set_id;
	hw_ps->pps.entropy_coding_mode_flag =
		!!(pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE);
	hw_ps->pps.bottom_field_pic_order_in_frame_present_flag =
		!!(pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT);
	hw_ps->pps.num_ref_idx_l0_default_active_minus1 =
		pps->num_ref_idx_l0_default_active_minus1;
	hw_ps->pps.num_ref_idx_l1_default_active_minus1 =
		pps->num_ref_idx_l1_default_active_minus1;
	hw_ps->pps.weighted_pred_flag =
		!!(pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED);
	hw_ps->pps.weighted_bipred_idc = pps->weighted_bipred_idc;
	hw_ps->pps.pic_init_qp_minus26 = pps->pic_init_qp_minus26;
	hw_ps->pps.pic_init_qs_minus26 = pps->pic_init_qs_minus26;
	hw_ps->pps.chroma_qp_index_offset = pps->chroma_qp_index_offset;
	hw_ps->pps.deblocking_filter_control_present_flag =
		!!(pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT);
	hw_ps->pps.constrained_intra_pred_flag =
		!!(pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED);
	hw_ps->pps.redundant_pic_cnt_present =
		!!(pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT);
	hw_ps->pps.transform_8x8_mode_flag =
		!!(pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE);
	hw_ps->pps.second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
	hw_ps->pps.scaling_list_enable_flag =
		!!(pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT);

	set_field_order_cnt(&hw_ps->pps, dpb);
	set_dec_params(&hw_ps->pps, dec_params);

}

static void rkvdec_write_regs(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_h264_ctx *h264_ctx = ctx->priv;

	rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_COMMON_REGS,
			   &h264_ctx->regs.common,
			   sizeof(h264_ctx->regs.common));
	rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_COMMON_ADDR_REGS,
			   &h264_ctx->regs.common_addr,
			   sizeof(h264_ctx->regs.common_addr));
	rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_CODEC_PARAMS_REGS,
			   &h264_ctx->regs.h26x_params,
			   sizeof(h264_ctx->regs.h26x_params));
	rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_CODEC_ADDR_REGS,
			   &h264_ctx->regs.h26x_addr,
			   sizeof(h264_ctx->regs.h26x_addr));
}

static void config_registers(struct rkvdec_ctx *ctx,
			     struct rkvdec_h264_run *run)
{
	const struct v4l2_ctrl_h264_decode_params *dec_params = run->decode_params;
	struct rkvdec_h264_ctx *h264_ctx = ctx->priv;
	dma_addr_t priv_start_addr = h264_ctx->priv_tbl.dma;
	const struct v4l2_pix_format_mplane *dst_fmt;
	struct vb2_v4l2_buffer *src_buf = run->base.bufs.src;
	struct vb2_v4l2_buffer *dst_buf = run->base.bufs.dst;
	struct vdpu383_regs_h26x *regs = &h264_ctx->regs;
	const struct v4l2_format *f;
	dma_addr_t rlc_addr;
	dma_addr_t dst_addr;
	u32 hor_virstride;
	u32 ver_virstride;
	u32 y_virstride;
	u32 offset;
	u32 pixels;
	u32 i;

	memset(regs, 0, sizeof(*regs));

	/* Set H264 mode */
	regs->common.reg008_dec_mode = VDPU383_MODE_H264;

	/* Set input stream length */
	regs->h26x_params.reg066_stream_len = vb2_get_plane_payload(&src_buf->vb2_buf, 0);

	/* Set strides */
	f = &ctx->decoded_fmt;
	dst_fmt = &f->fmt.pix_mp;
	hor_virstride = dst_fmt->plane_fmt[0].bytesperline;
	ver_virstride = dst_fmt->height;
	y_virstride = hor_virstride * ver_virstride;

	pixels = dst_fmt->height * dst_fmt->width;

	regs->h26x_params.reg068_hor_virstride = hor_virstride / 16;
	regs->h26x_params.reg069_raster_uv_hor_virstride = hor_virstride / 16;
	regs->h26x_params.reg070_y_virstride = y_virstride / 16;

	/* Activate block gating */
	regs->common.reg010_block_gating_en.strmd_auto_gating_e      = 1;
	regs->common.reg010_block_gating_en.inter_auto_gating_e      = 1;
	regs->common.reg010_block_gating_en.intra_auto_gating_e      = 1;
	regs->common.reg010_block_gating_en.transd_auto_gating_e     = 1;
	regs->common.reg010_block_gating_en.recon_auto_gating_e      = 1;
	regs->common.reg010_block_gating_en.filterd_auto_gating_e    = 1;
	regs->common.reg010_block_gating_en.bus_auto_gating_e        = 1;
	regs->common.reg010_block_gating_en.ctrl_auto_gating_e       = 1;
	regs->common.reg010_block_gating_en.rcb_auto_gating_e        = 1;
	regs->common.reg010_block_gating_en.err_prc_auto_gating_e    = 1;

	/* 2026-06-05 Step-A.5: optionally disable block auto-gating (race probe) */
	if (h264_nogate == 1) {
		regs->common.reg010_block_gating_en.filterd_auto_gating_e = 0;
		regs->common.reg010_block_gating_en.rcb_auto_gating_e     = 0;
	} else if (h264_nogate >= 2) {
		memset(&regs->common.reg010_block_gating_en, 0,
		       sizeof(regs->common.reg010_block_gating_en));
	}

	/* Set timeout threshold */
	if (pixels < RKVDEC_1080P_PIXELS)
		regs->common.reg013_core_timeout_threshold = VDPU383_TIMEOUT_1080p;
	else if (pixels < RKVDEC_4K_PIXELS)
		regs->common.reg013_core_timeout_threshold = VDPU383_TIMEOUT_4K;
	else if (pixels < RKVDEC_8K_PIXELS)
		regs->common.reg013_core_timeout_threshold = VDPU383_TIMEOUT_8K;
	else
		regs->common.reg013_core_timeout_threshold = VDPU383_TIMEOUT_MAX;

	regs->common.reg016_error_ctrl_set.error_proc_disable = 1;

	/* Set ref pic address & poc */
	for (i = 0; i < ARRAY_SIZE(dec_params->dpb); i++) {
		struct vb2_buffer *vb_buf = run->ref_buf[i];
		dma_addr_t buf_dma;

		/*
		 * If a DPB entry is unused or invalid, address of current destination
		 * buffer is returned.
		 */
		if (!vb_buf)
			vb_buf = &dst_buf->vb2_buf;

		buf_dma = vb2_dma_contig_plane_dma_addr(vb_buf, 0);

		/* Set reference addresses */
		regs->h26x_addr.reg170_185_ref_base[i] = buf_dma;
		regs->h26x_addr.reg195_210_payload_st_ref_base[i] = buf_dma;

		/* Set COLMV addresses */
		regs->h26x_addr.reg217_232_colmv_ref_base[i] = buf_dma + ctx->colmv_offset;
	}

	/* Set rlc base address (input stream) */
	rlc_addr = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	regs->common_addr.reg128_strm_base = rlc_addr;

	/* Set output base address */
	dst_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	regs->h26x_addr.reg168_decout_base = dst_addr;
	regs->h26x_addr.reg169_error_ref_base = dst_addr;
	regs->h26x_addr.reg192_payload_st_cur_base = dst_addr;

	/* Set colmv address */
	regs->h26x_addr.reg216_colmv_cur_base = dst_addr + ctx->colmv_offset;

	/* Set RCB addresses */
	for (i = 0; i < rkvdec_rcb_buf_count(ctx); i++) {
		regs->common_addr.reg140_162_rcb_info[i].offset = rkvdec_rcb_buf_dma_addr(ctx, i);
		regs->common_addr.reg140_162_rcb_info[i].size = rkvdec_rcb_buf_size(ctx, i);
	}

	/* 2026-06-05 Step-B: optionally match MPP by zeroing FLTD_ON_COL (slot 9 /
	 * reg158) and FLTD_UPSC_ON_COL (slot 10 / reg160) for H.264. */
	if (h264_rcb_zcol >= 1) {
		if (rkvdec_rcb_buf_count(ctx) > 9) {
			regs->common_addr.reg140_162_rcb_info[9].offset = 0;
			regs->common_addr.reg140_162_rcb_info[9].size = 0;
		}
		if (rkvdec_rcb_buf_count(ctx) > 10) {
			regs->common_addr.reg140_162_rcb_info[10].offset = 0;
			regs->common_addr.reg140_162_rcb_info[10].size = 0;
		}
	}
	if (h264_rcb_zcol >= 2 && rkvdec_rcb_buf_count(ctx) > 8) {
		regs->common_addr.reg140_162_rcb_info[8].offset = 0;
		regs->common_addr.reg140_162_rcb_info[8].size = 0;
	}

	/* 2026-06-05 Step-C: override RCB size regs with MPP's exact 1080p values */
	if (h264_mpp_rcb >= 1) {
		static const u32 mpp_sz[11] = {
			0, 0, 5568, 5568, 5760, 5760,
			38592, 38592, 49536, 0, 0,
		};
		int n = rkvdec_rcb_buf_count(ctx);

		for (i = 0; i < n && i < 11; i++)
			regs->common_addr.reg140_162_rcb_info[i].size = mpp_sz[i];
	}

	/* Set hw pps address */
	offset = offsetof(struct rkvdec_h264_priv_tbl, param_set);
	regs->common_addr.reg131_gbl_base = priv_start_addr + offset;
	regs->h26x_params.reg067_global_len = sizeof(struct rkvdec_sps_pps) / 16;

	/* Set hw rps address */
	offset = offsetof(struct rkvdec_h264_priv_tbl, rps);
	regs->common_addr.reg129_rps_base = priv_start_addr + offset;

	/* Set cabac table */
	offset = offsetof(struct rkvdec_h264_priv_tbl, cabac_table);
	regs->common_addr.reg130_cabactbl_base = priv_start_addr + offset;

	/* Set scaling list address */
	offset = offsetof(struct rkvdec_h264_priv_tbl, scaling_list);
	regs->common_addr.reg132_scanlist_addr = priv_start_addr + offset;

	/* 2026-06-05 Step-B: optionally match MPP ctrl_regs (reg13/20/21/28/29) */
	if (h264_mpp_ctrl >= 1) {
		regs->common.reg013_core_timeout_threshold = 0xffffff;
		regs->common.reg020_cabac_error_en_lowbits = 0xfffedfff;
		regs->common.reg021_cabac_error_en_highbits = 0x0ffbf9ff;
		regs->common.reg028_debug_perf_latency_ctrl0.axi_perf_work_e = 1;
		regs->common.reg028_debug_perf_latency_ctrl0.axi_cnt_type = 1;
		regs->common.reg028_debug_perf_latency_ctrl0.rd_latency_id = 11;
		regs->common.reg029_debug_perf_latency_ctrl1.addr_align_type = 1;
		regs->common.reg029_debug_perf_latency_ctrl1.aw_cnt_id_type = 1;
		regs->common.reg029_debug_perf_latency_ctrl1.ar_count_id = 17;
		regs->common.reg029_debug_perf_latency_ctrl1.aw_count_id = 0;
	}

	rkvdec_write_regs(ctx);
}

static int rkvdec_h264_start(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_h264_priv_tbl *priv_tbl;
	struct rkvdec_h264_ctx *h264_ctx;
	struct v4l2_ctrl *ctrl;
	int ret;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_SPS);
	if (!ctrl)
		return -EINVAL;

	ret = rkvdec_h264_validate_sps(ctx, ctrl->p_new.p_h264_sps);
	if (ret)
		return ret;

	h264_ctx = kzalloc_obj(*h264_ctx);
	if (!h264_ctx)
		return -ENOMEM;

	priv_tbl = dma_alloc_coherent(rkvdec->dev, sizeof(*priv_tbl),
				      &h264_ctx->priv_tbl.dma, GFP_KERNEL);
	if (!priv_tbl) {
		ret = -ENOMEM;
		goto err_free_ctx;
	}

	h264_ctx->priv_tbl.size = sizeof(*priv_tbl);
	h264_ctx->priv_tbl.cpu = priv_tbl;
	memcpy(priv_tbl->cabac_table, rkvdec_h264_cabac_table,
	       sizeof(rkvdec_h264_cabac_table));

	ctx->priv = h264_ctx;

	return 0;

err_free_ctx:
	kfree(h264_ctx);
	return ret;
}

static void rkvdec_h264_stop(struct rkvdec_ctx *ctx)
{
	struct rkvdec_h264_ctx *h264_ctx = ctx->priv;
	struct rkvdec_dev *rkvdec = ctx->dev;

	dma_free_coherent(rkvdec->dev, h264_ctx->priv_tbl.size,
			  h264_ctx->priv_tbl.cpu, h264_ctx->priv_tbl.dma);
	kfree(h264_ctx);
}

static int rkvdec_h264_run(struct rkvdec_ctx *ctx)
{
	struct v4l2_h264_reflist_builder reflist_builder;
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_h264_ctx *h264_ctx = ctx->priv;
	struct rkvdec_h264_run run;
	struct rkvdec_h264_priv_tbl *tbl = h264_ctx->priv_tbl.cpu;
	u32 timeout_threshold;

	rkvdec_h264_run_preamble(ctx, &run);

	/* Build the P/B{0,1} ref lists. */
	v4l2_h264_init_reflist_builder(&reflist_builder, run.decode_params,
				       run.sps, run.decode_params->dpb);
	v4l2_h264_build_p_ref_list(&reflist_builder, h264_ctx->reflists.p);
	v4l2_h264_build_b_ref_lists(&reflist_builder, h264_ctx->reflists.b0,
				    h264_ctx->reflists.b1);

	assemble_hw_scaling_list(&run, &tbl->scaling_list);
	assemble_hw_pps(ctx, &run);
	lookup_ref_buf_idx(ctx, &run);
	assemble_hw_rps(&reflist_builder, &run, &h264_ctx->reflists, &tbl->rps);

	config_registers(ctx, &run);

	/* --- 2026-06-05 Step-A probe: filterd (slot 6) RCB perturbation --- */
	if (h264_rcb_dbg >= 0) {
		struct rkvdec_rcb_config *rcb = ctx->rcb_config;
		static int dbg_logged;

		if (rcb && rcb->rcb_count > 6 && rcb->rcb_bufs[6].cpu) {
			u8 *p = rcb->rcb_bufs[6].cpu;
			size_t sz = rcb->rcb_bufs[6].size;
			u8 fill = (u8)h264_rcb_dbg;
			size_t i, survivors = 0;

			for (i = 0; i < sz; i++)
				if (p[i] == fill)
					survivors++;
			if (dbg_logged < 8) {
				pr_info("rkvdec-h264 rcb-dbg: slot6 sz=%zu fill=0x%02x survivors=%zu first16=%*ph\n",
					sz, fill, survivors, 16, p);
				dbg_logged++;
			}
			memset(p, fill, sz);
		}
	}
	/* --- end probe --- */

	rkvdec_run_postamble(ctx, &run.base);

	timeout_threshold = h264_ctx->regs.common.reg013_core_timeout_threshold;
	rkvdec_schedule_watchdog(rkvdec, timeout_threshold);

	/* 2026-06-05 Lead 3: clear decoder internal cache(s) pre-kick (VP9/BSP do) */
	if (h264_cache_clr >= 1) {
		/* CACHE0_SIZE = CACHEABLE | READ_ALLOC | LINE_64, then CLR_CACHE0 */
		writel(0x1u | 0x2u | 0x10u, rkvdec->regs + 0x41c);
		if (h264_cache_clr >= 2) {
			writel(0x1u | 0x2u | 0x10u, rkvdec->regs + 0x45c);
			writel(0x1u | 0x2u | 0x10u, rkvdec->regs + 0x49c);
		}
		writel(0x1u, rkvdec->regs + 0x410);
		if (h264_cache_clr >= 2) {
			writel(0x1u, rkvdec->regs + 0x450);
			writel(0x1u, rkvdec->regs + 0x490);
		}
		wmb();
	}

	/* 2026-06-05 Lead 1: pre-kick barrier + IOMMU IOTLB flush (match MPP) */
	if (h264_kick_flush >= 1)
		wmb();
	if (h264_kick_flush >= 2 && rkvdec->iommu_domain)
		iommu_flush_iotlb_all(rkvdec->iommu_domain);

	/* Start decoding! */
	writel(timeout_threshold, rkvdec->link + VDPU383_LINK_TIMEOUT_THRESHOLD);
	writel(0, rkvdec->link + VDPU383_LINK_IP_ENABLE);
	writel(VDPU383_DEC_E_BIT, rkvdec->link + VDPU383_LINK_DEC_ENABLE);

	return 0;
}

static int rkvdec_h264_try_ctrl(struct rkvdec_ctx *ctx, struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_H264_SPS)
		return rkvdec_h264_validate_sps(ctx, ctrl->p_new.p_h264_sps);

	return 0;
}

const struct rkvdec_coded_fmt_ops rkvdec_vdpu383_h264_fmt_ops = {
	.adjust_fmt = rkvdec_h264_adjust_fmt,
	.get_image_fmt = rkvdec_h264_get_image_fmt,
	.start = rkvdec_h264_start,
	.stop = rkvdec_h264_stop,
	.run = rkvdec_h264_run,
	.try_ctrl = rkvdec_h264_try_ctrl,
};
