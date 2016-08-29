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
#include <media/msm_media_info.h>

#include "common.h"

#define DBG_TAG "   vid"

static char *dbg_type[2] = {"OUTPUT", "CAPTURE"};
static char *dbg_status[2] = {"ON", "OFF"};

#define CASE(ENUM) case ENUM: return #ENUM;

static const char *fourcc_to_string(uint32_t fourcc)
{
	static __thread char s[4];
	uint32_t fmt = htole32(fourcc);

	memcpy(s, &fmt, 4);

	return s;
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

#undef CASE

static void list_formats(struct instance *i, enum v4l2_buf_type type)
{
	struct v4l2_fmtdesc fdesc;
	struct v4l2_frmsizeenum frmsize;

	info("%s formats:",
	     dbg_type[type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE]);

	memzero(fdesc);
	fdesc.type = type;

	while (!ioctl(i->video.fd, VIDIOC_ENUM_FMT, &fdesc)) {
		info("  %s", fdesc.description);

		memzero(frmsize);
		frmsize.pixel_format = fdesc.pixelformat;

		while (!ioctl(i->video.fd, VIDIOC_ENUM_FRAMESIZES, &frmsize)) {
			switch (frmsize.type) {
			case V4L2_FRMSIZE_TYPE_DISCRETE:
				info("    %dx%d",
				     frmsize.discrete.width,
				     frmsize.discrete.height);
				break;
			case V4L2_FRMSIZE_TYPE_STEPWISE:
			case V4L2_FRMSIZE_TYPE_CONTINUOUS:
				info("    %dx%d to %dx%d, step %+d%+d",
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

	info("caps (%s): driver=\"%s\" bus_info=\"%s\" card=\"%s\" "
	     "version=%u.%u.%u", name, cap.driver, cap.bus_info, cap.card,
	     (cap.version >> 16) & 0xff,
	     (cap.version >> 8) & 0xff,
	     cap.version & 0xff);

	info("  [%c] V4L2_CAP_VIDEO_CAPTURE",
	     cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ? '*' : ' ');
	info("  [%c] V4L2_CAP_VIDEO_CAPTURE_MPLANE",
	     cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? '*' : ' ');
	info("  [%c] V4L2_CAP_VIDEO_OUTPUT",
	     cap.capabilities & V4L2_CAP_VIDEO_OUTPUT ? '*' : ' ');
	info("  [%c] V4L2_CAP_VIDEO_OUTPUT_MPLANE",
	     cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE ? '*' : ' ');
	info("  [%c] V4L2_CAP_VIDEO_M2M",
	     cap.capabilities & V4L2_CAP_VIDEO_M2M ? '*' : ' ');
	info("  [%c] V4L2_CAP_VIDEO_M2M_MPLANE",
	     cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ? '*' : ' ');
	info("  [%c] V4L2_CAP_VIDEO_OVERLAY",
	     cap.capabilities & V4L2_CAP_VIDEO_OVERLAY ? '*' : ' ');
	info("  [%c] V4L2_CAP_VBI_CAPTURE",
	     cap.capabilities & V4L2_CAP_VBI_CAPTURE ? '*' : ' ');
	info("  [%c] V4L2_CAP_VBI_OUTPUT",
	     cap.capabilities & V4L2_CAP_VBI_OUTPUT ? '*' : ' ');
	info("  [%c] V4L2_CAP_SLICED_VBI_CAPTURE",
	     cap.capabilities & V4L2_CAP_SLICED_VBI_CAPTURE ? '*' : ' ');
	info("  [%c] V4L2_CAP_SLICED_VBI_OUTPUT",
	     cap.capabilities & V4L2_CAP_SLICED_VBI_OUTPUT ? '*' : ' ');
	info("  [%c] V4L2_CAP_RDS_CAPTURE",
	     cap.capabilities & V4L2_CAP_RDS_CAPTURE ? '*' : ' ');
	info("  [%c] V4L2_CAP_VIDEO_OUTPUT_OVERLAY",
	     cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_OVERLAY ? '*' : ' ');
	info("  [%c] V4L2_CAP_HW_FREQ_SEEK",
	     cap.capabilities & V4L2_CAP_HW_FREQ_SEEK ? '*' : ' ');
	info("  [%c] V4L2_CAP_RDS_OUTPUT",
	     cap.capabilities & V4L2_CAP_RDS_OUTPUT ? '*' : ' ');
	info("  [%c] V4L2_CAP_TUNER",
	     cap.capabilities & V4L2_CAP_TUNER ? '*' : ' ');
	info("  [%c] V4L2_CAP_AUDIO",
	     cap.capabilities & V4L2_CAP_AUDIO ? '*' : ' ');
	info("  [%c] V4L2_CAP_RADIO",
	     cap.capabilities & V4L2_CAP_RADIO ? '*' : ' ');
	info("  [%c] V4L2_CAP_MODULATOR",
	     cap.capabilities & V4L2_CAP_MODULATOR ? '*' : ' ');
	info("  [%c] V4L2_CAP_SDR_CAPTURE",
	     cap.capabilities & V4L2_CAP_SDR_CAPTURE ? '*' : ' ');
	info("  [%c] V4L2_CAP_EXT_PIX_FORMAT",
	     cap.capabilities & V4L2_CAP_EXT_PIX_FORMAT ? '*' : ' ');
	info("  [%c] V4L2_CAP_READWRITE",
	     cap.capabilities & V4L2_CAP_READWRITE ? '*' : ' ');
	info("  [%c] V4L2_CAP_ASYNCIO",
	     cap.capabilities & V4L2_CAP_ASYNCIO ? '*' : ' ');
	info("  [%c] V4L2_CAP_STREAMING",
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

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONTINUE_DATA_TRANSFER;
	control.value = 1;

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

	return 0;
}

int video_set_dpb(struct instance *i,
		  enum v4l2_mpeg_vidc_video_dpb_color_format format)
{
	struct v4l2_ext_control control[2] = {0};
	struct v4l2_ext_controls controls = {0};

	control[0].id = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE;
	control[0].value =
		(format == V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_TP10_UBWC) ?
		V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_SECONDARY :
		V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_PRIMARY;

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

int video_queue_buf_out(struct instance *i, int n, int length,
			uint32_t flags, struct timeval timestamp)
{
	struct video *vid = &i->video;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[1];

	if (n >= vid->out_buf_cnt) {
		err("Tried to queue a non existing buffer");
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
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
		err("Failed to queue buffer (index=%d) on OUTPUT: %m",
		    buf.index);
		return -1;
	}

	dbg("Queued buffer on OUTPUT queue with index %d "
	    "(flags:%08x, bytesused:%d, ts: %ld.%lu)",
	    buf.index, buf.flags, buf.m.planes[0].bytesused,
	    buf.timestamp.tv_sec, buf.timestamp.tv_usec);

	return 0;
}

int video_queue_buf_cap(struct instance *i, int n)
{
	struct video *vid = &i->video;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];

	if (n >= vid->cap_buf_cnt) {
		err("Tried to queue a non existing buffer");
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.index = n;
	buf.length = 2;
	buf.m.planes = planes;

	buf.m.planes[0].m.userptr = (unsigned long)vid->cap_ion_addr;
	buf.m.planes[0].reserved[0] = vid->cap_ion_fd;
	buf.m.planes[0].reserved[1] = vid->cap_buf_off[n][0];
	buf.m.planes[0].length = vid->cap_buf_size[0];
	buf.m.planes[0].bytesused = vid->cap_buf_size[0];
	buf.m.planes[0].data_offset = 0;

	buf.m.planes[1].m.userptr = (unsigned long)vid->cap_ion_addr;
	buf.m.planes[1].reserved[0] = vid->cap_ion_fd;
	buf.m.planes[1].reserved[1] = 0;
	buf.m.planes[1].length = 0;
	buf.m.planes[1].bytesused = 0;
	buf.m.planes[1].data_offset = 0;

	if (ioctl(vid->fd, VIDIOC_QBUF, &buf) < 0) {
		err("Failed to queue buffer (index=%d) on CAPTURE: %m",
		    buf.index);
		return -1;
	}

	dbg("Queued buffer on CAPTURE queue with index %d", buf.index);

	return 0;
}

static int video_dequeue_buf(struct instance *i, struct v4l2_buffer *buf)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, VIDIOC_DQBUF, buf);
	if (ret < 0) {
		err("Failed to dequeue buffer on %s: %m",
		    dbg_type[buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE]);
		return -errno;
	}

	switch (buf->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		dbg("Dequeued buffer on OUTPUT queue with index %d",
		    buf->index);
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		dbg("Dequeued buffer on CAPTURE queue with index %d "
		    "(flags:%08x, bytesused:%d, ts: %ld.%lu)",
		    buf->index, buf->flags, buf->m.planes[0].bytesused,
		    buf->timestamp.tv_sec, buf->timestamp.tv_usec);
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

int video_dequeue_capture(struct instance *i, int *n, int *finished,
			  unsigned int *bytesused, struct timeval *ts)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[CAP_PLANES];

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.m.planes = planes;
	buf.length = CAP_PLANES;

	if (video_dequeue_buf(i, &buf))
		return -1;

	*finished = 0;

	if (buf.flags & V4L2_QCOM_BUF_FLAG_EOS)
		*finished = 1;

	*bytesused = buf.m.planes[0].bytesused;
	*n = buf.index;

	if (ts)
		*ts = buf.timestamp;

	return 0;
}

int video_stream(struct instance *i, enum v4l2_buf_type type, int status)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, status, &type);
	if (ret) {
		err("Failed to change streaming (type=%s, status=%s)",
		    dbg_type[type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE],
		    dbg_status[status == VIDIOC_STREAMOFF]);
		return -1;
	}

	dbg("Stream %s on %s queue", dbg_status[status==VIDIOC_STREAMOFF],
	    dbg_type[type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE]);

	return 0;
}

int video_flush(struct instance *i, uint32_t flags)
{
	struct video *vid = &i->video;
	struct v4l2_decoder_cmd dec;

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
alloc_ion_buffer(struct instance *i, size_t size)
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

	ion_alloc.len = size;
	ion_alloc.align = 4096;
	ion_alloc.heap_id_mask = ION_HEAP(ION_IOMMU_HEAP_ID);
	ion_alloc.flags = 0;
	ion_alloc.handle = -1;

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

static int get_msm_color_format(uint32_t fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_NV12:
		return COLOR_FMT_NV12;
	case V4L2_PIX_FMT_NV21:
		return COLOR_FMT_NV21;
	case V4L2_PIX_FMT_NV12_UBWC:
		return COLOR_FMT_NV12_UBWC;
	case V4L2_PIX_FMT_NV12_TP10_UBWC:
		return COLOR_FMT_NV12_BPP10_UBWC;
	case V4L2_PIX_FMT_RGBA8888_UBWC:
		return COLOR_FMT_RGBA8888_UBWC;
	}

	return -1;
}

int video_setup_capture(struct instance *i, int num_buffers, int w, int h)
{
	struct video *vid = &i->video;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers reqbuf;
	int buffer_size;
	int color_fmt;
	int ion_fd;
	void *buf_addr;
	int n;

	memzero(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.width = w;
#if 0
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12_UBWC;
#else
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
#endif

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("Failed to set format (%dx%d)", w, h);
		return -1;
	}

	memzero(reqbuf);
	reqbuf.count = num_buffers;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	reqbuf.memory = V4L2_MEMORY_USERPTR;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS failed on CAPTURE queue: %m");
		return -1;
	}

	dbg("Number of CAPTURE buffers is %d (requested %d)",
	    reqbuf.count, num_buffers);

	vid->cap_buf_cnt = reqbuf.count;

	if (ioctl(vid->fd, VIDIOC_G_FMT, &fmt) < 0) {
		err("Failed to get format");
		return -1;
	}

	dbg("  %dx%d fmt=%s (%d planes) field=%s cspace=%s flags=%08x",
	    fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	    fourcc_to_string(fmt.fmt.pix_mp.pixelformat),
	    fmt.fmt.pix_mp.num_planes,
	    v4l2_field_to_string(fmt.fmt.pix_mp.field),
	    v4l2_colorspace_to_string(fmt.fmt.pix_mp.colorspace),
	    fmt.fmt.pix_mp.flags);

	for (n = 0; n < fmt.fmt.pix_mp.num_planes; n++)
		dbg("    plane %d: size=%d stride=%d scanlines=%d", n,
		    fmt.fmt.pix_mp.plane_fmt[n].sizeimage,
		    fmt.fmt.pix_mp.plane_fmt[n].bytesperline,
		    fmt.fmt.pix_mp.plane_fmt[n].reserved[0]);

	color_fmt = get_msm_color_format(fmt.fmt.pix_mp.pixelformat);
	if (color_fmt < 0) {
		err("unhandled output pixel format");
		return -1;
	}

	vid->cap_buf_format = fmt.fmt.pix_mp.pixelformat;
	vid->cap_w = fmt.fmt.pix_mp.width;
	vid->cap_h = fmt.fmt.pix_mp.height;
	vid->cap_buf_stride[0] = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	vid->cap_buf_size[0] = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

#if 0
	vid->cap_h = VENUS_Y_SCANLINES(color_fmt, fmt.fmt.pix_mp.height);
	vid->cap_buf_stride[0] = VENUS_Y_STRIDE(color_fmt, fmt.fmt.pix_mp.width);
#endif

	buffer_size = VENUS_BUFFER_SIZE(color_fmt,
					fmt.fmt.pix_mp.width,
					fmt.fmt.pix_mp.height);

	if (vid->cap_buf_size[0] < buffer_size)
		vid->cap_buf_size[0] = buffer_size;

	ion_fd = alloc_ion_buffer(i, vid->cap_buf_cnt * vid->cap_buf_size[0]);
	if (ion_fd < 0)
		return -1;

	buf_addr = mmap(NULL, vid->cap_buf_cnt * vid->cap_buf_size[0],
			PROT_READ, MAP_SHARED, ion_fd, 0);
	if (buf_addr == MAP_FAILED) {
		err("Failed to map output buffer: %m");
		return -1;
	}

	vid->cap_ion_fd = ion_fd;
	vid->cap_ion_addr = buf_addr;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		vid->cap_buf_off[n][0] = n * vid->cap_buf_size[0];
		vid->cap_buf_addr[n][0] = buf_addr + vid->cap_buf_off[n][0];
	}

	dbg("Succesfully mmapped %d CAPTURE buffers", n);

	return 0;
}

int video_stop_capture(struct instance *i)
{
	struct video *vid = &i->video;
	struct v4l2_requestbuffers reqbuf;

	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			 VIDIOC_STREAMOFF))
		return -1;

	if (vid->cap_ion_addr) {
		if (munmap(vid->cap_ion_addr,
			   vid->cap_buf_cnt * vid->cap_buf_size[0]))
			err("failed to unmap buffer: %m");
	}

	if (vid->cap_ion_fd >= 0) {
		if (close(vid->cap_ion_fd) < 0)
			err("failed to close ion buffer: %m");
	}

	vid->cap_ion_fd = -1;
	vid->cap_ion_addr = NULL;
	vid->cap_buf_cnt = 0;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_USERPTR;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on CAPTURE queue failed: %m");
		return -1;
	}

	return 0;
}

static int video_set_framerate(struct instance *i, int num, int den)
{
	struct v4l2_streamparm parm;

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

int video_setup_output(struct instance *i, unsigned long codec,
		       unsigned int size, int count)
{
	struct video *vid = &i->video;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers reqbuf;
	int ion_fd;
	void *buf_addr;
	int n;

	memzero(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width = i->width;
	fmt.fmt.pix_mp.height = i->height;
	fmt.fmt.pix_mp.pixelformat = codec;

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("Failed to set format on OUTPUT: %m");
		return -1;
	}

	dbg("Setup decoding OUTPUT buffer size=%u (requested=%u)",
	    fmt.fmt.pix_mp.plane_fmt[0].sizeimage, size);

	vid->out_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	memzero(reqbuf);
	reqbuf.count = count;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	reqbuf.memory = V4L2_MEMORY_USERPTR;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS failed on OUTPUT queue");
		return -1;
	}

	vid->out_buf_cnt = reqbuf.count;

	dbg("Number of video decoder OUTPUT buffers is %d (requested %d)",
	    vid->out_buf_cnt, count);

	ion_fd = alloc_ion_buffer(i, vid->out_buf_cnt * vid->out_buf_size);
	if (ion_fd < 0)
		return -1;

	buf_addr = mmap(NULL, vid->out_buf_cnt * vid->out_buf_size,
			PROT_READ | PROT_WRITE, MAP_SHARED, ion_fd, 0);
	if (buf_addr == MAP_FAILED) {
		err("Failed to map output buffer: %m");
		return -1;
	}

	vid->out_ion_fd = ion_fd;
	vid->out_ion_addr = buf_addr;

	for (n = 0; n < vid->out_buf_cnt; n++) {
		vid->out_buf_off[n] = n * vid->out_buf_size;
		vid->out_buf_addr[n] = buf_addr + vid->out_buf_off[n];
		vid->out_buf_flag[n] = 0;
	}

	dbg("Succesfully mmapped %d OUTPUT buffers", n);

	video_set_framerate(i, 25, 1);

	return 0;
}

int video_stop_output(struct instance *i)
{
	struct video *vid = &i->video;
	struct v4l2_requestbuffers reqbuf;

	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			 VIDIOC_STREAMOFF))
		return -1;

	if (vid->out_ion_addr) {
		if (munmap(vid->out_ion_addr,
			   vid->out_buf_cnt * vid->out_buf_size))
			err("failed to unmap buffer: %m");
	}

	if (vid->out_ion_fd >= 0) {
		if (close(vid->out_ion_fd) < 0)
			err("failed to close ion buffer: %m");
	}

	vid->out_ion_fd = -1;
	vid->out_ion_addr = NULL;
	vid->out_buf_cnt = 0;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_USERPTR;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on OUTPUT queue failed: %m");
		return -1;
	}

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
