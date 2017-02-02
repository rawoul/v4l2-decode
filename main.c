/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Main file of the application
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

#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <linux/videodev2.h>
#include <media/msm_vidc.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "args.h"
#include "common.h"
#include "video.h"
#include "display.h"

#define DBG_TAG "  main"

#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))

/* This is the size of the buffer for the compressed stream.
 * It limits the maximum compressed frame size. */
#define STREAM_BUUFER_SIZE	(1024 * 1024)

static void stream_close(struct instance *i);

static const int event_type[] = {
	V4L2_EVENT_MSM_VIDC_FLUSH_DONE,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT,
	V4L2_EVENT_MSM_VIDC_SYS_ERROR,
	V4L2_EVENT_MSM_VIDC_HW_OVERLOAD,
	V4L2_EVENT_MSM_VIDC_HW_UNSUPPORTED,
	V4L2_EVENT_MSM_VIDC_RELEASE_BUFFER_REFERENCE,
	V4L2_EVENT_MSM_VIDC_RELEASE_UNQUEUED_BUFFER,
};

static int
subscribe_events(struct instance *i)
{
	const int n_events = sizeof(event_type) / sizeof(event_type[0]);
	int idx;

	for (idx = 0; idx < n_events; idx++) {
		if (video_subscribe_event(i, event_type[idx]))
			return -1;
	}

	return 0;
}

static int
restart_capture(struct instance *i)
{
	struct video *vid = &i->video;
	int n;

	/*
	 * Destroy window buffers that are not in use by the
	 * wayland compositor; buffers in use will be destroyed
	 * when the release callback is called
	 */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		struct fb *fb = i->disp_buffers[n];
		if (fb && !fb->busy)
			fb_destroy(fb);
	}

	/* Stop capture and release buffers */
	if (vid->cap_buf_cnt > 0 && video_stop_capture(i))
		return -1;

	/* Setup capture queue with new parameters */
	if (video_setup_capture(i, 4, i->width, i->height))
		return -1;

	/* Queue all capture buffers */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		if (video_queue_buf_cap(i, n))
			return -1;
	}

	/* Start streaming */
	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			 VIDIOC_STREAMON))
		return -1;

	/*
	 * Recreate the window frame buffers
	 */
	i->group++;

	if (i->window) {
		for (n = 0; n < vid->cap_buf_cnt; n++) {
			i->disp_buffers[n] =
				window_create_buffer(i->window, i->group, n,
						     vid->cap_ion_fd,
						     vid->cap_buf_off[n][0],
						     vid->cap_buf_format,
						     vid->cap_w, vid->cap_h,
						     vid->cap_buf_stride[0]);
			if (!i->disp_buffers[n])
				return -1;
		}
	}

	return 0;
}

static const char *
colorspace_to_string(int cspace)
{
	switch (cspace) {
	case MSM_VIDC_BT709_5:
		return "bt709";
	case MSM_VIDC_UNSPECIFIED:
		return "unspecified";
	case MSM_VIDC_BT470_6_M:
		return "bt470m";
	case MSM_VIDC_BT601_6_625:
		return "bt601/625";
	case MSM_VIDC_BT601_6_525:
		return "bt601/525";
	case MSM_VIDC_SMPTE_240M:
		return "smpte240m";
	case MSM_VIDC_GENERIC_FILM:
		return "generic";
	case MSM_VIDC_BT2020:
		return "bt2020";
	case MSM_VIDC_RESERVED_1:
		return "reserved1";
	case MSM_VIDC_RESERVED_2:
		return "reserved2";
	}
	return "unknown";
}

static const char *
depth_to_string(int depth)
{
	switch (depth) {
	case MSM_VIDC_BIT_DEPTH_8:
		return "8bits";
	case MSM_VIDC_BIT_DEPTH_10:
		return "10bits";
	case MSM_VIDC_BIT_DEPTH_UNSUPPORTED:
		return "unsupported";
	}
	return "unknown";
}

static const char *
pic_struct_to_string(int pic)
{
	switch (pic) {
	case MSM_VIDC_PIC_STRUCT_PROGRESSIVE:
		return "progressive";
	case MSM_VIDC_PIC_STRUCT_MAYBE_INTERLACED:
		return "interlaced";
	}
	return "unknown";
}

static int
handle_video_event(struct instance *i)
{
	struct v4l2_event event;

	if (video_dequeue_event(i, &event))
		return -1;

	switch (event.type) {
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT: {
		unsigned int *ptr = (unsigned int *)event.u.data;
		unsigned int height = ptr[0];
		unsigned int width = ptr[1];

		info("Port Reconfig received insufficient, new size %ux%u",
		     width, height);

		if (ptr[2] & V4L2_EVENT_BITDEPTH_FLAG) {
			enum msm_vidc_pixel_depth depth = ptr[3];
			info("  bit depth changed to %s",
			     depth_to_string(depth));

			switch (depth) {
			case MSM_VIDC_BIT_DEPTH_10:
				i->depth = 10;
				break;
			case MSM_VIDC_BIT_DEPTH_8:
				i->depth = 8;
				break;
			default:
				i->depth = 0;
				break;
			}
		}

		if (ptr[2] & V4L2_EVENT_PICSTRUCT_FLAG) {
			unsigned int pic_struct = ptr[4];
			info("  interlacing changed to %s",
			     pic_struct_to_string(pic_struct));

			if (pic_struct == MSM_VIDC_PIC_STRUCT_MAYBE_INTERLACED)
				i->interlaced = 1;
			else
				i->interlaced = 0;
		}

		if (ptr[2] & V4L2_EVENT_COLOUR_SPACE_FLAG) {
			unsigned int cspace = ptr[5];
			info("  colorspace changed to %s",
			     colorspace_to_string(cspace));
		}

		i->width = width;
		i->height = height;
		i->reconfigure_pending = 1;

		/* flush capture queue, we will reconfigure it when flush
		 * done event is received */
		video_flush(i, V4L2_QCOM_CMD_FLUSH_CAPTURE);
		break;
	}
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT:
		dbg("Setting changed sufficient");
		break;
	case V4L2_EVENT_MSM_VIDC_FLUSH_DONE: {
		unsigned int *ptr = (unsigned int *)event.u.data;
		unsigned int flags = ptr[0];

		if (flags & V4L2_QCOM_CMD_FLUSH_CAPTURE)
			dbg("Flush Done received on CAPTURE queue");
		if (flags & V4L2_QCOM_CMD_FLUSH_OUTPUT)
			dbg("Flush Done received on OUTPUT queue");

		if (i->reconfigure_pending) {
			dbg("Reconfiguring output");
			restart_capture(i);
			i->reconfigure_pending = 0;
		}
		break;
	}
	case V4L2_EVENT_MSM_VIDC_SYS_ERROR:
		dbg("SYS Error received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_OVERLOAD:
		dbg("HW Overload received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_UNSUPPORTED:
		dbg("HW Unsupported received");
		break;
	case V4L2_EVENT_MSM_VIDC_RELEASE_BUFFER_REFERENCE:
		dbg("Release buffer reference");
		break;
	case V4L2_EVENT_MSM_VIDC_RELEASE_UNQUEUED_BUFFER:
		dbg("Release unqueued buffer");
		break;
	default:
		dbg("unknown event type occurred %x", event.type);
		break;
	}

	return 0;
}

static void
cleanup(struct instance *i)
{
	stream_close(i);
	if (i->window)
		window_destroy(i->window);
	if (i->display)
		display_destroy(i->display);
	if (i->sigfd != 1)
		close(i->sigfd);
	if (i->video.fd)
		video_close(i);
}

static int
save_frame(struct instance *i, const void *buf, unsigned int size)
{
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	char filename[64];
	int fd;
	int ret;
	static unsigned int frame_num = 0;

	if (!i->save_frames)
		return 0;

	if (!i->save_path)
		ret = sprintf(filename, "/mnt/frame%04d.nv12", frame_num);
	else
		ret = sprintf(filename, "%s/frame%04d.nv12", i->save_path,
			      frame_num);
	if (ret < 0) {
		err("sprintf fail (%s)", strerror(errno));
		return -1;
	}

	dbg("create file %s", filename);

	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, mode);
	if (fd < 0) {
		err("cannot open file (%s)", strerror(errno));
		return -1;
	}

	ret = write(fd, buf, size);
	if (ret < 0) {
		err("cannot write to file (%s)", strerror(errno));
		return -1;
	}

	close(fd);

	frame_num++;

	return 0;
}

struct ts_entry {
	uint64_t pts;
	uint64_t dts;
	uint64_t duration;
	uint64_t base;
	struct list_head link;
};

#define TIMESTAMP_NONE	((uint64_t)-1)

static struct ts_entry *
ts_insert(struct video *vid, uint64_t pts, uint64_t dts, uint64_t duration,
	  uint64_t base)
{
	struct ts_entry *l;

	l = malloc(sizeof (*l));
	if (!l)
		return NULL;

	l->pts = pts;
	l->dts = dts;
	l->duration = duration;
	l->base = base;

	list_add_tail(&l->link, &vid->pending_ts_list);

	return l;
}

static void
ts_remove(struct ts_entry *l)
{
	list_del(&l->link);
	free(l);
}

static int
parse_frame(struct instance *i, AVPacket *pkt)
{
	int ret;

	if (!i->bsf_data_pending) {
		ret = av_read_frame(i->avctx, pkt);
		if (ret < 0)
			return ret;

		if (pkt->stream_index != i->stream->index) {
			av_packet_unref(pkt);
			return AVERROR(EAGAIN);
		}

		if (i->bsf) {
			ret = av_bsf_send_packet(i->bsf, pkt);
			if (ret < 0)
				return ret;

			i->bsf_data_pending = 1;
		}
	}

	if (i->bsf) {
		ret = av_bsf_receive_packet(i->bsf, pkt);
		if (ret == AVERROR(EAGAIN))
			i->bsf_data_pending = 0;

		if (ret < 0)
			return ret;
	}

	return 0;
}

static void
finish(struct instance *i)
{
	pthread_mutex_lock(&i->lock);
	i->finish = 1;
	pthread_cond_signal(&i->cond);
	pthread_mutex_unlock(&i->lock);
}

static int
send_eos(struct instance *i, int buf_index)
{
	struct video *vid = &i->video;
	struct timeval tv;

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (video_queue_buf_out(i, buf_index, 0,
				V4L2_QCOM_BUF_FLAG_EOS |
				V4L2_QCOM_BUF_TIMESTAMP_INVALID, tv) < 0)
		return -1;

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}

static char *
dump_pkt(const uint8_t *data, size_t size)
{
	static char *buf;
	static size_t buf_size;
	size_t s = size * 3 + 1;

	if (!buf || buf_size < s) {
		char *old = buf;
		s = (s + 4095) & ~4095;
		buf = realloc(old, s);
		if (!buf) {
			free(old);
			return NULL;
		}
		buf_size = s;
	}

	for (size_t i = 0; i < size; i++) {
		sprintf(buf + i * 3, "%c%02x",
			i % 32 == 0 ? '\n' : ' ', data[i]);
	}

	buf[s - 1] = 0;

	return buf;
}


/*
 * Escape start codes in BDU
 */
static int
rbdu_escape(uint8_t *dst, int dst_size, const uint8_t *src, int src_size)
{
	uint8_t *dstp = dst;
	const uint8_t *srcp = src;
	const uint8_t *end = src + src_size;
	int count = 0;

	while (srcp < end) {
		if (count == 2 && *srcp <= 0x03) {
			*dstp++ = 0x03;
			count = 0;
		}

		if (*srcp == 0)
			count++;
		else
			count = 0;

		*dstp++ = *srcp++;
	}

	return dstp - dst;
}

/*
 * Transform RBDU (raw bitstream decodable units)
 *  into an EBDU (encapsulated bitstream decodable units)
 */
static int
vc1_write_bdu(uint8_t *dst, int dst_size,
	      uint8_t *bdu, int bdu_size,
	      uint8_t type)
{
	int len;

	/* add start code */
	dst[0] = 0x00;
	dst[1] = 0x00;
	dst[2] = 0x01;
	dst[3] = type;
	len = 4;

	/* escape start codes */
	len += rbdu_escape(dst + len, dst_size - len, bdu, bdu_size);

	/* add flushing byte at the end of the BDU */
	dst[len++] = 0x80;

	return len;
}

static int
vc1_find_sc(const uint8_t *data, int size)
{
	for (int i = 0; i < size - 4; i++) {
		if (data[i + 0] == 0x00 &&
		    data[i + 1] == 0x00 &&
		    data[i + 2] == 0x01)
			return i;
	}

	return -1;
}

static int
write_sequence_header_vc1(struct instance *i, uint8_t *data, int size)
{
	AVCodecParameters *codecpar = i->stream->codecpar;
	int n;

	if (codecpar->extradata_size == 0) {
		dbg("no codec data, skip sequence header generation");
		return 0;
	}

	if (codecpar->extradata_size == 4 || codecpar->extradata_size == 5) {
		/* Simple/Main Profile ASF header */
		return vc1_write_bdu(data, size,
				     codecpar->extradata,
				     codecpar->extradata_size,
				     0x0f);
	}

	if (codecpar->extradata_size == 36 && codecpar->extradata[3] == 0xc5) {
		/* Annex L Sequence Layer */
		if (size < codecpar->extradata_size)
			return -1;

		memcpy(data, codecpar->extradata, codecpar->extradata_size);
		return codecpar->extradata_size;
	}

	n = vc1_find_sc(codecpar->extradata, codecpar->extradata_size);
	if (n >= 0) {
		/* BDU in header */
		if (size < codecpar->extradata_size - n)
			return -1;

		memcpy(data, codecpar->extradata + n,
		       codecpar->extradata_size - n);
		return codecpar->extradata_size - n;
	}

	err("cannot parse VC1 codec data");

	return -1;
}

static int
write_sequence_header(struct instance *i, uint8_t *data, int size)
{
	AVCodecParameters *codecpar = i->stream->codecpar;

	switch (codecpar->codec_id) {
	case AV_CODEC_ID_WMV3:
	case AV_CODEC_ID_VC1:
		return write_sequence_header_vc1(i, data, size);
	default:
		return 0;
	}
}

static int
send_pkt(struct instance *i, int buf_index, AVPacket *pkt)
{
	struct video *vid = &i->video;
	struct timeval tv;
	uint64_t pts, dts, duration, start_time;
	int flags;
	int size;
	uint8_t *data;
	const char *hex;
	AVRational vid_timebase;
	AVRational v4l_timebase = { 1, 1000000 };
	AVCodecParameters *codecpar = i->stream->codecpar;

	data = (uint8_t *)vid->out_buf_addr[buf_index];
	size = 0;

	if (i->need_header) {
		int n = write_sequence_header(i, data, vid->out_buf_size);
		if (n > 0)
			size += n;

		switch (codecpar->codec_id) {
		case AV_CODEC_ID_WMV3:
		case AV_CODEC_ID_VC1:
			if (vc1_find_sc(pkt->data, MIN(10, pkt->size)) < 0)
				i->insert_sc = 1;
			break;
		default:
			break;
		}

		i->need_header = 0;
	}

	if ((codecpar->codec_id == AV_CODEC_ID_WMV3 ||
	     codecpar->codec_id == AV_CODEC_ID_VC1) &&
	    i->insert_sc) {
		size += vc1_write_bdu(data + size, vid->out_buf_size - size,
				      pkt->data, pkt->size, 0x0d);
	} else {
		memcpy(data + size, pkt->data, pkt->size);
		size += pkt->size;
	}

	flags = 0;

	vid_timebase = i->stream->time_base;

	start_time = 0;
	if (i->stream->start_time != AV_NOPTS_VALUE)
		start_time = av_rescale_q(i->stream->start_time,
					  vid_timebase, v4l_timebase);

	pts = TIMESTAMP_NONE;
	if (pkt->pts != AV_NOPTS_VALUE)
		pts = av_rescale_q(pkt->pts, vid_timebase, v4l_timebase);

	dts = TIMESTAMP_NONE;
	if (pkt->dts != AV_NOPTS_VALUE)
		dts = av_rescale_q(pkt->dts, vid_timebase, v4l_timebase);

	duration = TIMESTAMP_NONE;
	if (pkt->duration) {
		duration = av_rescale_q(pkt->duration,
					vid_timebase, v4l_timebase);
	}

	if (debug_level > 3)
		hex = dump_pkt(data, size);
	else
		hex = "";

	dbg("input size=%d pts=%" PRIi64 " dts=%" PRIi64 " duration=%" PRIu64
	     " start_time=%" PRIi64 "%s", size, pts, dts, duration,
	     start_time, hex);

	if (pts != TIMESTAMP_NONE) {
		tv.tv_sec = pts / 1000000;
		tv.tv_usec = pts % 1000000;
	} else {
		flags |= V4L2_QCOM_BUF_TIMESTAMP_INVALID;
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	if ((pkt->flags & AV_PKT_FLAG_KEY) &&
	    pts != TIMESTAMP_NONE && dts != TIMESTAMP_NONE)
		vid->pts_dts_delta = pts - dts;

	if (video_queue_buf_out(i, buf_index, size, flags, tv) < 0)
		return -1;

	pthread_mutex_lock(&i->lock);
	ts_insert(vid, pts, dts, duration, start_time);
	pthread_mutex_unlock(&i->lock);

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}

static int
get_buffer_unlocked(struct instance *i)
{
	struct video *vid = &i->video;

	for (int n = 0; n < vid->out_buf_cnt; n++) {
		if (!vid->out_buf_flag[n])
			return n;
	}

	return -1;
}

/* This threads is responsible for parsing the stream and
 * feeding video decoder with consecutive frames to decode */
static void *
parser_thread_func(void *args)
{
	struct instance *i = (struct instance *)args;
	AVPacket pkt;
	int buf, parse_ret;

	dbg("Parser thread started");

	av_init_packet(&pkt);

	while (1) {
		parse_ret = parse_frame(i, &pkt);
		if (parse_ret == AVERROR(EAGAIN))
			continue;


		buf = -1;

		pthread_mutex_lock(&i->lock);
		while (!i->finish && (buf = get_buffer_unlocked(i)) < 0)
			pthread_cond_wait(&i->cond, &i->lock);
		pthread_mutex_unlock(&i->lock);

		if (buf < 0) {
			/* decoding stopped before parsing ended, abort */
			break;
		}

		if (parse_ret < 0) {
			if (parse_ret == AVERROR_EOF)
				dbg("Queue end of stream");
			else
				av_err(parse_ret, "Parsing failed");

			send_eos(i, buf);
			break;
		}

		if (send_pkt(i, buf, &pkt) < 0)
			break;

		av_packet_unref(&pkt);
	}

	av_packet_unref(&pkt);

	dbg("Parser thread finished");

	return NULL;
}

static void
buffer_released(struct fb *fb, void *data)
{
	struct instance *i = data;
	int n = fb->index;

	if (fb->group != i->group) {
		fb_destroy(fb);
		return;
	}

	if (!i->reconfigure_pending)
		video_queue_buf_cap(i, n);
}

static int
handle_video_capture(struct instance *i)
{
	struct video *vid = &i->video;
	struct timeval tv;
	uint32_t flags;
	uint64_t pts;
	unsigned int bytesused;
	bool busy;
	int ret, n;

	/* capture buffer is ready */

	ret = video_dequeue_capture(i, &n, &bytesused, &flags, &tv);
	if (ret < 0) {
		err("dequeue capture buffer fail");
		return ret;
	}

	if (flags & V4L2_QCOM_BUF_TIMESTAMP_INVALID)
		pts = TIMESTAMP_NONE;
	else
		pts = ((uint64_t)tv.tv_sec) * 1000000 + tv.tv_usec;

	busy = false;

	if (bytesused > 0) {
		struct ts_entry *l, *min = NULL;
		int pending = 0;

		vid->total_captured++;

		save_frame(i, (void *)vid->cap_buf_addr[n][0],
			   bytesused);

		pthread_mutex_lock(&i->lock);

		/* PTS are expected to be monotonically increasing,
		 * so when unknown use the lowest pending DTS */
		list_for_each_entry(l, &vid->pending_ts_list, link) {
			if (l->dts == TIMESTAMP_NONE)
				continue;
			if (min == NULL || min->dts > l->dts)
				min = l;
			pending++;
		}

		if (min) {
			dbg("pending %d min pts %" PRIi64
			    " dts %" PRIi64
			    " duration %" PRIi64, pending,
			    min->pts, min->dts, min->duration);
		}

		if (pts == TIMESTAMP_NONE) {
			dbg("no pts on frame");
			if (min && vid->pts_dts_delta != TIMESTAMP_NONE) {
				dbg("reuse dts %" PRIu64
				    " delta %" PRIu64,
				    min->dts, vid->pts_dts_delta);
				pts = min->dts + vid->pts_dts_delta;
			}
		}

		if (pts == TIMESTAMP_NONE) {
			if (min && vid->cap_last_pts != TIMESTAMP_NONE)
				pts = vid->cap_last_pts + min->duration;
			else
				pts = 0;

			dbg("guessing pts %" PRIu64, pts);
		}

		vid->cap_last_pts = pts;

		if (min != NULL) {
			pts -= min->base;
			ts_remove(min);
		}

		pthread_mutex_unlock(&i->lock);

		if (i->window) {
			info("show buffer pts=%" PRIu64, pts);
			window_show_buffer(i->window, i->disp_buffers[n],
					   buffer_released, i);
			busy = true;
		}

		i->prerolled = 1;

	}

	if (!busy && !i->reconfigure_pending)
		video_queue_buf_cap(i, n);

	if (flags & V4L2_QCOM_BUF_FLAG_EOS) {
		info("End of stream");
		finish(i);
	}

	return 0;
}

static int
handle_video_output(struct instance *i)
{
	struct video *vid = &i->video;
	int ret, n;

	ret = video_dequeue_output(i, &n);
	if (ret < 0) {
		err("dequeue output buffer fail");
		return ret;
	}

	pthread_mutex_lock(&i->lock);
	vid->out_buf_flag[n] = 0;
	pthread_cond_signal(&i->cond);
	pthread_mutex_unlock(&i->lock);

	return 0;
}

static int
handle_signal(struct instance *i)
{
	struct signalfd_siginfo siginfo;
	sigset_t sigmask;

	if (read(i->sigfd, &siginfo, sizeof (siginfo)) < 0) {
		perror("signalfd/read");
		return -1;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, siginfo.ssi_signo);
	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);

	finish(i);

	return 0;
}

static int
setup_signal(struct instance *i)
{
	sigset_t sigmask;
	int fd;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	fd = signalfd(-1, &sigmask, SFD_CLOEXEC);
	if (fd < 0) {
		perror("signalfd");
		return -1;
	}

	sigprocmask(SIG_BLOCK, &sigmask, NULL);
	i->sigfd = fd;

	return 0;
}

enum {
	EV_VIDEO,
	EV_DISPLAY,
	EV_SIGNAL,
	EV_COUNT
};

void main_loop(struct instance *i)
{
	struct video *vid = &i->video;
	struct wl_display *wl_display = NULL;
	struct pollfd pfd[EV_COUNT];
	int ev[EV_COUNT];
	short revents;
	int nfds = 0;
	int ret;

	dbg("main thread started");

	for (int i = 0; i < EV_COUNT; i++)
		ev[i] = -1;

	memset(pfd, 0, sizeof (pfd));

	pfd[nfds].fd = vid->fd;
	pfd[nfds].events = POLLOUT | POLLWRNORM | POLLPRI;
	ev[EV_VIDEO] = nfds++;

	if (i->display) {
		wl_display = display_get_wl_display(i->display);
		pfd[nfds].fd = wl_display_get_fd(wl_display);
		pfd[nfds].events = POLLIN;
		ev[EV_DISPLAY] = nfds++;
	}

	if (i->sigfd != -1) {
		pfd[nfds].fd = i->sigfd;
		pfd[nfds].events = POLLIN;
		ev[EV_SIGNAL] = nfds++;
	}

	while (!i->finish) {
		if (i->display) {
			if (!display_is_running(i->display))
				break;

			while (wl_display_prepare_read(wl_display) != 0)
				wl_display_dispatch_pending(wl_display);

			ret = wl_display_flush(wl_display);
			if (ret < 0) {
				if (errno == EAGAIN)
					pfd[ev[EV_DISPLAY]].events |= POLLOUT;
				else if (errno != EPIPE) {
					err("wl_display_flush: %m");
					wl_display_cancel_read(wl_display);
					break;
				}
			}
		}

		if (i->paused && i->prerolled)
			pfd[ev[EV_VIDEO]].events &= ~(POLLIN | POLLRDNORM);
		else
			pfd[ev[EV_VIDEO]].events |= POLLIN | POLLRDNORM;

		ret = poll(pfd, nfds, -1);
		if (ret <= 0) {
			err("poll error");
			break;
		}

		if (i->display) {
			ret = wl_display_read_events(wl_display);
			if (ret < 0) {
				err("wl_display_read_events: %m");
				break;
			}

			ret = wl_display_dispatch_pending(wl_display);
			if (ret < 0) {
				err("wl_display_dispatch_pending: %m");
				break;
			}
		}

		for (int idx = 0; idx < nfds; idx++) {
			revents = pfd[idx].revents;
			if (!revents)
				continue;

			if (idx == ev[EV_VIDEO]) {
				if (revents & (POLLIN | POLLRDNORM))
					handle_video_capture(i);
				if (revents & (POLLOUT | POLLWRNORM))
					handle_video_output(i);
				if (revents & POLLPRI)
					handle_video_event(i);

			} else if (idx == ev[EV_DISPLAY]) {
				if (revents & POLLOUT)
					pfd[ev[EV_DISPLAY]].events &= ~POLLOUT;

			} else if (idx == ev[EV_SIGNAL]) {
				handle_signal(i);
				break;
			}
		}
	}

	finish(i);

	dbg("main thread finished");
}

static void
handle_window_key(struct window *window, uint32_t time, uint32_t key,
		  enum wl_keyboard_key_state state)
{
	struct instance *i = window_get_user_data(window);

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	switch (key) {
	case KEY_ESC:
		finish(i);
		break;

	case KEY_SPACE:
		info("%s", i->paused ? "Resume" : "Pause");
		i->paused = !i->paused;
		if (i->paused)
			av_read_pause(i->avctx);
		else
			av_read_play(i->avctx);
		break;

	case KEY_S:
		info("Frame Step");
		i->prerolled = 0;
		break;

	case KEY_F:
		if (i->window)
			window_toggle_fullscreen(i->window);
		break;
	}
}

static int
setup_display(struct instance *i)
{
	AVRational ar;

	i->display = display_create();
	if (!i->display)
		return -1;

	i->window = display_create_window(i->display);
	if (!i->window)
		return -1;

	window_set_user_data(i->window, i);
	window_set_key_callback(i->window, handle_window_key);

	ar = av_guess_sample_aspect_ratio(i->avctx, i->stream, NULL);
	window_set_aspect_ratio(i->window, ar.num, ar.den);

	if (i->fullscreen)
		window_toggle_fullscreen(i->window);

	return 0;
}

static void
stream_close(struct instance *i)
{
	i->stream = NULL;
	if (i->bsf)
		av_bsf_free(&i->bsf);
	if (i->avctx)
		avformat_close_input(&i->avctx);
}

static int
get_av_log_level(void)
{
	if (debug_level >= 5)
		return AV_LOG_TRACE;
	if (debug_level >= 4)
		return AV_LOG_DEBUG;
	if (debug_level >= 3)
		return AV_LOG_VERBOSE;
	if (debug_level >= 2)
		return AV_LOG_INFO;
	if (debug_level >= 1)
		return AV_LOG_ERROR;
	return AV_LOG_QUIET;
}

static int
stream_open(struct instance *i)
{
	const AVBitStreamFilter *filter;
	AVCodecParameters *codecpar;
	AVRational framerate;
	int codec;
	int ret;

	av_log_set_level(get_av_log_level());

	av_register_all();
	avformat_network_init();

	ret = avformat_open_input(&i->avctx, i->url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", i->url);
		goto fail;
	}

	ret = avformat_find_stream_info(i->avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		goto fail;
	}

	av_dump_format(i->avctx, -1, i->url, 0);

	ret = av_find_best_stream(i->avctx, AVMEDIA_TYPE_VIDEO, -1, -1,
				  NULL, 0);
	if (ret < 0) {
		av_err(ret, "stream does not seem to contain video");
		goto fail;
	}

	i->stream = i->avctx->streams[ret];
	codecpar = i->stream->codecpar;

	i->width = codecpar->width;
	i->height = codecpar->height;
	i->need_header = 1;

	framerate = av_stream_get_r_frame_rate(i->stream);
	i->fps_n = framerate.num;
	i->fps_d = framerate.den;

	filter = NULL;

	switch (codecpar->codec_id) {
	case AV_CODEC_ID_H263:
		codec = V4L2_PIX_FMT_H263;
		break;
	case AV_CODEC_ID_H264:
		codec = V4L2_PIX_FMT_H264;
		filter = av_bsf_get_by_name("h264_mp4toannexb");
		break;
	case AV_CODEC_ID_HEVC:
		codec = V4L2_PIX_FMT_HEVC;
		filter = av_bsf_get_by_name("hevc_mp4toannexb");
		break;
	case AV_CODEC_ID_MPEG2VIDEO:
		codec = V4L2_PIX_FMT_MPEG2;
		break;
	case AV_CODEC_ID_MPEG4:
		codec = V4L2_PIX_FMT_MPEG4;
		break;
	case AV_CODEC_ID_MSMPEG4V3:
		codec = V4L2_PIX_FMT_DIVX_311;
		break;
	case AV_CODEC_ID_WMV3:
		codec = V4L2_PIX_FMT_VC1_ANNEX_G;
		break;
	case AV_CODEC_ID_VC1:
		codec = V4L2_PIX_FMT_VC1_ANNEX_G;
		break;
	case AV_CODEC_ID_VP8:
		codec = V4L2_PIX_FMT_VP8;
		break;
	case AV_CODEC_ID_VP9:
		codec = V4L2_PIX_FMT_VP9;
		break;
	default:
		err("cannot decode %s", avcodec_get_name(codecpar->codec_id));
		goto fail;
	}

	i->fourcc = codec;

	if (filter) {
		ret = av_bsf_alloc(filter, &i->bsf);
		if (ret < 0) {
			av_err(ret, "cannot allocate bistream filter");
			goto fail;
		}

		avcodec_parameters_copy(i->bsf->par_in, codecpar);
		i->bsf->time_base_in = i->stream->time_base;

		ret = av_bsf_init(i->bsf);
		if (ret < 0) {
			av_err(ret, "failed to initialize bitstream filter");
			goto fail;
		}
	}

	return 0;

fail:
	stream_close(i);
	return -1;
}

int main(int argc, char **argv)
{
	struct instance inst;
	pthread_t parser_thread;
	int ret;

	ret = parse_args(&inst, argc, argv);
	if (ret) {
		print_usage(argv[0]);
		return 1;
	}

	inst.sigfd = -1;
	pthread_mutex_init(&inst.lock, 0);
	pthread_cond_init(&inst.cond, 0);

	INIT_LIST_HEAD(&inst.video.pending_ts_list);
	inst.video.pts_dts_delta = TIMESTAMP_NONE;
	inst.video.cap_last_pts = TIMESTAMP_NONE;

	ret = stream_open(&inst);
	if (ret)
		goto err;

	ret = video_open(&inst, inst.video.name);
	if (ret)
		goto err;

	ret = subscribe_events(&inst);
	if (ret)
		goto err;

	if (inst.secure) {
		ret = video_set_secure(&inst);
		if (ret)
			goto err;
	}

	ret = video_setup_output(&inst, inst.fourcc,
				 STREAM_BUUFER_SIZE, 6);
	if (ret)
		goto err;

	ret = setup_display(&inst);
	if (ret)
		err("display server not available, continuing anyway...");

	ret = video_set_control(&inst);
	if (ret)
		goto err;

	ret = video_stream(&inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			   VIDIOC_STREAMON);
	if (ret)
		goto err;

	ret = restart_capture(&inst);
	if (ret)
		goto err;

	dbg("Launching threads");

	setup_signal(&inst);

	if (pthread_create(&parser_thread, NULL, parser_thread_func, &inst))
		goto err;

	main_loop(&inst);

	pthread_join(parser_thread, 0);

	dbg("Threads have finished");

	video_stop_capture(&inst);
	video_stop_output(&inst);

	cleanup(&inst);

	pthread_cond_destroy(&inst.cond);
	pthread_mutex_destroy(&inst.lock);

	info("Total frames captured %ld", inst.video.total_captured);

	return 0;
err:
	cleanup(&inst);
	return 1;
}

