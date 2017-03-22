/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 * Copyright (c) 2015 Linaro Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <endian.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <linux/videodev2.h>
#include <linux/ion.h>
#include <linux/msm_ion.h>
#include <media/msm_vidc.h>

#include "common.h"

#define DBG_TAG "   vid"

#define CASE(ENUM) case ENUM: return #ENUM;

#define EXTRADATA_IDX(__num_planes) ((__num_planes) ? (__num_planes) - 1 : 0)

static const struct {
	uint32_t mask;
	const char *str;
} v4l2_buf_flags[] = {
	{ V4L2_BUF_FLAG_MAPPED, "MAPPED" },
	{ V4L2_BUF_FLAG_QUEUED, "QUEUED" },
	{ V4L2_BUF_FLAG_DONE, "DONE" },
	{ V4L2_BUF_FLAG_KEYFRAME, "KEYFRAME" },
	{ V4L2_BUF_FLAG_PFRAME, "PFRAME" },
	{ V4L2_BUF_FLAG_BFRAME, "BFRAME" },
	{ V4L2_BUF_FLAG_ERROR, "ERROR" },
	{ V4L2_BUF_FLAG_TIMECODE, "TIMECODE" },
	{ V4L2_BUF_FLAG_PREPARED, "PREPARED" },
	{ V4L2_BUF_FLAG_NO_CACHE_INVALIDATE, "NO_CACHE_INVALIDATE" },
	{ V4L2_BUF_FLAG_NO_CACHE_CLEAN, "NO_CACHE_CLEAN" },
	{ V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN, "TIMESTAMP_UNKNOWN" },
	{ V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC" },
	{ V4L2_BUF_FLAG_TIMESTAMP_COPY, "TIMESTAMP_COPY" },
	{ V4L2_BUF_FLAG_TSTAMP_SRC_EOF, "TSTAMP_SRC_EOF" },
	{ V4L2_BUF_FLAG_TSTAMP_SRC_SOE, "TSTAMP_SRC_SOE" },
	{ V4L2_QCOM_BUF_FLAG_CODECCONFIG, "QCOM_CODECCONFIG" },
	{ V4L2_QCOM_BUF_FLAG_EOSEQ, "QCOM_EOSEQ" },
	{ V4L2_QCOM_BUF_TIMESTAMP_INVALID, "QCOM_TIMESTAMP_INVALID" },
	{ V4L2_QCOM_BUF_FLAG_IDRFRAME, "QCOM_IDRFRAME" },
	{ V4L2_QCOM_BUF_FLAG_DECODEONLY, "QCOM_DECODEONLY" },
	{ V4L2_QCOM_BUF_DATA_CORRUPT, "QCOM_DATA_CORRUPT" },
	{ V4L2_QCOM_BUF_DROP_FRAME, "QCOM_DROP_FRAME" },
	{ V4L2_QCOM_BUF_INPUT_UNSUPPORTED, "QCOM_INPUT_UNSUPPORTED" },
	{ V4L2_QCOM_BUF_FLAG_EOS, "QCOM_EOS" },
	{ V4L2_QCOM_BUF_FLAG_READONLY, "QCOM_READONLY" },
	{ V4L2_MSM_VIDC_BUF_START_CODE_NOT_FOUND, "MSM_START_CODE_NOT_FOUND" },
	{ V4L2_MSM_BUF_FLAG_YUV_601_709_CLAMP, "MSM_YUV_601_709_CLAMP" },
	{ V4L2_MSM_BUF_FLAG_MBAFF, "MSM_MBAFF" },
	{ V4L2_MSM_BUF_FLAG_DEFER, "MSM_DEFER" },
};

static const char *buf_flags_to_string(uint32_t flags)
{
	static __thread char s[256];
	size_t n = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(v4l2_buf_flags); i++) {
		if (flags & v4l2_buf_flags[i].mask) {
			n += snprintf(s + n, sizeof (s) - n, "%s%s",
				      n > 0 ? "|" : "",
				      v4l2_buf_flags[i].str);
			if (n >= sizeof (s))
				break;
		}
	}

	s[MIN(n, sizeof (s) - 1)] = '\0';

	return s;
}

static const char *fourcc_to_string(uint32_t fourcc)
{
	static __thread char s[4];
	uint32_t fmt = htole32(fourcc);

	memcpy(s, &fmt, 4);

	return s;
}

static const char *buf_type_to_string(enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return "OUTPUT";
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return "CAPTURE";
	default:
		return "??";
	}
}

static const char *v4l2_field_to_string(enum v4l2_field field)
{
	switch (field) {
	CASE(V4L2_FIELD_ANY)
	CASE(V4L2_FIELD_NONE)
	CASE(V4L2_FIELD_TOP)
	CASE(V4L2_FIELD_BOTTOM)
	CASE(V4L2_FIELD_INTERLACED)
	CASE(V4L2_FIELD_SEQ_TB)
	CASE(V4L2_FIELD_SEQ_BT)
	CASE(V4L2_FIELD_ALTERNATE)
	CASE(V4L2_FIELD_INTERLACED_TB)
	CASE(V4L2_FIELD_INTERLACED_BT)
	default: return "unknown";
	}
}

static const char *v4l2_colorspace_to_string(enum v4l2_colorspace cspace)
{
	switch (cspace) {
	CASE(V4L2_COLORSPACE_SMPTE170M)
	CASE(V4L2_COLORSPACE_SMPTE240M)
	CASE(V4L2_COLORSPACE_REC709)
	CASE(V4L2_COLORSPACE_BT878)
	CASE(V4L2_COLORSPACE_470_SYSTEM_M)
	CASE(V4L2_COLORSPACE_470_SYSTEM_BG)
	CASE(V4L2_COLORSPACE_JPEG)
	CASE(V4L2_COLORSPACE_SRGB)
	default: return "unknown";
	}
}

static const char *extradata_type_to_string(int type)
{
	switch (type) {
	CASE(MSM_VIDC_EXTRADATA_NONE)
	CASE(MSM_VIDC_EXTRADATA_MB_QUANTIZATION)
	CASE(MSM_VIDC_EXTRADATA_INTERLACE_VIDEO)
	CASE(MSM_VIDC_EXTRADATA_VC1_FRAMEDISP)
	CASE(MSM_VIDC_EXTRADATA_VC1_SEQDISP)
	CASE(MSM_VIDC_EXTRADATA_TIMESTAMP)
	CASE(MSM_VIDC_EXTRADATA_S3D_FRAME_PACKING)
	CASE(MSM_VIDC_EXTRADATA_FRAME_RATE)
	CASE(MSM_VIDC_EXTRADATA_PANSCAN_WINDOW)
	CASE(MSM_VIDC_EXTRADATA_RECOVERY_POINT_SEI)
	CASE(MSM_VIDC_EXTRADATA_MPEG2_SEQDISP)
	CASE(MSM_VIDC_EXTRADATA_STREAM_USERDATA)
	CASE(MSM_VIDC_EXTRADATA_FRAME_QP)
	CASE(MSM_VIDC_EXTRADATA_FRAME_BITS_INFO)
	CASE(MSM_VIDC_EXTRADATA_VQZIP_SEI)
	CASE(MSM_VIDC_EXTRADATA_ROI_QP)
	CASE(MSM_VIDC_EXTRADATA_MASTERING_DISPLAY_COLOUR_SEI)
	CASE(MSM_VIDC_EXTRADATA_CONTENT_LIGHT_LEVEL_SEI)
	CASE(MSM_VIDC_EXTRADATA_PQ_INFO)
	CASE(MSM_VIDC_EXTRADATA_INPUT_CROP)
	CASE(MSM_VIDC_EXTRADATA_OUTPUT_CROP)
	CASE(MSM_VIDC_EXTRADATA_DIGITAL_ZOOM)
	CASE(MSM_VIDC_EXTRADATA_VPX_COLORSPACE_INFO)
	CASE(MSM_VIDC_EXTRADATA_MULTISLICE_INFO)
	CASE(MSM_VIDC_EXTRADATA_NUM_CONCEALED_MB)
	CASE(MSM_VIDC_EXTRADATA_INDEX)
	CASE(MSM_VIDC_EXTRADATA_ASPECT_RATIO)
	CASE(MSM_VIDC_EXTRADATA_METADATA_LTR)
	CASE(MSM_VIDC_EXTRADATA_METADATA_FILLER)
	CASE(MSM_VIDC_EXTRADATA_METADATA_MBI)
	CASE(MSM_VIDC_EXTRADATA_VUI_DISPLAY_INFO)
	CASE(MSM_VIDC_EXTRADATA_YUVSTATS_INFO)
	default: return "Unknown";
	}
}

static const char *extradata_interlace_format_to_string(enum msm_vidc_interlace_type format)
{
	switch (format) {
	CASE(MSM_VIDC_INTERLACE_FRAME_PROGRESSIVE)
	CASE(MSM_VIDC_INTERLACE_INTERLEAVE_FRAME_TOPFIELDFIRST)
	CASE(MSM_VIDC_INTERLACE_INTERLEAVE_FRAME_BOTTOMFIELDFIRST)
	CASE(MSM_VIDC_INTERLACE_FRAME_TOPFIELDFIRST)
	CASE(MSM_VIDC_INTERLACE_FRAME_BOTTOMFIELDFIRST)
	default : return "Unknown";
	}
}

static const char *extradata_interlace_color_format_to_string(unsigned int format)
{
	switch (format) {
	CASE(MSM_VIDC_HAL_INTERLACE_COLOR_FORMAT_NV12)
	CASE(MSM_VIDC_HAL_INTERLACE_COLOR_FORMAT_NV12_UBWC)
	default: return "Unknown";
	}
}

#undef CASE

static void list_formats(struct instance *i, enum v4l2_buf_type type)
{
	struct v4l2_fmtdesc fdesc;
	struct v4l2_frmsizeenum frmsize;

	dbg("%s formats:", buf_type_to_string(type));

	memzero(fdesc);
	fdesc.type = type;

	while (!ioctl(i->video.fd, VIDIOC_ENUM_FMT, &fdesc)) {
		dbg("  %s", fdesc.description);

		memzero(frmsize);
		frmsize.pixel_format = fdesc.pixelformat;

		while (!ioctl(i->video.fd, VIDIOC_ENUM_FRAMESIZES, &frmsize)) {
			switch (frmsize.type) {
			case V4L2_FRMSIZE_TYPE_DISCRETE:
				dbg("    %dx%d",
				    frmsize.discrete.width,
				    frmsize.discrete.height);
				break;
			case V4L2_FRMSIZE_TYPE_STEPWISE:
			case V4L2_FRMSIZE_TYPE_CONTINUOUS:
				dbg("    %dx%d to %dx%d, step %+d%+d",
				    frmsize.stepwise.min_width,
				    frmsize.stepwise.min_height,
				    frmsize.stepwise.max_width,
				    frmsize.stepwise.max_height,
				    frmsize.stepwise.step_width,
				    frmsize.stepwise.step_height);
				break;
			}

			if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
				break;

			frmsize.index++;
		}

		fdesc.index++;
	}
}

int video_open(struct instance *i, char *name)
{
	struct v4l2_capability cap;

	i->video.fd = open(name, O_RDWR, 0);
	if (i->video.fd < 0) {
		err("Failed to open video decoder: %s", name);
		return -1;
	}

	memzero(cap);
	if (ioctl(i->video.fd, VIDIOC_QUERYCAP, &cap) < 0) {
		err("Failed to verify capabilities: %m");
		return -1;
	}

	dbg("caps (%s): driver=\"%s\" bus_info=\"%s\" card=\"%s\" "
	    "version=%u.%u.%u", name, cap.driver, cap.bus_info, cap.card,
	    (cap.version >> 16) & 0xff,
	    (cap.version >> 8) & 0xff,
	    cap.version & 0xff);

	dbg("  [%c] V4L2_CAP_VIDEO_CAPTURE",
	    cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_CAPTURE_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_M2M",
	    cap.capabilities & V4L2_CAP_VIDEO_M2M ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_M2M_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OVERLAY",
	    cap.capabilities & V4L2_CAP_VIDEO_OVERLAY ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VBI_CAPTURE",
	    cap.capabilities & V4L2_CAP_VBI_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VBI_OUTPUT",
	    cap.capabilities & V4L2_CAP_VBI_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SLICED_VBI_CAPTURE",
	    cap.capabilities & V4L2_CAP_SLICED_VBI_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SLICED_VBI_OUTPUT",
	    cap.capabilities & V4L2_CAP_SLICED_VBI_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RDS_CAPTURE",
	    cap.capabilities & V4L2_CAP_RDS_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT_OVERLAY",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_OVERLAY ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_HW_FREQ_SEEK",
	    cap.capabilities & V4L2_CAP_HW_FREQ_SEEK ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RDS_OUTPUT",
	    cap.capabilities & V4L2_CAP_RDS_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_TUNER",
	    cap.capabilities & V4L2_CAP_TUNER ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_AUDIO",
	    cap.capabilities & V4L2_CAP_AUDIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RADIO",
	    cap.capabilities & V4L2_CAP_RADIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_MODULATOR",
	    cap.capabilities & V4L2_CAP_MODULATOR ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SDR_CAPTURE",
	    cap.capabilities & V4L2_CAP_SDR_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_EXT_PIX_FORMAT",
	    cap.capabilities & V4L2_CAP_EXT_PIX_FORMAT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_READWRITE",
	    cap.capabilities & V4L2_CAP_READWRITE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_ASYNCIO",
	    cap.capabilities & V4L2_CAP_ASYNCIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_STREAMING",
	    cap.capabilities & V4L2_CAP_STREAMING ? '*' : ' ');

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ||
	    !(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE) ||
	    !(cap.capabilities & V4L2_CAP_STREAMING)) {
		err("Insufficient capabilities for video device (is %s correct?)",
		    name);
		return -1;
	}

	list_formats(i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	list_formats(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

        return 0;
}

void video_close(struct instance *i)
{
	close(i->video.fd);
}

int video_set_secure(struct instance *i)
{
	struct v4l2_control control = {0};

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_SECURE;
	control.value = 1;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to set secure mode: %m");
		return -1;
	}

	return 0;
}

int video_set_control(struct instance *i)
{
	struct v4l2_control control = {0};

	if (i->decode_order) {
		control.id = V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER;
		control.value = V4L2_MPEG_VIDC_VIDEO_OUTPUT_ORDER_DECODE;

		if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
			err("failed to set output order: %m");
			return -1;
		}
	}

	if (i->skip_frames) {
		control.id = V4L2_CID_MPEG_VIDC_VIDEO_PICTYPE_DEC_MODE;
		control.value = V4L2_MPEG_VIDC_VIDEO_PICTYPE_DECODE_ON;

		if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
			err("failed to set skip mode: %m");
			return -1;
		}
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONTINUE_DATA_TRANSFER;
	control.value = i->continue_data_transfer;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to set data transfer mode: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_SET_PERF_LEVEL;
	control.value = V4L2_CID_MPEG_VIDC_PERF_LEVEL_TURBO;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to set perf level: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONCEAL_COLOR;
	control.value = 0x00ff;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to set conceal color: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_INTERLACE_VIDEO;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to enable interlace extradata: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_OUTPUT_CROP;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to enable output crop extradata: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_ASPECT_RATIO;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to enable aspect ratio extradata: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_FRAME_RATE;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to enable framerate extradata: %m");
		return -1;
	}

#if 0
	/* FIXME : Use this when QCOM has fixed the bug */
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_DISPLAY_COLOUR_SEI;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to enable display colour sei extradata: %m");
		return -1;
	}
#endif

	return 0;
}

int video_set_dpb(struct instance *i,
		  enum v4l2_mpeg_vidc_video_dpb_color_format format)
{
	struct v4l2_ext_control control[2] = {0};
	struct v4l2_ext_controls controls = {0};

	control[0].id = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE;
	control[0].value = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_PRIMARY;

	control[1].id = V4L2_CID_MPEG_VIDC_VIDEO_DPB_COLOR_FORMAT;
	control[1].value = format;

	controls.count = 2;
	controls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
	controls.controls = control;

	if (ioctl(i->video.fd, VIDIOC_S_EXT_CTRLS, &controls) < 0) {
		err("failed to set dpb format: %m");
		return -1;
	}

	return 0;
}

int video_set_framerate(struct instance *i, int num, int den)
{
	struct v4l2_streamparm parm;

	dbg("set framerate to %.3f", (float)num / (float)den);

	memzero(parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	parm.parm.output.timeperframe.numerator = den;
	parm.parm.output.timeperframe.denominator = num;

	if (ioctl(i->video.fd, VIDIOC_S_PARM, &parm) < 0) {
		err("Failed to set framerate on OUTPUT: %m");
		return -1;
	}

	return 0;
}

static void video_handle_extradata_payload(struct instance *i,
					   enum msm_vidc_extradata_type type,
					   void *data, int size, struct fb *fb)
{
	switch (type) {
	case MSM_VIDC_EXTRADATA_OUTPUT_CROP: {
		struct msm_vidc_output_crop_payload *payload = data;

		if (size != sizeof (*payload)) {
			dbg("extradata: Invalid data size for %s",
			    extradata_type_to_string(type));
			return;
		}

		dbg("extradata: %s top=%u left=%u "
		    "display_width=%u display_height=%u width=%u height=%u",
		    extradata_type_to_string(type),
		    payload->top, payload->left,
		    payload->display_width, payload->display_height,
		    payload->width, payload->height);

		fb->crop_x = payload->left;
		fb->crop_y = payload->top;
		fb->crop_w = payload->display_width;
		fb->crop_h = payload->display_height;
		break;
	}

	case MSM_VIDC_EXTRADATA_ASPECT_RATIO: {
		struct msm_vidc_aspect_ratio_payload *payload = data;

		if (size != sizeof (*payload)) {
			dbg("extradata: Invalid data size for %s",
			    extradata_type_to_string(type));
			return;
		}

		dbg("extradata: %s aspect_width=%u aspect_height=%u",
		    extradata_type_to_string(type),
		    payload->aspect_width, payload->aspect_height);

		fb->ar_x = payload->aspect_width;
		fb->ar_y = payload->aspect_height;
		break;
	}

	case MSM_VIDC_EXTRADATA_INTERLACE_VIDEO: {
		struct msm_vidc_interlace_payload *payload = data;

		if (size != sizeof (*payload)) {
			dbg("extradata: Invalid data size for %s",
			    extradata_type_to_string(type));
			return;
		}

		dbg("extradata: %s format=%s color_format=%s",
		    extradata_type_to_string(type),
		    extradata_interlace_format_to_string(payload->format),
		    extradata_interlace_color_format_to_string(payload->color_format));

		break;
	}

	case MSM_VIDC_EXTRADATA_MASTERING_DISPLAY_COLOUR_SEI: {
		struct msm_vidc_mastering_display_colour_sei_payload *payload = data;

		if (size != sizeof (*payload)) {
			dbg("extradata: Invalid data size for %s",
			    extradata_type_to_string(type));
			return;
		}

		dbg("extradata: %s nDisplayPrimariesX={%u, %u, %u} "
		    "nDisplayPrimariesY={%u, %u, %u} nWhitePointX=%u "
		    "nWhitePointY=%u nMaxDisplayMasteringLuminance=%u "
		    "nMinDisplayMasteringLuminance=%u",
		    extradata_type_to_string(type), payload->nDisplayPrimariesX[0],
		    payload->nDisplayPrimariesX[1], payload->nDisplayPrimariesX[2],
		    payload->nDisplayPrimariesY[0], payload->nDisplayPrimariesY[1],
		    payload->nDisplayPrimariesY[2], payload->nWhitePointX,
		    payload->nWhitePointY, payload->nMaxDisplayMasteringLuminance,
		    payload->nMinDisplayMasteringLuminance);

		break;
	}

	case MSM_VIDC_EXTRADATA_FRAME_RATE: {
		struct msm_vidc_framerate_payload *payload = data;

		if (size != sizeof (*payload)) {
			dbg("extradata: Invalid data size for %s",
			    extradata_type_to_string(type));
			return;
		}

		int framerate_num = payload->frame_rate;
		int framerate_den = 0x10000;

		dbg("extradata: %s frame_rate=%.3f",
		    extradata_type_to_string(type),
		    (float)framerate_num / (float)framerate_den);

		break;
	}

	default:
		dbg("extradata: unhandled extradata index %s (0x%02x)",
		    extradata_type_to_string(type), type);
		break;
	}
}

void video_handle_extradata(struct instance *i,
			    struct msm_vidc_extradata_header *hdr,
			    struct fb *fb)
{
	uint32_t left = i->video.extradata_size;

	if (!hdr)
		return;

	while (left >= sizeof (*hdr) && left >= hdr->size &&
	       hdr->type != MSM_VIDC_EXTRADATA_NONE) {
		if (hdr->type == MSM_VIDC_EXTRADATA_INDEX) {
			struct msm_vidc_extradata_index *payload =
				(void *)hdr->data;

			video_handle_extradata_payload(i, payload->type,
						       ((uint8_t *)hdr->data) + sizeof (payload->type),
						       hdr->data_size - sizeof (payload->type), fb);
		} else {
			video_handle_extradata_payload(i, hdr->type,
						       hdr->data,
						       hdr->data_size, fb);
		}

		left -= hdr->size;
		hdr = (void *)((uint8_t*)hdr + hdr->size);
	}
}

static int video_count_capture_queued_bufs(struct video *vid)
{
	int cap_queued = 0;

	for (int idx = 0; idx < vid->cap_buf_cnt; idx++) {
		if (vid->cap_buf_flag[idx])
			cap_queued++;
	}

	return cap_queued;
}

static int video_count_output_queued_bufs(struct video *vid)
{
	int out_queued = 0;

	for (int idx = 0; idx < vid->out_buf_cnt; idx++) {
		if (vid->out_buf_flag[idx])
			out_queued++;
	}

	return out_queued;
}

int video_queue_buf_out(struct instance *i, int n, int length,
			uint32_t flags, struct timeval timestamp)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[1];

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	if (n >= vid->out_buf_cnt) {
		err("tried to queue a non existing %s buffer",
		    buf_type_to_string(type));
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = type;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.index = n;
	buf.length = 1;
	buf.m.planes = planes;

	buf.m.planes[0].m.userptr = (unsigned long)vid->out_ion_addr;
	buf.m.planes[0].reserved[0] = vid->out_ion_fd;
	buf.m.planes[0].reserved[1] = vid->out_buf_off[n];
	buf.m.planes[0].length = vid->out_buf_size;
	buf.m.planes[0].bytesused = length;
	buf.m.planes[0].data_offset = 0;

	buf.flags = flags;
	buf.timestamp = timestamp;

	if (ioctl(vid->fd, VIDIOC_QBUF, &buf) < 0) {
		err("failed to queue %s buffer (index=%d): %m",
		    buf_type_to_string(buf.type), buf.index);
		return -1;
	}

	dbg("%s: queued buffer %d (flags:%08x:%s, bytesused:%d, "
	    "ts: %ld.%06lu), %d/%d queued", buf_type_to_string(buf.type),
	    buf.index, buf.flags, buf_flags_to_string(buf.flags),
	    buf.m.planes[0].bytesused,
	    buf.timestamp.tv_sec, buf.timestamp.tv_usec,
	    video_count_output_queued_bufs(vid), vid->out_buf_cnt);

	return 0;
}

int video_queue_buf_cap(struct instance *i, int n)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (n >= vid->cap_buf_cnt) {
		err("tried to queue a non existing %s buffer",
		    buf_type_to_string(type));
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = type;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.index = n;
	buf.length = 2;
	buf.m.planes = planes;

	buf.m.planes[0].m.userptr = i->secure ?
		(unsigned long)vid->cap_buf_fd[n] :
		(unsigned long)vid->cap_buf_addr[n];
	buf.m.planes[0].reserved[0] = vid->cap_buf_fd[n];
	buf.m.planes[0].reserved[1] = 0;
	buf.m.planes[0].length = vid->cap_buf_size;
	buf.m.planes[0].bytesused = vid->cap_buf_size;
	buf.m.planes[0].data_offset = 0;

	if (vid->extradata_index) { // Should be 1
		buf.m.planes[vid->extradata_index].m.userptr = (unsigned long)vid->extradata_ion_addr;
		buf.m.planes[vid->extradata_index].reserved[0] = vid->extradata_ion_fd;
		buf.m.planes[vid->extradata_index].reserved[1] = vid->extradata_off[n];
		buf.m.planes[vid->extradata_index].length = vid->extradata_size;
		buf.m.planes[vid->extradata_index].bytesused = 0;
		buf.m.planes[vid->extradata_index].data_offset = 0;
	}

	if (ioctl(vid->fd, VIDIOC_QBUF, &buf) < 0) {
		err("failed to queue %s buffer (index=%d): %m",
		    buf_type_to_string(buf.type), buf.index);
		return -1;
	}

	vid->cap_buf_flag[n] = 1;

	dbg("%s: queued buffer %d, %d/%d queued", buf_type_to_string(buf.type),
	    buf.index, video_count_capture_queued_bufs(vid), vid->cap_buf_cnt);

	return 0;
}

static int video_dequeue_buf(struct instance *i, struct v4l2_buffer *buf)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, VIDIOC_DQBUF, buf);
	if (ret < 0) {
		err("failed to dequeue buffer on %s queue: %m",
		    buf_type_to_string(buf->type));
		return -errno;
	}

	switch (buf->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		dbg("%s: dequeued buffer %d, %d/%d queued",
		    buf_type_to_string(buf->type), buf->index,
		    video_count_output_queued_bufs(vid), vid->out_buf_cnt);
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		vid->cap_buf_flag[buf->index] = 0;
		dbg("%s: dequeued buffer %d (flags:%08x:%s, bytesused:%d, "
		    "ts: %ld.%06lu), %d/%d queued",
		    buf_type_to_string(buf->type),
		    buf->index, buf->flags, buf_flags_to_string(buf->flags),
		    buf->m.planes[0].bytesused,
		    buf->timestamp.tv_sec, buf->timestamp.tv_usec,
		    video_count_capture_queued_bufs(vid), vid->cap_buf_cnt);
		break;
	}

	return 0;
}

int video_dequeue_output(struct instance *i, int *n)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[OUT_PLANES];
	int ret;

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.m.planes = planes;
	buf.length = OUT_PLANES;

	ret = video_dequeue_buf(i, &buf);
	if (ret < 0)
		return ret;

	*n = buf.index;

	return 0;
}

int video_dequeue_capture(struct instance *i, int *n, unsigned int *bytesused,
			  uint32_t *flags, struct timeval *ts,
			  struct msm_vidc_extradata_header **extradata)
{
	struct video *vid = &i->video;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[CAP_PLANES];

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.m.planes = planes;
	buf.length = CAP_PLANES;

	if (video_dequeue_buf(i, &buf))
		return -1;

	*bytesused = buf.m.planes[0].bytesused;
	*n = buf.index;

	if (flags)
		*flags = buf.flags;
	if (ts)
		*ts = buf.timestamp;

	if (extradata) {
		if (vid->extradata_index >= 0)
			*extradata = vid->extradata_addr[buf.index];
		else
			*extradata = NULL;
	}

	return 0;
}

int video_stream(struct instance *i, enum v4l2_buf_type type, int status)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, status, &type);
	if (ret) {
		err("failed to stream on %s queue (status=%d)",
		    buf_type_to_string(type), status);
		return -1;
	}

	dbg("%s: stream %s", buf_type_to_string(type),
	    status == VIDIOC_STREAMON ? "ON" :
	    status == VIDIOC_STREAMOFF ? "OFF" : "??");

	return 0;
}

int video_flush(struct instance *i, uint32_t flags)
{
	struct video *vid = &i->video;
	struct v4l2_decoder_cmd dec;

	if (flags & V4L2_QCOM_CMD_FLUSH_CAPTURE)
		dbg("flushing CAPTURE queue");

	if (flags & V4L2_QCOM_CMD_FLUSH_OUTPUT)
		dbg("flushing OUTPUT queue");

	memzero(dec);
	dec.flags = flags;
	dec.cmd = V4L2_DEC_QCOM_CMD_FLUSH;
	if (ioctl(vid->fd, VIDIOC_DECODER_CMD, &dec) < 0) {
		err("failed to flush: %m");
		return -1;
	}

	return 0;
}

static int
alloc_ion_buffer(struct instance *i, size_t size, uint32_t flags)
{
	struct ion_allocation_data ion_alloc = { 0 };
	struct ion_fd_data ion_fd_data = { 0 };
	struct ion_handle_data ion_handle_data = { 0 };
	static int ion_fd = -1;
	int ret;

	if (ion_fd < 0) {
		ion_fd = open("/dev/ion", O_RDONLY);
		if (ion_fd < 0) {
			err("Cannot open ion device: %m");
			return -1;
		}
	}

	ion_alloc.handle = -1;
	ion_alloc.len = size;
	ion_alloc.align = 4096;
	ion_alloc.flags = flags;

	if (flags & ION_SECURE)
		ion_alloc.heap_id_mask = ION_HEAP(ION_SECURE_HEAP_ID);
	else
		ion_alloc.heap_id_mask = ION_HEAP(ION_IOMMU_HEAP_ID);

	if (flags & ION_FLAG_CP_BITSTREAM)
		ion_alloc.heap_id_mask |= ION_HEAP(ION_SECURE_DISPLAY_HEAP_ID);

	if (ioctl(ion_fd, ION_IOC_ALLOC, &ion_alloc) < 0) {
		err("Failed to allocate ion buffer: %m");
		return -1;
	}

	dbg("Allocated %zd bytes ION buffer %d",
	    ion_alloc.len, ion_alloc.handle);

	ion_fd_data.handle = ion_alloc.handle;
	ion_fd_data.fd = -1;

	if (ioctl(ion_fd, ION_IOC_MAP, &ion_fd_data) < 0) {
		err("Failed to map ion buffer: %m");
		ret = -1;
	} else {
		ret = ion_fd_data.fd;
	}

	ion_handle_data.handle = ion_alloc.handle;
	if (ioctl(ion_fd, ION_IOC_FREE, &ion_handle_data) < 0)
		err("Failed to free ion buffer: %m");

	return ret;
}

static int setup_extradata(struct instance *i, int index, int size)
{
	struct video *vid = &i->video;
	int off = 0;

	vid->extradata_index = index;
	vid->extradata_size = size;

	if (vid->extradata_ion_fd < 0) {
		vid->extradata_ion_fd = alloc_ion_buffer(i, size * MAX_CAP_BUF, 0);
		vid->extradata_ion_addr = mmap(NULL,
					       size * MAX_CAP_BUF,
					       PROT_READ|PROT_WRITE,
					       MAP_SHARED,
					       vid->extradata_ion_fd,
					       0);

		for (int i = 0; i < MAX_CAP_BUF; i++) {
			vid->extradata_off[i] = off;
			vid->extradata_addr[i] = vid->extradata_ion_addr + off;
			off += size;
		}
	}

	return 0;
}

int video_setup_capture(struct instance *i, int num_buffers, int w, int h)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_format fmt;
	struct v4l2_pix_format_mplane *pix;
	struct v4l2_requestbuffers reqbuf;
	int ion_fd;
	uint32_t ion_flags;
	void *buf_addr;
	int n, extra_idx;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	video_set_dpb(i, i->depth == 10 ?
		      V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_TP10_UBWC :
		      V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_NONE);

	memzero(fmt);
	fmt.type = type;
	pix = &fmt.fmt.pix_mp;
	pix->height = h;
	pix->width = w;

	if (i->depth == 10)
		pix->pixelformat = V4L2_PIX_FMT_NV12_TP10_UBWC;
	else if (!i->interlaced)
		pix->pixelformat = V4L2_PIX_FMT_NV12_UBWC;
	else
		pix->pixelformat = V4L2_PIX_FMT_NV12;

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("failed to set %s format (%dx%d)",
		    buf_type_to_string(fmt.type), w, h);
		return -1;
	}

	memzero(reqbuf);
	reqbuf.count = num_buffers;
	reqbuf.type = type;
	reqbuf.memory = V4L2_MEMORY_USERPTR;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("failed to request %s buffers: %m",
		    buf_type_to_string(type));
		return -1;
	}

	dbg("%s: requested %d buffers, got %d", buf_type_to_string(type),
	    num_buffers, reqbuf.count);

	vid->cap_buf_cnt = reqbuf.count;

	if (ioctl(vid->fd, VIDIOC_G_FMT, &fmt) < 0) {
		err("failed to get %s format", buf_type_to_string(type));
		return -1;
	}

	dbg("  %dx%d fmt=%s (%d planes) field=%s cspace=%s flags=%08x",
	    pix->width, pix->height, fourcc_to_string(pix->pixelformat),
	    pix->num_planes, v4l2_field_to_string(pix->field),
	    v4l2_colorspace_to_string(pix->colorspace), pix->flags);

	for (n = 0; n < pix->num_planes; n++)
		dbg("    plane %d: size=%d stride=%d scanlines=%d", n,
		    pix->plane_fmt[n].sizeimage,
		    pix->plane_fmt[n].bytesperline,
		    pix->plane_fmt[n].reserved[0]);

	vid->cap_buf_format = pix->pixelformat;
	vid->cap_w = pix->width;
	vid->cap_h = pix->height;

	/* MSM V4L2 driver stores video data in the first plane and extra
	 * metadata in the second plane. */
	vid->cap_buf_size = pix->plane_fmt[0].sizeimage;

	switch (vid->cap_buf_format) {
	case V4L2_PIX_FMT_NV12:
		vid->cap_planes_count = 2;
		/* Y plane */
		vid->cap_plane_off[0] = 0;
		vid->cap_plane_stride[0] = pix->plane_fmt[0].bytesperline;
		/* UV plane */
		vid->cap_plane_off[1] = pix->plane_fmt[0].reserved[0] *
			pix->plane_fmt[0].bytesperline;
		vid->cap_plane_stride[1] = pix->plane_fmt[0].bytesperline;
		break;
	default:
		/* the driver does not provide enough information to compute
		 * the plane offsets for some formats like UBWC, so just use
		 * a single plane. */
		vid->cap_planes_count = 1;
		vid->cap_plane_off[0] = 0;
		vid->cap_plane_stride[0] = pix->plane_fmt[0].bytesperline;
		break;
	}

	if (i->secure)
		ion_flags = ION_FLAG_SECURE | ION_FLAG_CP_PIXEL;
	else
		ion_flags = 0;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		ion_fd = alloc_ion_buffer(i, vid->cap_buf_size, ion_flags);
		if (ion_fd < 0)
			return -1;

		if (!i->secure) {
			buf_addr = mmap(NULL, vid->cap_buf_size, PROT_READ,
					MAP_SHARED, ion_fd, 0);
			if (buf_addr == MAP_FAILED) {
				err("failed to map %s buffer: %m",
				    buf_type_to_string(type));
				close(ion_fd);
				return -1;
			}
		} else {
			buf_addr = NULL;
		}

		vid->cap_buf_fd[n] = ion_fd;
		vid->cap_buf_addr[n] = buf_addr;
	}

	dbg("%s: succesfully mmapped %d buffers", buf_type_to_string(type),
	    vid->cap_buf_cnt);

	extra_idx = EXTRADATA_IDX(pix->num_planes);
	if (extra_idx && (extra_idx < VIDEO_MAX_PLANES)) {
		dbg("%s: extradata plane is %d (size=%d)",
		    buf_type_to_string(type), extra_idx,
		    pix->plane_fmt[extra_idx].sizeimage);
		setup_extradata(i, extra_idx,
				pix->plane_fmt[extra_idx].sizeimage);
	}

	return 0;
}

int video_stop_capture(struct instance *i)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_requestbuffers reqbuf;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (video_stream(i, type, VIDIOC_STREAMOFF))
		return -1;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_USERPTR;
	reqbuf.type = type;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on %s queue failed: %m",
		    buf_type_to_string(type));
		return -1;
	}

	for (int n = 0; n < vid->cap_buf_cnt; n++) {
		if (munmap(vid->cap_buf_addr[n], vid->cap_buf_size))
			err("failed to unmap %s buffer: %m",
			    buf_type_to_string(type));

		if (close(vid->cap_buf_fd[n]) < 0)
			err("failed to close %s ion buffer: %m",
			    buf_type_to_string(type));

		vid->cap_buf_fd[n] = -1;
		vid->cap_buf_addr[n] = NULL;
		vid->cap_buf_flag[n] = 0;
	}

	vid->cap_planes_count = 0;
	vid->cap_buf_size = 0;
	vid->cap_buf_cnt = 0;

	return 0;
}

int video_setup_output(struct instance *i, unsigned long codec,
		       unsigned int size, int count)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_format fmt;
	struct v4l2_pix_format_mplane *pix;
	struct v4l2_requestbuffers reqbuf;
	int ion_fd;
	int ion_size;
	void *buf_addr;
	int n;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	memzero(fmt);
	fmt.type = type;
	pix = &fmt.fmt.pix_mp;
	pix->width = i->width;
	pix->height = i->height;
	pix->pixelformat = codec;

	video_set_framerate(i, i->fps_n, i->fps_d);

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("failed to set %s format: %m", buf_type_to_string(type));
		return -1;
	}

	dbg("%s: setup buffer size=%u (requested=%u)", buf_type_to_string(type),
	    pix->plane_fmt[0].sizeimage, size);

	vid->out_buf_size = pix->plane_fmt[0].sizeimage;

	memzero(reqbuf);
	reqbuf.count = count;
	reqbuf.type = type;
	reqbuf.memory = V4L2_MEMORY_USERPTR;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("failed to request %s buffers: %m",
		    buf_type_to_string(type));
		return -1;
	}

	vid->out_buf_cnt = reqbuf.count;

	dbg("%s: requested %d buffers, got %d", buf_type_to_string(type),
	    count, reqbuf.count);

	ion_size = vid->out_buf_cnt * vid->out_buf_size;
	ion_fd = alloc_ion_buffer(i, ion_size, 0);
	if (ion_fd < 0)
		return -1;

	buf_addr = mmap(NULL, ion_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			ion_fd, 0);
	if (buf_addr == MAP_FAILED) {
		err("failed to map %s buffer: %m", buf_type_to_string(type));
		return -1;
	}

	vid->out_ion_fd = ion_fd;
	vid->out_ion_size = ion_size;
	vid->out_ion_addr = buf_addr;

	for (n = 0; n < vid->out_buf_cnt; n++) {
		vid->out_buf_off[n] = n * vid->out_buf_size;
		vid->out_buf_addr[n] = buf_addr + vid->out_buf_off[n];
		vid->out_buf_flag[n] = 0;
	}

	dbg("%s: succesfully mmapped %d buffers", buf_type_to_string(type),
	    vid->out_buf_cnt);

	return 0;
}

int video_stop_output(struct instance *i)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_requestbuffers reqbuf;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	if (video_stream(i, type, VIDIOC_STREAMOFF))
		return -1;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_USERPTR;
	reqbuf.type = type;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on %s queue failed: %m",
		    buf_type_to_string(type));
		return -1;
	}

	if (vid->out_ion_addr) {
		if (munmap(vid->out_ion_addr, vid->out_ion_size))
			err("failed to unmap %s buffer: %m",
			    buf_type_to_string(type));
	}

	if (vid->out_ion_fd >= 0) {
		if (close(vid->out_ion_fd) < 0)
			err("failed to close %s ion buffer: %m",
			    buf_type_to_string(type));
	}

	for (int n = 0; n < vid->out_buf_cnt; n++) {
		vid->out_buf_flag[n] = 0;
		vid->out_buf_off[n] = 0;
		vid->out_buf_addr[n] = NULL;
	}

	vid->out_ion_fd = -1;
	vid->out_ion_size = 0;
	vid->out_ion_addr = NULL;
	vid->out_buf_cnt = 0;

	return 0;
}

int video_subscribe_event(struct instance *i, int event_type)
{
	struct v4l2_event_subscription sub;

	memset(&sub, 0, sizeof(sub));
	sub.type = event_type;

	if (ioctl(i->video.fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		err("failed to subscribe to event type %u: %m", sub.type);
		return -1;
	}

	return 0;
}

int video_dequeue_event(struct instance *i, struct v4l2_event *ev)
{
	struct video *vid = &i->video;

	memset(ev, 0, sizeof (*ev));

	if (ioctl(vid->fd, VIDIOC_DQEVENT, ev) < 0) {
		err("failed to dequeue event: %m");
		return -1;
	}

	return 0;
}
