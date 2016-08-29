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

		vid->cap_buf_flag[n] = 1;
	}

	/* Start streaming */
	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			 VIDIOC_STREAMON))
		return -1;

	/*
	 * Recreate the window frame buffers
	 */
	i->group++;

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

	return 0;
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
			     depth == MSM_VIDC_BIT_DEPTH_10 ? "10bits" :
			     depth == MSM_VIDC_BIT_DEPTH_8 ? "8bits" :
			     "??");

			video_set_dpb(i, depth == MSM_VIDC_BIT_DEPTH_10 ?
				      V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_TP10_UBWC :
				      V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_NONE);
		}

		if (ptr[2] & V4L2_EVENT_PICSTRUCT_FLAG) {
			unsigned int pic_struct = ptr[4];
			info("  interlacing changed to %s",
			     pic_struct == MSM_VIDC_PIC_STRUCT_PROGRESSIVE ?
			     "progressive" :
			     pic_struct == MSM_VIDC_PIC_STRUCT_MAYBE_INTERLACED ?
			     "interlaced" : "??");
		}

		i->width = width;
		i->height = height;
		i->reconfigure_pending = 1;

		/* flush capture queue, we will reconfigure it when flush
		 * done event is received */
		video_flush(i, V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE);
		break;
	}
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT:
		dbg("Setting changed sufficient");
		break;
	case V4L2_EVENT_MSM_VIDC_FLUSH_DONE:
		dbg("Flush Done received");
		if (i->reconfigure_pending) {
			dbg("Reconfiguring output");
			restart_capture(i);
			i->reconfigure_pending = 0;
		}
		break;
	case V4L2_EVENT_MSM_VIDC_SYS_ERROR:
		dbg("SYS Error received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_OVERLOAD:
		dbg("HW Overload received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_UNSUPPORTED:
		dbg("HW Unsupported received");
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

static int
send_pkt(struct instance *i, int buf_index, AVPacket *pkt)
{
	struct video *vid = &i->video;
	struct timeval tv;
	int flags;

	memcpy(vid->out_buf_addr[buf_index], pkt->data, pkt->size);
	flags = 0;

	if (pkt->pts != AV_NOPTS_VALUE) {
		AVRational vid_timebase;
		AVRational v4l_timebase = { 1, 1000000 };
		int64_t v4l_pts;

		if (i->bsf)
			vid_timebase = i->bsf->time_base_out;
		else
			vid_timebase = i->stream->time_base;

		v4l_pts = av_rescale_q(pkt->pts, vid_timebase, v4l_timebase);
		tv.tv_sec = v4l_pts / 1000000;
		tv.tv_usec = v4l_pts % 1000000;
	} else {
		flags |= V4L2_QCOM_BUF_TIMESTAMP_INVALID;
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	if (video_queue_buf_out(i, buf_index, pkt->size, flags, tv) < 0)
		return -1;

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
	struct video *vid = &i->video;
	int n = fb->index;

	if (fb->group != i->group) {
		fb_destroy(fb);
		return;
	}

	if (video_queue_buf_cap(i, n) == 0)
		vid->cap_buf_flag[n] = 1;
}

static int
handle_video_capture(struct instance *i)
{
	struct video *vid = &i->video;
	struct timeval tv;
	unsigned int bytesused;
	int ret, n, finished;

	/* capture buffer is ready */

	ret = video_dequeue_capture(i, &n, &finished,
				    &bytesused, &tv);
	if (ret < 0) {
		err("dequeue capture buffer fail");
		return ret;
	}

	vid->cap_buf_flag[n] = 0;

	if (bytesused > 0) {
		info("decoded frame %ld with ts %ld.%03lu",
		     vid->total_captured, tv.tv_sec, tv.tv_usec / 1000);

		vid->total_captured++;

		save_frame(i, (void *)vid->cap_buf_addr[n][0],
			   bytesused);

		window_show_buffer(i->window, i->disp_buffers[n],
				   buffer_released, i);
	}

	if (finished) {
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

void main_loop(struct instance *i)
{
	struct video *vid = &i->video;
	struct wl_display *wl_display;
	struct pollfd pfd[3];
	short revents;
	int nfds;
	int ret;

	dbg("main thread started");

	pfd[0].fd = vid->fd;
	pfd[0].events = POLLOUT | POLLWRNORM | POLLPRI;

	wl_display = display_get_wl_display(i->display);
	pfd[1].fd = wl_display_get_fd(wl_display);
	pfd[1].events = POLLIN;

	nfds = 2;

	if (i->sigfd != -1) {
		pfd[nfds].fd = i->sigfd;
		pfd[nfds].events = POLLIN;
		nfds++;
	}

	while (!i->finish && display_is_running(i->display)) {

		while (wl_display_prepare_read(wl_display) != 0)
			wl_display_dispatch_pending(wl_display);

		ret = wl_display_flush(wl_display);
		if (ret < 0) {
			if (errno == EAGAIN)
				pfd[1].events |= POLLOUT;
			else if (errno != EPIPE) {
				err("wl_display_flush: %m");
				wl_display_cancel_read(wl_display);
				break;
			}
		}

		ret = poll(pfd, nfds, -1);
		if (ret <= 0) {
			err("poll error");
			break;
		}

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

		if (i->paused)
			pfd[0].events &= ~(POLLIN | POLLRDNORM);
		else
			pfd[0].events |= POLLIN | POLLRDNORM;

		for (int idx = 0; idx < nfds; idx++) {
			revents = pfd[idx].revents;
			if (!revents)
				continue;

			switch (idx) {
			case 0:
				if (revents & (POLLIN | POLLRDNORM))
					handle_video_capture(i);
				if (revents & (POLLOUT | POLLWRNORM))
					handle_video_output(i);
				if (revents & POLLPRI)
					handle_video_event(i);
				break;
			case 1:
				if (revents & POLLOUT)
					pfd[1].events &= ~POLLOUT;
				break;
			case 2:
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

	case KEY_F:
		window_toggle_fullscreen(i->window);
		break;
	}
}

static int
setup_display(struct instance *i)
{
	i->display = display_create();
	if (!i->display)
		return -1;

	i->window = display_create_window(i->display);
	if (!i->window)
		return -1;

	window_set_user_data(i->window, i);
	window_set_key_callback(i->window, handle_window_key);

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
stream_open(struct instance *i)
{
	const AVBitStreamFilter *filter;
	AVCodecParameters *codecpar;
	int codec;
	int ret;

	av_register_all();
	avformat_network_init();

	ret = avformat_open_input(&i->avctx, i->url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", i->url);
		goto fail;
	}

	ret = av_find_best_stream(i->avctx, AVMEDIA_TYPE_VIDEO, -1, -1,
				  NULL, 0);
	if (ret < 0) {
		av_err(ret, "stream does not seem to contain video");
		goto fail;
	}

	i->stream = i->avctx->streams[ret];
	codecpar = i->stream->codecpar;

	if (codecpar->width == 0 || codecpar->height == 0) {
		ret = avformat_find_stream_info(i->avctx, NULL);
		if (ret < 0) {
			av_err(ret, "failed to get streams info");
			goto fail;
		}
	}

	i->width = codecpar->width;
	i->height = codecpar->height;

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
		codec = V4L2_PIX_FMT_VC1_ANNEX_L;
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

	av_dump_format(i->avctx, i->stream->index, i->url, 0);

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

	ret = stream_open(&inst);
	if (ret)
		goto err;

	ret = video_open(&inst, inst.video.name);
	if (ret)
		goto err;

	ret = subscribe_events(&inst);
	if (ret)
		goto err;

	ret = video_setup_output(&inst, inst.fourcc,
				 STREAM_BUUFER_SIZE, 6);
	if (ret)
		goto err;

	ret = setup_display(&inst);
	if (ret)
		goto err;

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

