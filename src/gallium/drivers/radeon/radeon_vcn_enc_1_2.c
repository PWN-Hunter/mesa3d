/**************************************************************************
 *
 * Copyright 2017 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "pipe/p_video_codec.h"
#include "radeon_vcn_enc.h"
#include "radeon_video.h"
#include "si_pipe.h"
#include "util/u_video.h"

#include <stdio.h>

#define RENCODE_FW_INTERFACE_MAJOR_VERSION 1
#define RENCODE_FW_INTERFACE_MINOR_VERSION 2

#define RENCODE_IB_PARAM_SESSION_INFO              0x00000001
#define RENCODE_IB_PARAM_TASK_INFO                 0x00000002
#define RENCODE_IB_PARAM_SESSION_INIT              0x00000003
#define RENCODE_IB_PARAM_LAYER_CONTROL             0x00000004
#define RENCODE_IB_PARAM_LAYER_SELECT              0x00000005
#define RENCODE_IB_PARAM_RATE_CONTROL_SESSION_INIT 0x00000006
#define RENCODE_IB_PARAM_RATE_CONTROL_LAYER_INIT   0x00000007
#define RENCODE_IB_PARAM_RATE_CONTROL_PER_PICTURE  0x00000008
#define RENCODE_IB_PARAM_QUALITY_PARAMS            0x00000009
#define RENCODE_IB_PARAM_SLICE_HEADER              0x0000000a
#define RENCODE_IB_PARAM_ENCODE_PARAMS             0x0000000b
#define RENCODE_IB_PARAM_INTRA_REFRESH             0x0000000c
#define RENCODE_IB_PARAM_ENCODE_CONTEXT_BUFFER     0x0000000d
#define RENCODE_IB_PARAM_VIDEO_BITSTREAM_BUFFER    0x0000000e
#define RENCODE_IB_PARAM_FEEDBACK_BUFFER           0x00000010
#define RENCODE_IB_PARAM_DIRECT_OUTPUT_NALU        0x00000020

#define RENCODE_HEVC_IB_PARAM_SLICE_CONTROL     0x00100001
#define RENCODE_HEVC_IB_PARAM_SPEC_MISC         0x00100002
#define RENCODE_HEVC_IB_PARAM_DEBLOCKING_FILTER 0x00100003

#define RENCODE_H264_IB_PARAM_SLICE_CONTROL     0x00200001
#define RENCODE_H264_IB_PARAM_SPEC_MISC         0x00200002
#define RENCODE_H264_IB_PARAM_ENCODE_PARAMS     0x00200003
#define RENCODE_H264_IB_PARAM_DEBLOCKING_FILTER 0x00200004

static void radeon_enc_session_info(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.session_info);
   RADEON_ENC_CS(enc->enc_pic.session_info.interface_version);
   RADEON_ENC_READWRITE(enc->si->res->buf, enc->si->res->domains, 0x0);
   RADEON_ENC_CS(RENCODE_ENGINE_TYPE_ENCODE);
   RADEON_ENC_END();
}

static void radeon_enc_task_info(struct radeon_encoder *enc, bool need_feedback)
{
   enc->enc_pic.task_info.task_id++;

   if (need_feedback)
      enc->enc_pic.task_info.allowed_max_num_feedbacks = 1;
   else
      enc->enc_pic.task_info.allowed_max_num_feedbacks = 0;

   RADEON_ENC_BEGIN(enc->cmd.task_info);
   enc->p_task_size = &enc->cs->current.buf[enc->cs->current.cdw++];
   RADEON_ENC_CS(enc->enc_pic.task_info.task_id);
   RADEON_ENC_CS(enc->enc_pic.task_info.allowed_max_num_feedbacks);
   RADEON_ENC_END();
}

static void radeon_enc_session_init(struct radeon_encoder *enc)
{
   enc->enc_pic.session_init.encode_standard = RENCODE_ENCODE_STANDARD_H264;
   enc->enc_pic.session_init.aligned_picture_width = align(enc->base.width, 16);
   enc->enc_pic.session_init.aligned_picture_height = align(enc->base.height, 16);
   enc->enc_pic.session_init.padding_width =
      enc->enc_pic.session_init.aligned_picture_width - enc->base.width;
   enc->enc_pic.session_init.padding_height =
      enc->enc_pic.session_init.aligned_picture_height - enc->base.height;
   enc->enc_pic.session_init.pre_encode_mode = RENCODE_PREENCODE_MODE_NONE;
   enc->enc_pic.session_init.pre_encode_chroma_enabled = false;

   RADEON_ENC_BEGIN(enc->cmd.session_init);
   RADEON_ENC_CS(enc->enc_pic.session_init.encode_standard);
   RADEON_ENC_CS(enc->enc_pic.session_init.aligned_picture_width);
   RADEON_ENC_CS(enc->enc_pic.session_init.aligned_picture_height);
   RADEON_ENC_CS(enc->enc_pic.session_init.padding_width);
   RADEON_ENC_CS(enc->enc_pic.session_init.padding_height);
   RADEON_ENC_CS(enc->enc_pic.session_init.pre_encode_mode);
   RADEON_ENC_CS(enc->enc_pic.session_init.pre_encode_chroma_enabled);
   RADEON_ENC_END();
}

static void radeon_enc_session_init_hevc(struct radeon_encoder *enc)
{
   enc->enc_pic.session_init.encode_standard = RENCODE_ENCODE_STANDARD_HEVC;
   enc->enc_pic.session_init.aligned_picture_width = align(enc->base.width, 64);
   enc->enc_pic.session_init.aligned_picture_height = align(enc->base.height, 16);
   enc->enc_pic.session_init.padding_width =
      enc->enc_pic.session_init.aligned_picture_width - enc->base.width;
   enc->enc_pic.session_init.padding_height =
      enc->enc_pic.session_init.aligned_picture_height - enc->base.height;
   enc->enc_pic.session_init.pre_encode_mode = RENCODE_PREENCODE_MODE_NONE;
   enc->enc_pic.session_init.pre_encode_chroma_enabled = false;

   RADEON_ENC_BEGIN(enc->cmd.session_init);
   RADEON_ENC_CS(enc->enc_pic.session_init.encode_standard);
   RADEON_ENC_CS(enc->enc_pic.session_init.aligned_picture_width);
   RADEON_ENC_CS(enc->enc_pic.session_init.aligned_picture_height);
   RADEON_ENC_CS(enc->enc_pic.session_init.padding_width);
   RADEON_ENC_CS(enc->enc_pic.session_init.padding_height);
   RADEON_ENC_CS(enc->enc_pic.session_init.pre_encode_mode);
   RADEON_ENC_CS(enc->enc_pic.session_init.pre_encode_chroma_enabled);
   RADEON_ENC_END();
}

static void radeon_enc_layer_control(struct radeon_encoder *enc)
{
   enc->enc_pic.layer_ctrl.max_num_temporal_layers = 1;
   enc->enc_pic.layer_ctrl.num_temporal_layers = 1;

   RADEON_ENC_BEGIN(enc->cmd.layer_control);
   RADEON_ENC_CS(enc->enc_pic.layer_ctrl.max_num_temporal_layers);
   RADEON_ENC_CS(enc->enc_pic.layer_ctrl.num_temporal_layers);
   RADEON_ENC_END();
}

static void radeon_enc_layer_select(struct radeon_encoder *enc)
{
   enc->enc_pic.layer_sel.temporal_layer_index = 0;

   RADEON_ENC_BEGIN(enc->cmd.layer_select);
   RADEON_ENC_CS(enc->enc_pic.layer_sel.temporal_layer_index);
   RADEON_ENC_END();
}

static void radeon_enc_slice_control(struct radeon_encoder *enc)
{
   enc->enc_pic.slice_ctrl.slice_control_mode = RENCODE_H264_SLICE_CONTROL_MODE_FIXED_MBS;
   enc->enc_pic.slice_ctrl.num_mbs_per_slice =
      align(enc->base.width, 16) / 16 * align(enc->base.height, 16) / 16;

   RADEON_ENC_BEGIN(enc->cmd.slice_control_h264);
   RADEON_ENC_CS(enc->enc_pic.slice_ctrl.slice_control_mode);
   RADEON_ENC_CS(enc->enc_pic.slice_ctrl.num_mbs_per_slice);
   RADEON_ENC_END();
}

static void radeon_enc_slice_control_hevc(struct radeon_encoder *enc)
{
   enc->enc_pic.hevc_slice_ctrl.slice_control_mode = RENCODE_HEVC_SLICE_CONTROL_MODE_FIXED_CTBS;
   enc->enc_pic.hevc_slice_ctrl.fixed_ctbs_per_slice.num_ctbs_per_slice =
      align(enc->base.width, 64) / 64 * align(enc->base.height, 64) / 64;
   enc->enc_pic.hevc_slice_ctrl.fixed_ctbs_per_slice.num_ctbs_per_slice_segment =
      enc->enc_pic.hevc_slice_ctrl.fixed_ctbs_per_slice.num_ctbs_per_slice;

   RADEON_ENC_BEGIN(enc->cmd.slice_control_hevc);
   RADEON_ENC_CS(enc->enc_pic.hevc_slice_ctrl.slice_control_mode);
   RADEON_ENC_CS(enc->enc_pic.hevc_slice_ctrl.fixed_ctbs_per_slice.num_ctbs_per_slice);
   RADEON_ENC_CS(enc->enc_pic.hevc_slice_ctrl.fixed_ctbs_per_slice.num_ctbs_per_slice_segment);
   RADEON_ENC_END();
}

static void radeon_enc_spec_misc(struct radeon_encoder *enc)
{
   enc->enc_pic.spec_misc.constrained_intra_pred_flag = 0;
   enc->enc_pic.spec_misc.cabac_enable = 0;
   enc->enc_pic.spec_misc.cabac_init_idc = 0;
   enc->enc_pic.spec_misc.half_pel_enabled = 1;
   enc->enc_pic.spec_misc.quarter_pel_enabled = 1;
   enc->enc_pic.spec_misc.profile_idc = u_get_h264_profile_idc(enc->base.profile);
   enc->enc_pic.spec_misc.level_idc = enc->base.level;

   RADEON_ENC_BEGIN(enc->cmd.spec_misc_h264);
   RADEON_ENC_CS(enc->enc_pic.spec_misc.constrained_intra_pred_flag);
   RADEON_ENC_CS(enc->enc_pic.spec_misc.cabac_enable);
   RADEON_ENC_CS(enc->enc_pic.spec_misc.cabac_init_idc);
   RADEON_ENC_CS(enc->enc_pic.spec_misc.half_pel_enabled);
   RADEON_ENC_CS(enc->enc_pic.spec_misc.quarter_pel_enabled);
   RADEON_ENC_CS(enc->enc_pic.spec_misc.profile_idc);
   RADEON_ENC_CS(enc->enc_pic.spec_misc.level_idc);
   RADEON_ENC_END();
}

static void radeon_enc_spec_misc_hevc(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.spec_misc_hevc);
   RADEON_ENC_CS(enc->enc_pic.hevc_spec_misc.log2_min_luma_coding_block_size_minus3);
   RADEON_ENC_CS(enc->enc_pic.hevc_spec_misc.amp_disabled);
   RADEON_ENC_CS(enc->enc_pic.hevc_spec_misc.strong_intra_smoothing_enabled);
   RADEON_ENC_CS(enc->enc_pic.hevc_spec_misc.constrained_intra_pred_flag);
   RADEON_ENC_CS(enc->enc_pic.hevc_spec_misc.cabac_init_flag);
   RADEON_ENC_CS(enc->enc_pic.hevc_spec_misc.half_pel_enabled);
   RADEON_ENC_CS(enc->enc_pic.hevc_spec_misc.quarter_pel_enabled);
   RADEON_ENC_END();
}

static void radeon_enc_rc_session_init(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.rc_session_init);
   RADEON_ENC_CS(enc->enc_pic.rc_session_init.rate_control_method);
   RADEON_ENC_CS(enc->enc_pic.rc_session_init.vbv_buffer_level);
   RADEON_ENC_END();
}

static void radeon_enc_rc_layer_init(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.rc_layer_init);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.target_bit_rate);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.peak_bit_rate);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.frame_rate_num);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.frame_rate_den);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.vbv_buffer_size);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.avg_target_bits_per_picture);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.peak_bits_per_picture_integer);
   RADEON_ENC_CS(enc->enc_pic.rc_layer_init.peak_bits_per_picture_fractional);
   RADEON_ENC_END();
}

static void radeon_enc_deblocking_filter_h264(struct radeon_encoder *enc)
{
   enc->enc_pic.h264_deblock.disable_deblocking_filter_idc = 0;
   enc->enc_pic.h264_deblock.alpha_c0_offset_div2 = 0;
   enc->enc_pic.h264_deblock.beta_offset_div2 = 0;
   enc->enc_pic.h264_deblock.cb_qp_offset = 0;
   enc->enc_pic.h264_deblock.cr_qp_offset = 0;

   RADEON_ENC_BEGIN(enc->cmd.deblocking_filter_h264);
   RADEON_ENC_CS(enc->enc_pic.h264_deblock.disable_deblocking_filter_idc);
   RADEON_ENC_CS(enc->enc_pic.h264_deblock.alpha_c0_offset_div2);
   RADEON_ENC_CS(enc->enc_pic.h264_deblock.beta_offset_div2);
   RADEON_ENC_CS(enc->enc_pic.h264_deblock.cb_qp_offset);
   RADEON_ENC_CS(enc->enc_pic.h264_deblock.cr_qp_offset);
   RADEON_ENC_END();
}

static void radeon_enc_deblocking_filter_hevc(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.deblocking_filter_hevc);
   RADEON_ENC_CS(enc->enc_pic.hevc_deblock.loop_filter_across_slices_enabled);
   RADEON_ENC_CS(enc->enc_pic.hevc_deblock.deblocking_filter_disabled);
   RADEON_ENC_CS(enc->enc_pic.hevc_deblock.beta_offset_div2);
   RADEON_ENC_CS(enc->enc_pic.hevc_deblock.tc_offset_div2);
   RADEON_ENC_CS(enc->enc_pic.hevc_deblock.cb_qp_offset);
   RADEON_ENC_CS(enc->enc_pic.hevc_deblock.cr_qp_offset);
   RADEON_ENC_END();
}

static void radeon_enc_quality_params(struct radeon_encoder *enc)
{
   enc->enc_pic.quality_params.vbaq_mode = 0;
   enc->enc_pic.quality_params.scene_change_sensitivity = 0;
   enc->enc_pic.quality_params.scene_change_min_idr_interval = 0;

   RADEON_ENC_BEGIN(enc->cmd.quality_params);
   RADEON_ENC_CS(enc->enc_pic.quality_params.vbaq_mode);
   RADEON_ENC_CS(enc->enc_pic.quality_params.scene_change_sensitivity);
   RADEON_ENC_CS(enc->enc_pic.quality_params.scene_change_min_idr_interval);
   RADEON_ENC_END();
}

static void radeon_enc_nalu_sps(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.nalu);
   RADEON_ENC_CS(RENCODE_DIRECT_OUTPUT_NALU_TYPE_SPS);
   uint32_t *size_in_bytes = &enc->cs->current.buf[enc->cs->current.cdw++];
   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);
   radeon_enc_code_fixed_bits(enc, 0x00000001, 32);
   radeon_enc_code_fixed_bits(enc, 0x67, 8);
   radeon_enc_byte_align(enc);
   radeon_enc_set_emulation_prevention(enc, true);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.spec_misc.profile_idc, 8);
   radeon_enc_code_fixed_bits(enc, 0x44, 8); // hardcode to constrained baseline
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.spec_misc.level_idc, 8);
   radeon_enc_code_ue(enc, 0x0);

   if (enc->enc_pic.spec_misc.profile_idc == 100 || enc->enc_pic.spec_misc.profile_idc == 110 ||
       enc->enc_pic.spec_misc.profile_idc == 122 || enc->enc_pic.spec_misc.profile_idc == 244 ||
       enc->enc_pic.spec_misc.profile_idc == 44 || enc->enc_pic.spec_misc.profile_idc == 83 ||
       enc->enc_pic.spec_misc.profile_idc == 86 || enc->enc_pic.spec_misc.profile_idc == 118 ||
       enc->enc_pic.spec_misc.profile_idc == 128 || enc->enc_pic.spec_misc.profile_idc == 138) {
      radeon_enc_code_ue(enc, 0x1);
      radeon_enc_code_ue(enc, 0x0);
      radeon_enc_code_ue(enc, 0x0);
      radeon_enc_code_fixed_bits(enc, 0x0, 2);
   }

   radeon_enc_code_ue(enc, 1);
   radeon_enc_code_ue(enc, enc->enc_pic.pic_order_cnt_type);

   if (enc->enc_pic.pic_order_cnt_type == 0)
      radeon_enc_code_ue(enc, 1);

   radeon_enc_code_ue(enc, (enc->base.max_references + 1));
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.layer_ctrl.max_num_temporal_layers > 1 ? 0x1 : 0x0,
                              1);
   radeon_enc_code_ue(enc, (enc->enc_pic.session_init.aligned_picture_width / 16 - 1));
   radeon_enc_code_ue(enc, (enc->enc_pic.session_init.aligned_picture_height / 16 - 1));
   bool progressive_only = true;
   radeon_enc_code_fixed_bits(enc, progressive_only ? 0x1 : 0x0, 1);

   if (!progressive_only)
      radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   if ((enc->enc_pic.crop_left != 0) || (enc->enc_pic.crop_right != 0) ||
       (enc->enc_pic.crop_top != 0) || (enc->enc_pic.crop_bottom != 0)) {
      radeon_enc_code_fixed_bits(enc, 0x1, 1);
      radeon_enc_code_ue(enc, enc->enc_pic.crop_left);
      radeon_enc_code_ue(enc, enc->enc_pic.crop_right);
      radeon_enc_code_ue(enc, enc->enc_pic.crop_top);
      radeon_enc_code_ue(enc, enc->enc_pic.crop_bottom);
   } else
      radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 16);
   radeon_enc_code_ue(enc, 16);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, (enc->base.max_references + 1));

   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   radeon_enc_byte_align(enc);
   radeon_enc_flush_headers(enc);
   *size_in_bytes = (enc->bits_output + 7) / 8;
   RADEON_ENC_END();
}

static void radeon_enc_nalu_sps_hevc(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.nalu);
   RADEON_ENC_CS(RENCODE_DIRECT_OUTPUT_NALU_TYPE_SPS);
   uint32_t *size_in_bytes = &enc->cs->current.buf[enc->cs->current.cdw++];
   int i;

   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);
   radeon_enc_code_fixed_bits(enc, 0x00000001, 32);
   radeon_enc_code_fixed_bits(enc, 0x4201, 16);
   radeon_enc_byte_align(enc);
   radeon_enc_set_emulation_prevention(enc, true);
   radeon_enc_code_fixed_bits(enc, 0x0, 4);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1, 3);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 2);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.general_tier_flag, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.general_profile_idc, 5);
   radeon_enc_code_fixed_bits(enc, 0x60000000, 32);
   radeon_enc_code_fixed_bits(enc, 0xb0000000, 32);
   radeon_enc_code_fixed_bits(enc, 0x0, 16);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.general_level_idc, 8);

   for (i = 0; i < (enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1); i++)
      radeon_enc_code_fixed_bits(enc, 0x0, 2);

   if ((enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1) > 0) {
      for (i = (enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1); i < 8; i++)
         radeon_enc_code_fixed_bits(enc, 0x0, 2);
   }

   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, enc->enc_pic.chroma_format_idc);
   radeon_enc_code_ue(enc, enc->enc_pic.session_init.aligned_picture_width);
   radeon_enc_code_ue(enc, enc->enc_pic.session_init.aligned_picture_height);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_ue(enc, enc->enc_pic.bit_depth_luma_minus8);
   radeon_enc_code_ue(enc, enc->enc_pic.bit_depth_chroma_minus8);
   radeon_enc_code_ue(enc, enc->enc_pic.log2_max_poc - 4);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_ue(enc, 1);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, enc->enc_pic.hevc_spec_misc.log2_min_luma_coding_block_size_minus3);
   // Only support CTBSize 64
   radeon_enc_code_ue(enc,
                      6 - (enc->enc_pic.hevc_spec_misc.log2_min_luma_coding_block_size_minus3 + 3));
   radeon_enc_code_ue(enc, enc->enc_pic.log2_min_transform_block_size_minus2);
   radeon_enc_code_ue(enc, enc->enc_pic.log2_diff_max_min_transform_block_size);
   radeon_enc_code_ue(enc, enc->enc_pic.max_transform_hierarchy_depth_inter);
   radeon_enc_code_ue(enc, enc->enc_pic.max_transform_hierarchy_depth_intra);

   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, !enc->enc_pic.hevc_spec_misc.amp_disabled, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.sample_adaptive_offset_enabled_flag, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.pcm_enabled_flag, 1);

   radeon_enc_code_ue(enc, 1);
   radeon_enc_code_ue(enc, 1);
   radeon_enc_code_ue(enc, 0);
   radeon_enc_code_ue(enc, 0);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_fixed_bits(enc, 0, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.hevc_spec_misc.strong_intra_smoothing_enabled, 1);

   radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   radeon_enc_byte_align(enc);
   radeon_enc_flush_headers(enc);
   *size_in_bytes = (enc->bits_output + 7) / 8;
   RADEON_ENC_END();
}

static void radeon_enc_nalu_pps(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.nalu);
   RADEON_ENC_CS(RENCODE_DIRECT_OUTPUT_NALU_TYPE_PPS);
   uint32_t *size_in_bytes = &enc->cs->current.buf[enc->cs->current.cdw++];
   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);
   radeon_enc_code_fixed_bits(enc, 0x00000001, 32);
   radeon_enc_code_fixed_bits(enc, 0x68, 8);
   radeon_enc_byte_align(enc);
   radeon_enc_set_emulation_prevention(enc, true);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_fixed_bits(enc, (enc->enc_pic.spec_misc.cabac_enable ? 0x1 : 0x0), 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 2);
   radeon_enc_code_se(enc, 0x0);
   radeon_enc_code_se(enc, 0x0);
   radeon_enc_code_se(enc, 0x0);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   radeon_enc_byte_align(enc);
   radeon_enc_flush_headers(enc);
   *size_in_bytes = (enc->bits_output + 7) / 8;
   RADEON_ENC_END();
}

static void radeon_enc_nalu_pps_hevc(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.nalu);
   RADEON_ENC_CS(RENCODE_DIRECT_OUTPUT_NALU_TYPE_PPS);
   uint32_t *size_in_bytes = &enc->cs->current.buf[enc->cs->current.cdw++];
   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);
   radeon_enc_code_fixed_bits(enc, 0x00000001, 32);
   radeon_enc_code_fixed_bits(enc, 0x4401, 16);
   radeon_enc_byte_align(enc);
   radeon_enc_set_emulation_prevention(enc, true);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 4);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_se(enc, 0x0);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.hevc_spec_misc.constrained_intra_pred_flag, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   if (enc->enc_pic.rc_session_init.rate_control_method == RENCODE_RATE_CONTROL_METHOD_NONE)
      radeon_enc_code_fixed_bits(enc, 0x0, 1);
   else {
      radeon_enc_code_fixed_bits(enc, 0x1, 1);
      radeon_enc_code_ue(enc, 0x0);
   }
   radeon_enc_code_se(enc, enc->enc_pic.hevc_deblock.cb_qp_offset);
   radeon_enc_code_se(enc, enc->enc_pic.hevc_deblock.cr_qp_offset);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 2);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.hevc_deblock.loop_filter_across_slices_enabled, 1);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.hevc_deblock.deblocking_filter_disabled, 1);

   if (!enc->enc_pic.hevc_deblock.deblocking_filter_disabled) {
      radeon_enc_code_se(enc, enc->enc_pic.hevc_deblock.beta_offset_div2);
      radeon_enc_code_se(enc, enc->enc_pic.hevc_deblock.tc_offset_div2);
   }

   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_ue(enc, enc->enc_pic.log2_parallel_merge_level_minus2);
   radeon_enc_code_fixed_bits(enc, 0x0, 2);

   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   radeon_enc_byte_align(enc);
   radeon_enc_flush_headers(enc);
   *size_in_bytes = (enc->bits_output + 7) / 8;
   RADEON_ENC_END();
}

static void radeon_enc_nalu_vps(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.nalu);
   RADEON_ENC_CS(RENCODE_DIRECT_OUTPUT_NALU_TYPE_VPS);
   uint32_t *size_in_bytes = &enc->cs->current.buf[enc->cs->current.cdw++];
   int i;

   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);
   radeon_enc_code_fixed_bits(enc, 0x00000001, 32);
   radeon_enc_code_fixed_bits(enc, 0x4001, 16);
   radeon_enc_byte_align(enc);
   radeon_enc_set_emulation_prevention(enc, true);

   radeon_enc_code_fixed_bits(enc, 0x0, 4);
   radeon_enc_code_fixed_bits(enc, 0x3, 2);
   radeon_enc_code_fixed_bits(enc, 0x0, 6);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1, 3);
   radeon_enc_code_fixed_bits(enc, 0x1, 1);
   radeon_enc_code_fixed_bits(enc, 0xffff, 16);
   radeon_enc_code_fixed_bits(enc, 0x0, 2);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.general_tier_flag, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.general_profile_idc, 5);
   radeon_enc_code_fixed_bits(enc, 0x60000000, 32);
   radeon_enc_code_fixed_bits(enc, 0xb0000000, 32);
   radeon_enc_code_fixed_bits(enc, 0x0, 16);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.general_level_idc, 8);

   for (i = 0; i < (enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1); i++)
      radeon_enc_code_fixed_bits(enc, 0x0, 2);

   if ((enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1) > 0) {
      for (i = (enc->enc_pic.layer_ctrl.max_num_temporal_layers - 1); i < 8; i++)
         radeon_enc_code_fixed_bits(enc, 0x0, 2);
   }

   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_ue(enc, 0x1);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_ue(enc, 0x0);

   radeon_enc_code_fixed_bits(enc, 0x0, 6);
   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   radeon_enc_byte_align(enc);
   radeon_enc_flush_headers(enc);
   *size_in_bytes = (enc->bits_output + 7) / 8;
   RADEON_ENC_END();
}

static void radeon_enc_nalu_aud_hevc(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.nalu);
   RADEON_ENC_CS(RENCODE_DIRECT_OUTPUT_NALU_TYPE_AUD);
   uint32_t *size_in_bytes = &enc->cs->current.buf[enc->cs->current.cdw++];
   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);
   radeon_enc_code_fixed_bits(enc, 0x00000001, 32);
   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, 35, 6);
   radeon_enc_code_fixed_bits(enc, 0x0, 6);
   radeon_enc_code_fixed_bits(enc, 0x1, 3);
   radeon_enc_byte_align(enc);
   radeon_enc_set_emulation_prevention(enc, true);
   switch (enc->enc_pic.picture_type) {
   case PIPE_H265_ENC_PICTURE_TYPE_I:
   case PIPE_H265_ENC_PICTURE_TYPE_IDR:
      radeon_enc_code_fixed_bits(enc, 0x00, 3);
      break;
   case PIPE_H265_ENC_PICTURE_TYPE_P:
      radeon_enc_code_fixed_bits(enc, 0x01, 3);
      break;
   case PIPE_H265_ENC_PICTURE_TYPE_B:
      radeon_enc_code_fixed_bits(enc, 0x02, 3);
      break;
   default:
      radeon_enc_code_fixed_bits(enc, 0x02, 3);
   }

   radeon_enc_code_fixed_bits(enc, 0x1, 1);

   radeon_enc_byte_align(enc);
   radeon_enc_flush_headers(enc);
   *size_in_bytes = (enc->bits_output + 7) / 8;
   RADEON_ENC_END();
}

static void radeon_enc_slice_header(struct radeon_encoder *enc)
{
   uint32_t instruction[RENCODE_SLICE_HEADER_TEMPLATE_MAX_NUM_INSTRUCTIONS] = {0};
   uint32_t num_bits[RENCODE_SLICE_HEADER_TEMPLATE_MAX_NUM_INSTRUCTIONS] = {0};
   unsigned int inst_index = 0;
   unsigned int bit_index = 0;
   unsigned int bits_copied = 0;
   RADEON_ENC_BEGIN(enc->cmd.slice_header);
   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);

   if (enc->enc_pic.is_idr)
      radeon_enc_code_fixed_bits(enc, 0x65, 8);
   else if (enc->enc_pic.not_referenced)
      radeon_enc_code_fixed_bits(enc, 0x01, 8);
   else
      radeon_enc_code_fixed_bits(enc, 0x41, 8);

   radeon_enc_flush_headers(enc);
   bit_index++;
   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_COPY;
   num_bits[inst_index] = enc->bits_output - bits_copied;
   bits_copied = enc->bits_output;
   inst_index++;

   instruction[inst_index] = RENCODE_H264_HEADER_INSTRUCTION_FIRST_MB;
   inst_index++;

   switch (enc->enc_pic.picture_type) {
   case PIPE_H264_ENC_PICTURE_TYPE_I:
   case PIPE_H264_ENC_PICTURE_TYPE_IDR:
      radeon_enc_code_fixed_bits(enc, 0x08, 7);
      break;
   case PIPE_H264_ENC_PICTURE_TYPE_P:
   case PIPE_H264_ENC_PICTURE_TYPE_SKIP:
      radeon_enc_code_fixed_bits(enc, 0x06, 5);
      break;
   case PIPE_H264_ENC_PICTURE_TYPE_B:
      radeon_enc_code_fixed_bits(enc, 0x07, 5);
      break;
   default:
      radeon_enc_code_fixed_bits(enc, 0x08, 7);
   }

   radeon_enc_code_ue(enc, 0x0);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.frame_num % 32, 5);

   if (enc->enc_pic.h264_enc_params.input_picture_structure !=
       RENCODE_H264_PICTURE_STRUCTURE_FRAME) {
      radeon_enc_code_fixed_bits(enc, 0x1, 1);
      radeon_enc_code_fixed_bits(enc,
                                 enc->enc_pic.h264_enc_params.input_picture_structure ==
                                       RENCODE_H264_PICTURE_STRUCTURE_BOTTOM_FIELD
                                    ? 1
                                    : 0,
                                 1);
   }

   if (enc->enc_pic.is_idr)
      radeon_enc_code_ue(enc, enc->enc_pic.is_even_frame);

   enc->enc_pic.is_even_frame = !enc->enc_pic.is_even_frame;

   if (enc->enc_pic.pic_order_cnt_type == 0)
      radeon_enc_code_fixed_bits(enc, enc->enc_pic.pic_order_cnt % 32, 5);

   if (enc->enc_pic.picture_type != PIPE_H264_ENC_PICTURE_TYPE_IDR) {
      radeon_enc_code_fixed_bits(enc, 0x0, 1);

      if (enc->enc_pic.frame_num - enc->enc_pic.ref_idx_l0 > 1) {
         radeon_enc_code_fixed_bits(enc, 0x1, 1);
         radeon_enc_code_ue(enc, 0x0);
         radeon_enc_code_ue(enc, (enc->enc_pic.frame_num - enc->enc_pic.ref_idx_l0 - 1));
         radeon_enc_code_ue(enc, 0x3);
      } else
         radeon_enc_code_fixed_bits(enc, 0x0, 1);
   }

   if (enc->enc_pic.is_idr) {
      radeon_enc_code_fixed_bits(enc, 0x0, 1);
      radeon_enc_code_fixed_bits(enc, 0x0, 1);
   } else
      radeon_enc_code_fixed_bits(enc, 0x0, 1);

   if ((enc->enc_pic.picture_type != PIPE_H264_ENC_PICTURE_TYPE_IDR) &&
       (enc->enc_pic.spec_misc.cabac_enable))
      radeon_enc_code_ue(enc, enc->enc_pic.spec_misc.cabac_init_idc);

   radeon_enc_flush_headers(enc);
   bit_index++;
   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_COPY;
   num_bits[inst_index] = enc->bits_output - bits_copied;
   bits_copied = enc->bits_output;
   inst_index++;

   instruction[inst_index] = RENCODE_H264_HEADER_INSTRUCTION_SLICE_QP_DELTA;
   inst_index++;

   radeon_enc_code_ue(enc, enc->enc_pic.h264_deblock.disable_deblocking_filter_idc ? 1 : 0);

   if (!enc->enc_pic.h264_deblock.disable_deblocking_filter_idc) {
      radeon_enc_code_se(enc, enc->enc_pic.h264_deblock.alpha_c0_offset_div2);
      radeon_enc_code_se(enc, enc->enc_pic.h264_deblock.beta_offset_div2);
   }

   radeon_enc_flush_headers(enc);
   bit_index++;
   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_COPY;
   num_bits[inst_index] = enc->bits_output - bits_copied;
   bits_copied = enc->bits_output;
   inst_index++;

   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_END;

   for (int i = bit_index; i < RENCODE_SLICE_HEADER_TEMPLATE_MAX_TEMPLATE_SIZE_IN_DWORDS; i++)
      RADEON_ENC_CS(0x00000000);

   for (int j = 0; j < RENCODE_SLICE_HEADER_TEMPLATE_MAX_NUM_INSTRUCTIONS; j++) {
      RADEON_ENC_CS(instruction[j]);
      RADEON_ENC_CS(num_bits[j]);
   }

   RADEON_ENC_END();
}

static void radeon_enc_slice_header_hevc(struct radeon_encoder *enc)
{
   uint32_t instruction[RENCODE_SLICE_HEADER_TEMPLATE_MAX_NUM_INSTRUCTIONS] = {0};
   uint32_t num_bits[RENCODE_SLICE_HEADER_TEMPLATE_MAX_NUM_INSTRUCTIONS] = {0};
   unsigned int inst_index = 0;
   unsigned int bit_index = 0;
   unsigned int bits_copied = 0;
   RADEON_ENC_BEGIN(enc->cmd.slice_header);
   radeon_enc_reset(enc);
   radeon_enc_set_emulation_prevention(enc, false);

   radeon_enc_code_fixed_bits(enc, 0x0, 1);
   radeon_enc_code_fixed_bits(enc, enc->enc_pic.nal_unit_type, 6);
   radeon_enc_code_fixed_bits(enc, 0x0, 6);
   radeon_enc_code_fixed_bits(enc, 0x1, 3);

   radeon_enc_flush_headers(enc);
   bit_index++;
   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_COPY;
   num_bits[inst_index] = enc->bits_output - bits_copied;
   bits_copied = enc->bits_output;
   inst_index++;

   instruction[inst_index] = RENCODE_HEVC_HEADER_INSTRUCTION_FIRST_SLICE;
   inst_index++;

   if ((enc->enc_pic.nal_unit_type >= 16) && (enc->enc_pic.nal_unit_type <= 23))
      radeon_enc_code_fixed_bits(enc, 0x0, 1);

   radeon_enc_code_ue(enc, 0x0);

   radeon_enc_flush_headers(enc);
   bit_index++;
   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_COPY;
   num_bits[inst_index] = enc->bits_output - bits_copied;
   bits_copied = enc->bits_output;
   inst_index++;

   instruction[inst_index] = RENCODE_HEVC_HEADER_INSTRUCTION_SLICE_SEGMENT;
   inst_index++;

   instruction[inst_index] = RENCODE_HEVC_HEADER_INSTRUCTION_DEPENDENT_SLICE_END;
   inst_index++;

   switch (enc->enc_pic.picture_type) {
   case PIPE_H265_ENC_PICTURE_TYPE_I:
   case PIPE_H265_ENC_PICTURE_TYPE_IDR:
      radeon_enc_code_ue(enc, 0x2);
      break;
   case PIPE_H265_ENC_PICTURE_TYPE_P:
   case PIPE_H265_ENC_PICTURE_TYPE_SKIP:
      radeon_enc_code_ue(enc, 0x1);
      break;
   case PIPE_H265_ENC_PICTURE_TYPE_B:
      radeon_enc_code_ue(enc, 0x0);
      break;
   default:
      radeon_enc_code_ue(enc, 0x1);
   }

   if ((enc->enc_pic.nal_unit_type != 19) && (enc->enc_pic.nal_unit_type != 20)) {
      radeon_enc_code_fixed_bits(enc, enc->enc_pic.pic_order_cnt, enc->enc_pic.log2_max_poc);
      if (enc->enc_pic.picture_type == PIPE_H264_ENC_PICTURE_TYPE_P)
         radeon_enc_code_fixed_bits(enc, 0x1, 1);
      else {
         radeon_enc_code_fixed_bits(enc, 0x0, 1);
         radeon_enc_code_fixed_bits(enc, 0x0, 1);
         radeon_enc_code_ue(enc, 0x0);
         radeon_enc_code_ue(enc, 0x0);
      }
   }

   if ((enc->enc_pic.picture_type == PIPE_H264_ENC_PICTURE_TYPE_P) ||
       (enc->enc_pic.picture_type == PIPE_H264_ENC_PICTURE_TYPE_B)) {
      radeon_enc_code_fixed_bits(enc, 0x0, 1);
      radeon_enc_code_fixed_bits(enc, enc->enc_pic.hevc_spec_misc.cabac_init_flag, 1);
      radeon_enc_code_ue(enc, 5 - enc->enc_pic.max_num_merge_cand);
   }

   radeon_enc_flush_headers(enc);
   bit_index++;
   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_COPY;
   num_bits[inst_index] = enc->bits_output - bits_copied;
   bits_copied = enc->bits_output;
   inst_index++;

   instruction[inst_index] = RENCODE_HEVC_HEADER_INSTRUCTION_SLICE_QP_DELTA;
   inst_index++;

   if ((enc->enc_pic.hevc_deblock.loop_filter_across_slices_enabled) &&
       (!enc->enc_pic.hevc_deblock.deblocking_filter_disabled)) {
      radeon_enc_code_fixed_bits(enc, enc->enc_pic.hevc_deblock.loop_filter_across_slices_enabled,
                                 1);

      radeon_enc_flush_headers(enc);
      bit_index++;
      instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_COPY;
      num_bits[inst_index] = enc->bits_output - bits_copied;
      bits_copied = enc->bits_output;
      inst_index++;
   }

   instruction[inst_index] = RENCODE_HEADER_INSTRUCTION_END;

   for (int i = bit_index; i < RENCODE_SLICE_HEADER_TEMPLATE_MAX_TEMPLATE_SIZE_IN_DWORDS; i++)
      RADEON_ENC_CS(0x00000000);

   for (int j = 0; j < RENCODE_SLICE_HEADER_TEMPLATE_MAX_NUM_INSTRUCTIONS; j++) {
      RADEON_ENC_CS(instruction[j]);
      RADEON_ENC_CS(num_bits[j]);
   }

   RADEON_ENC_END();
}

static void radeon_enc_ctx(struct radeon_encoder *enc)
{
   enc->enc_pic.ctx_buf.swizzle_mode = 0;
   enc->enc_pic.ctx_buf.rec_luma_pitch = align(enc->base.width, enc->alignment);
   enc->enc_pic.ctx_buf.rec_chroma_pitch = align(enc->base.width, enc->alignment);
   enc->enc_pic.ctx_buf.num_reconstructed_pictures = 2;

   RADEON_ENC_BEGIN(enc->cmd.ctx);
   RADEON_ENC_READWRITE(enc->cpb.res->buf, enc->cpb.res->domains, 0);
   RADEON_ENC_CS(enc->enc_pic.ctx_buf.swizzle_mode);
   RADEON_ENC_CS(enc->enc_pic.ctx_buf.rec_luma_pitch);
   RADEON_ENC_CS(enc->enc_pic.ctx_buf.rec_chroma_pitch);
   RADEON_ENC_CS(enc->enc_pic.ctx_buf.num_reconstructed_pictures);
   /* reconstructed_picture_1_luma_offset */
   RADEON_ENC_CS(0x00000000);
   /* reconstructed_picture_1_chroma_offset */
   RADEON_ENC_CS(align(enc->base.width, enc->alignment) * align(enc->base.height, 16));
   /* reconstructed_picture_2_luma_offset */
   RADEON_ENC_CS(align(enc->base.width, enc->alignment) * align(enc->base.height, 16) * 3 / 2);
   /* reconstructed_picture_2_chroma_offset */
   RADEON_ENC_CS(align(enc->base.width, enc->alignment) * align(enc->base.height, 16) * 5 / 2);

   for (int i = 0; i < 136; i++)
      RADEON_ENC_CS(0x00000000);

   RADEON_ENC_END();
}

static void radeon_enc_bitstream(struct radeon_encoder *enc)
{
   enc->enc_pic.bit_buf.mode = RENCODE_REC_SWIZZLE_MODE_LINEAR;
   enc->enc_pic.bit_buf.video_bitstream_buffer_size = enc->bs_size;
   enc->enc_pic.bit_buf.video_bitstream_data_offset = 0;

   RADEON_ENC_BEGIN(enc->cmd.bitstream);
   RADEON_ENC_CS(enc->enc_pic.bit_buf.mode);
   RADEON_ENC_WRITE(enc->bs_handle, RADEON_DOMAIN_GTT, 0);
   RADEON_ENC_CS(enc->enc_pic.bit_buf.video_bitstream_buffer_size);
   RADEON_ENC_CS(enc->enc_pic.bit_buf.video_bitstream_data_offset);
   RADEON_ENC_END();
}

static void radeon_enc_feedback(struct radeon_encoder *enc)
{
   enc->enc_pic.fb_buf.mode = RENCODE_FEEDBACK_BUFFER_MODE_LINEAR;
   enc->enc_pic.fb_buf.feedback_buffer_size = 16;
   enc->enc_pic.fb_buf.feedback_data_size = 40;

   RADEON_ENC_BEGIN(enc->cmd.feedback);
   RADEON_ENC_CS(enc->enc_pic.fb_buf.mode);
   RADEON_ENC_WRITE(enc->fb->res->buf, enc->fb->res->domains, 0x0);
   RADEON_ENC_CS(enc->enc_pic.fb_buf.feedback_buffer_size);
   RADEON_ENC_CS(enc->enc_pic.fb_buf.feedback_data_size);
   RADEON_ENC_END();
}

static void radeon_enc_intra_refresh(struct radeon_encoder *enc)
{
   enc->enc_pic.intra_ref.intra_refresh_mode = RENCODE_INTRA_REFRESH_MODE_NONE;
   enc->enc_pic.intra_ref.offset = 0;
   enc->enc_pic.intra_ref.region_size = 0;

   RADEON_ENC_BEGIN(enc->cmd.intra_refresh);
   RADEON_ENC_CS(enc->enc_pic.intra_ref.intra_refresh_mode);
   RADEON_ENC_CS(enc->enc_pic.intra_ref.offset);
   RADEON_ENC_CS(enc->enc_pic.intra_ref.region_size);
   RADEON_ENC_END();
}

static void radeon_enc_rc_per_pic(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(enc->cmd.rc_per_pic);
   RADEON_ENC_CS(enc->enc_pic.rc_per_pic.qp);
   RADEON_ENC_CS(enc->enc_pic.rc_per_pic.min_qp_app);
   RADEON_ENC_CS(enc->enc_pic.rc_per_pic.max_qp_app);
   RADEON_ENC_CS(enc->enc_pic.rc_per_pic.max_au_size);
   RADEON_ENC_CS(enc->enc_pic.rc_per_pic.enabled_filler_data);
   RADEON_ENC_CS(enc->enc_pic.rc_per_pic.skip_frame_enable);
   RADEON_ENC_CS(enc->enc_pic.rc_per_pic.enforce_hrd);
   RADEON_ENC_END();
}

static void radeon_enc_encode_params(struct radeon_encoder *enc)
{
   switch (enc->enc_pic.picture_type) {
   case PIPE_H264_ENC_PICTURE_TYPE_I:
   case PIPE_H264_ENC_PICTURE_TYPE_IDR:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_I;
      break;
   case PIPE_H264_ENC_PICTURE_TYPE_P:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_P;
      break;
   case PIPE_H264_ENC_PICTURE_TYPE_SKIP:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_P_SKIP;
      break;
   case PIPE_H264_ENC_PICTURE_TYPE_B:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_B;
      break;
   default:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_I;
   }

   enc->enc_pic.enc_params.allowed_max_bitstream_size = enc->bs_size;
   enc->enc_pic.enc_params.input_pic_luma_pitch = enc->luma->u.gfx9.surf_pitch;
   enc->enc_pic.enc_params.input_pic_chroma_pitch = enc->chroma->u.gfx9.surf_pitch;
   enc->enc_pic.enc_params.input_pic_swizzle_mode = RENCODE_INPUT_SWIZZLE_MODE_LINEAR;

   if (enc->enc_pic.picture_type == PIPE_H264_ENC_PICTURE_TYPE_IDR)
      enc->enc_pic.enc_params.reference_picture_index = 0xFFFFFFFF;
   else
      enc->enc_pic.enc_params.reference_picture_index = (enc->enc_pic.frame_num - 1) % 2;

   enc->enc_pic.enc_params.reconstructed_picture_index = enc->enc_pic.frame_num % 2;

   RADEON_ENC_BEGIN(enc->cmd.enc_params);
   RADEON_ENC_CS(enc->enc_pic.enc_params.pic_type);
   RADEON_ENC_CS(enc->enc_pic.enc_params.allowed_max_bitstream_size);
   RADEON_ENC_READ(enc->handle, RADEON_DOMAIN_VRAM, enc->luma->u.gfx9.surf_offset);
   RADEON_ENC_READ(enc->handle, RADEON_DOMAIN_VRAM, enc->chroma->u.gfx9.surf_offset);
   RADEON_ENC_CS(enc->enc_pic.enc_params.input_pic_luma_pitch);
   RADEON_ENC_CS(enc->enc_pic.enc_params.input_pic_chroma_pitch);
   RADEON_ENC_CS(enc->enc_pic.enc_params.input_pic_swizzle_mode);
   RADEON_ENC_CS(enc->enc_pic.enc_params.reference_picture_index);
   RADEON_ENC_CS(enc->enc_pic.enc_params.reconstructed_picture_index);
   RADEON_ENC_END();
}

static void radeon_enc_encode_params_hevc(struct radeon_encoder *enc)
{
   switch (enc->enc_pic.picture_type) {
   case PIPE_H265_ENC_PICTURE_TYPE_I:
   case PIPE_H265_ENC_PICTURE_TYPE_IDR:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_I;
      break;
   case PIPE_H265_ENC_PICTURE_TYPE_P:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_P;
      break;
   case PIPE_H265_ENC_PICTURE_TYPE_SKIP:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_P_SKIP;
      break;
   case PIPE_H265_ENC_PICTURE_TYPE_B:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_B;
      break;
   default:
      enc->enc_pic.enc_params.pic_type = RENCODE_PICTURE_TYPE_I;
   }

   enc->enc_pic.enc_params.allowed_max_bitstream_size = enc->bs_size;
   enc->enc_pic.enc_params.input_pic_luma_pitch = enc->luma->u.gfx9.surf_pitch;
   enc->enc_pic.enc_params.input_pic_chroma_pitch = enc->chroma->u.gfx9.surf_pitch;
   enc->enc_pic.enc_params.input_pic_swizzle_mode = RENCODE_INPUT_SWIZZLE_MODE_LINEAR;

   if (enc->enc_pic.enc_params.pic_type == RENCODE_PICTURE_TYPE_I)
      enc->enc_pic.enc_params.reference_picture_index = 0xFFFFFFFF;
   else
      enc->enc_pic.enc_params.reference_picture_index = (enc->enc_pic.frame_num - 1) % 2;

   enc->enc_pic.enc_params.reconstructed_picture_index = enc->enc_pic.frame_num % 2;

   RADEON_ENC_BEGIN(enc->cmd.enc_params);
   RADEON_ENC_CS(enc->enc_pic.enc_params.pic_type);
   RADEON_ENC_CS(enc->enc_pic.enc_params.allowed_max_bitstream_size);
   RADEON_ENC_READ(enc->handle, RADEON_DOMAIN_VRAM, enc->luma->u.gfx9.surf_offset);
   RADEON_ENC_READ(enc->handle, RADEON_DOMAIN_VRAM, enc->chroma->u.gfx9.surf_offset);
   RADEON_ENC_CS(enc->enc_pic.enc_params.input_pic_luma_pitch);
   RADEON_ENC_CS(enc->enc_pic.enc_params.input_pic_chroma_pitch);
   RADEON_ENC_CS(enc->enc_pic.enc_params.input_pic_swizzle_mode);
   RADEON_ENC_CS(enc->enc_pic.enc_params.reference_picture_index);
   RADEON_ENC_CS(enc->enc_pic.enc_params.reconstructed_picture_index);
   RADEON_ENC_END();
}

static void radeon_enc_encode_params_h264(struct radeon_encoder *enc)
{
   enc->enc_pic.h264_enc_params.input_picture_structure = RENCODE_H264_PICTURE_STRUCTURE_FRAME;
   enc->enc_pic.h264_enc_params.interlaced_mode = RENCODE_H264_INTERLACING_MODE_PROGRESSIVE;
   enc->enc_pic.h264_enc_params.reference_picture_structure = RENCODE_H264_PICTURE_STRUCTURE_FRAME;
   enc->enc_pic.h264_enc_params.reference_picture1_index = 0xFFFFFFFF;

   RADEON_ENC_BEGIN(enc->cmd.enc_params_h264);
   RADEON_ENC_CS(enc->enc_pic.h264_enc_params.input_picture_structure);
   RADEON_ENC_CS(enc->enc_pic.h264_enc_params.interlaced_mode);
   RADEON_ENC_CS(enc->enc_pic.h264_enc_params.reference_picture_structure);
   RADEON_ENC_CS(enc->enc_pic.h264_enc_params.reference_picture1_index);
   RADEON_ENC_END();
}

static void radeon_enc_op_init(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(RENCODE_IB_OP_INITIALIZE);
   RADEON_ENC_END();
}

static void radeon_enc_op_close(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(RENCODE_IB_OP_CLOSE_SESSION);
   RADEON_ENC_END();
}

static void radeon_enc_op_enc(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(RENCODE_IB_OP_ENCODE);
   RADEON_ENC_END();
}

static void radeon_enc_op_init_rc(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(RENCODE_IB_OP_INIT_RC);
   RADEON_ENC_END();
}

static void radeon_enc_op_init_rc_vbv(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(RENCODE_IB_OP_INIT_RC_VBV_BUFFER_LEVEL);
   RADEON_ENC_END();
}

static void radeon_enc_op_speed(struct radeon_encoder *enc)
{
   RADEON_ENC_BEGIN(RENCODE_IB_OP_SET_SPEED_ENCODING_MODE);
   RADEON_ENC_END();
}

static void begin(struct radeon_encoder *enc)
{
   enc->session_info(enc);
   enc->total_task_size = 0;
   enc->task_info(enc, enc->need_feedback);
   enc->op_init(enc);

   enc->session_init(enc);
   enc->slice_control(enc);
   enc->spec_misc(enc);
   enc->deblocking_filter(enc);

   enc->layer_control(enc);
   enc->rc_session_init(enc);
   enc->quality_params(enc);
   enc->layer_select(enc);
   enc->rc_layer_init(enc);
   enc->layer_select(enc);
   enc->rc_per_pic(enc);
   enc->op_init_rc(enc);
   enc->op_init_rc_vbv(enc);
   *enc->p_task_size = (enc->total_task_size);
}

static void radeon_enc_headers_h264(struct radeon_encoder *enc)
{
   if (enc->enc_pic.is_idr) {
      enc->nalu_sps(enc);
      enc->nalu_pps(enc);
   }
   enc->slice_header(enc);
   enc->encode_params(enc);
   enc->encode_params_codec_spec(enc);
}

static void radeon_enc_headers_hevc(struct radeon_encoder *enc)
{
   enc->nalu_aud(enc);
   if (enc->enc_pic.is_idr) {
      enc->nalu_vps(enc);
      enc->nalu_pps(enc);
      enc->nalu_sps(enc);
   }
   enc->slice_header(enc);
   enc->encode_params(enc);
}

static void encode(struct radeon_encoder *enc)
{
   enc->session_info(enc);
   enc->total_task_size = 0;
   enc->task_info(enc, enc->need_feedback);

   enc->encode_headers(enc);
   enc->ctx(enc);
   enc->bitstream(enc);
   enc->feedback(enc);
   enc->intra_refresh(enc);

   enc->op_speed(enc);
   enc->op_enc(enc);
   *enc->p_task_size = (enc->total_task_size);
}

static void destroy(struct radeon_encoder *enc)
{
   enc->session_info(enc);
   enc->total_task_size = 0;
   enc->task_info(enc, enc->need_feedback);
   enc->op_close(enc);
   *enc->p_task_size = (enc->total_task_size);
}

void radeon_enc_1_2_init(struct radeon_encoder *enc)
{
   enc->begin = begin;
   enc->encode = encode;
   enc->destroy = destroy;
   enc->session_info = radeon_enc_session_info;
   enc->task_info = radeon_enc_task_info;
   enc->layer_control = radeon_enc_layer_control;
   enc->layer_select = radeon_enc_layer_select;
   enc->rc_session_init = radeon_enc_rc_session_init;
   enc->rc_layer_init = radeon_enc_rc_layer_init;
   enc->quality_params = radeon_enc_quality_params;
   enc->ctx = radeon_enc_ctx;
   enc->bitstream = radeon_enc_bitstream;
   enc->feedback = radeon_enc_feedback;
   enc->intra_refresh = radeon_enc_intra_refresh;
   enc->rc_per_pic = radeon_enc_rc_per_pic;
   enc->encode_params = radeon_enc_encode_params;
   enc->op_init = radeon_enc_op_init;
   enc->op_close = radeon_enc_op_close;
   enc->op_enc = radeon_enc_op_enc;
   enc->op_init_rc = radeon_enc_op_init_rc;
   enc->op_init_rc_vbv = radeon_enc_op_init_rc_vbv;
   enc->op_speed = radeon_enc_op_speed;

   if (u_reduce_video_profile(enc->base.profile) == PIPE_VIDEO_FORMAT_MPEG4_AVC) {
      enc->session_init = radeon_enc_session_init;
      enc->slice_control = radeon_enc_slice_control;
      enc->spec_misc = radeon_enc_spec_misc;
      enc->deblocking_filter = radeon_enc_deblocking_filter_h264;
      enc->nalu_sps = radeon_enc_nalu_sps;
      enc->nalu_pps = radeon_enc_nalu_pps;
      enc->slice_header = radeon_enc_slice_header;
      enc->encode_params = radeon_enc_encode_params;
      enc->encode_params_codec_spec = radeon_enc_encode_params_h264;
      enc->encode_headers = radeon_enc_headers_h264;
   } else if (u_reduce_video_profile(enc->base.profile) == PIPE_VIDEO_FORMAT_HEVC) {
      enc->session_init = radeon_enc_session_init_hevc;
      enc->slice_control = radeon_enc_slice_control_hevc;
      enc->spec_misc = radeon_enc_spec_misc_hevc;
      enc->deblocking_filter = radeon_enc_deblocking_filter_hevc;
      enc->nalu_sps = radeon_enc_nalu_sps_hevc;
      enc->nalu_pps = radeon_enc_nalu_pps_hevc;
      enc->nalu_vps = radeon_enc_nalu_vps;
      enc->nalu_aud = radeon_enc_nalu_aud_hevc;
      enc->slice_header = radeon_enc_slice_header_hevc;
      enc->encode_params = radeon_enc_encode_params_hevc;
      enc->encode_headers = radeon_enc_headers_hevc;
   }

   enc->cmd.session_info = RENCODE_IB_PARAM_SESSION_INFO;
   enc->cmd.task_info = RENCODE_IB_PARAM_TASK_INFO;
   enc->cmd.session_init = RENCODE_IB_PARAM_SESSION_INIT;
   enc->cmd.layer_control = RENCODE_IB_PARAM_LAYER_CONTROL;
   enc->cmd.layer_select = RENCODE_IB_PARAM_LAYER_SELECT;
   enc->cmd.rc_session_init = RENCODE_IB_PARAM_RATE_CONTROL_SESSION_INIT;
   enc->cmd.rc_layer_init = RENCODE_IB_PARAM_RATE_CONTROL_LAYER_INIT;
   enc->cmd.rc_per_pic = RENCODE_IB_PARAM_RATE_CONTROL_PER_PICTURE;
   enc->cmd.quality_params = RENCODE_IB_PARAM_QUALITY_PARAMS;
   enc->cmd.nalu = RENCODE_IB_PARAM_DIRECT_OUTPUT_NALU;
   enc->cmd.slice_header = RENCODE_IB_PARAM_SLICE_HEADER;
   enc->cmd.enc_params = RENCODE_IB_PARAM_ENCODE_PARAMS;
   enc->cmd.intra_refresh = RENCODE_IB_PARAM_INTRA_REFRESH;
   enc->cmd.ctx = RENCODE_IB_PARAM_ENCODE_CONTEXT_BUFFER;
   enc->cmd.bitstream = RENCODE_IB_PARAM_VIDEO_BITSTREAM_BUFFER;
   enc->cmd.feedback = RENCODE_IB_PARAM_FEEDBACK_BUFFER;
   enc->cmd.slice_control_hevc = RENCODE_HEVC_IB_PARAM_SLICE_CONTROL;
   enc->cmd.spec_misc_hevc = RENCODE_HEVC_IB_PARAM_SPEC_MISC;
   enc->cmd.deblocking_filter_hevc = RENCODE_HEVC_IB_PARAM_DEBLOCKING_FILTER;
   enc->cmd.slice_control_h264 = RENCODE_H264_IB_PARAM_SLICE_CONTROL;
   enc->cmd.spec_misc_h264 = RENCODE_H264_IB_PARAM_SPEC_MISC;
   enc->cmd.enc_params_h264 = RENCODE_H264_IB_PARAM_ENCODE_PARAMS;
   enc->cmd.deblocking_filter_h264 = RENCODE_H264_IB_PARAM_DEBLOCKING_FILTER;

   enc->enc_pic.session_info.interface_version =
      ((RENCODE_FW_INTERFACE_MAJOR_VERSION << RENCODE_IF_MAJOR_VERSION_SHIFT) |
       (RENCODE_FW_INTERFACE_MINOR_VERSION << RENCODE_IF_MINOR_VERSION_SHIFT));
}
