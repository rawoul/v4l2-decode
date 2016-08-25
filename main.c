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
#include <semaphore.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "args.h"
#include "common.h"
#include "fileops.h"
#include "video.h"
#include "display.h"
#include "parser.h"

/* This is the size of the buffer for the compressed stream.
 * It limits the maximum compressed frame size. */
#define STREAM_BUUFER_SIZE	(1024 * 1024)

/* The number of compress4ed stream buffers */
#define STREAM_BUFFER_CNT	2

/* The number of extra buffers for the decoded output.
 * This is the number of buffers that the application can keep
 * used and still enable video device to decode with the hardware. */
#define RESULT_EXTRA_BUFFER_CNT 2

#define V4L2_EVENT_MSM_VIDC_CLOSE_DONE      (V4L2_EVENT_MSM_VIDC_START + 4)

static const int event_type[] = {
	V4L2_EVENT_MSM_VIDC_FLUSH_DONE,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT,
	V4L2_EVENT_MSM_VIDC_CLOSE_DONE,
	V4L2_EVENT_MSM_VIDC_SYS_ERROR
};

static int subscribe_for_events(int fd)
{
	int size_event = sizeof(event_type) / sizeof(event_type[0]);
	struct v4l2_event_subscription sub;
	int i, ret;

	for (i = 0; i < size_event; i++) {
		memset(&sub, 0, sizeof(sub));
		sub.type = event_type[i];
		ret = ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
		if (ret < 0)
			err("cannot subscribe for event type %d (%s)",
				sub.type, strerror(errno));
	}

	return 0;
}

static int handle_video_event(struct instance *i)
{
	struct video *vid = &i->video;
	struct v4l2_event event;
	int ret;

	memset(&event, 0, sizeof(event));
	ret = ioctl(vid->fd, VIDIOC_DQEVENT, &event);
	if (ret < 0) {
		err("vidioc_dqevent failed (%s) %d", strerror(errno), -errno);
		return -errno;
	}

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
		}

		if (ptr[2] & V4L2_EVENT_PICSTRUCT_FLAG) {
			unsigned int pic_struct = ptr[4];
			info("  interlacing changed to %s",
			     pic_struct == MSM_VIDC_PIC_STRUCT_PROGRESSIVE ?
			     "progressive" :
			     pic_struct == MSM_VIDC_PIC_STRUCT_MAYBE_INTERLACED ?
			     "interlaced" : "??");
		}
		break;
	}
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT:
		dbg("Setting changed sufficient");
		break;
	case V4L2_EVENT_MSM_VIDC_FLUSH_DONE:
		dbg("Flush Done received");
		break;
	case V4L2_EVENT_MSM_VIDC_CLOSE_DONE:
		dbg("Close Done received");
		break;
	case V4L2_EVENT_MSM_VIDC_SYS_ERROR:
		dbg("SYS Error received");
		break;
	default:
		dbg("unknown event type occurred %x", event.type);
		break;
	}

	return 0;
}

void cleanup(struct instance *i)
{
	if (i->window)
		window_destroy(i->window);
	if (i->display)
		display_destroy(i->display);
	if (i->sigfd != 1)
		close(i->sigfd);
	if (i->video.fd)
		video_close(i);
	if (i->in.fd)
		input_close(i);
}

int extract_and_process_header(struct instance *i)
{
	int used, fs;
	int ret;

	ret = i->parser.func(&i->parser.ctx,
			     i->in.p + i->in.offs,
			     i->in.size - i->in.offs,
			     i->video.out_buf_addr[0],
			     i->video.out_buf_size,
			     &used, &fs, 1);

	if (ret == 0) {
		err("Failed to extract header from stream");
		return -1;
	}

	/* For H263 the header is passed with the first frame, so we should
	 * pass it again */
	if (i->parser.codec != V4L2_PIX_FMT_H263)
		i->in.offs += used;
	else
	/* To do this we shall reset the stream parser to the initial
	 * configuration */
		parse_stream_init(&i->parser.ctx);

	dbg("Extracted header of size %d", fs);

	ret = video_queue_buf_out(i, 0, fs);
	if (ret)
		return -1;

	dbg("queued output buffer %d", 0);

	i->video.out_buf_flag[0] = 1;

	ret = video_stream(i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			   VIDIOC_STREAMON);
	if (ret)
		return -1;

	return 0;
}

int save_frame(struct instance *i, const void *buf, unsigned int size)
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

/* This threads is responsible for parsing the stream and
 * feeding video decoder with consecutive frames to decode */
void *parser_thread_func(void *args)
{
	struct instance *i = (struct instance *)args;
	struct video *vid = &i->video;
	int used, fs, n;
	int ret;

	dbg("Parser thread started");

	while (!i->finish && !i->parser.finished) {
		pthread_mutex_lock(&i->lock);

		for (n = 0; n < vid->out_buf_cnt && vid->out_buf_flag[n]; n++)
			;

		if (n == vid->out_buf_cnt) {
			pthread_cond_wait(&i->cond, &i->lock);
			pthread_mutex_unlock(&i->lock);
			continue;
		}

		pthread_mutex_unlock(&i->lock);

		ret = i->parser.func(&i->parser.ctx,
				     i->in.p + i->in.offs,
				     i->in.size - i->in.offs,
				     vid->out_buf_addr[n],
				     vid->out_buf_size,
				     &used, &fs, 0);

		if (ret == 0 && i->in.offs == i->in.size) {
			info("Parser has extracted all frames");
			i->parser.finished = 1;
			fs = 0;
		}

		dbg("Extracted frame of size %d", fs);

		ret = video_queue_buf_out(i, n, fs);

		pthread_mutex_lock(&i->lock);
		vid->out_buf_flag[n] = 1;
		pthread_mutex_unlock(&i->lock);

		dbg("queued output buffer %d", n);

		i->in.offs += used;
	}

	dbg("Parser thread finished");

	pthread_cond_signal(&i->cond);

	return NULL;
}

static void
buffer_released(struct fb *fb, void *data)
{
	struct instance *i = data;
	int n = fb->index;

	int ret = video_queue_buf_cap(i, n);
	if (!ret)
		i->video.cap_buf_flag[n] = 1;
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

	info("decoded frame %ld with ts %lu.%03lu",
	     vid->total_captured, tv.tv_sec, tv.tv_usec / 1000);

	vid->total_captured++;

	save_frame(i, (void *)vid->cap_buf_addr[n][0],
		   bytesused);

	window_show_buffer(i->window, i->disp_buffers[n],
			   buffer_released, i);

	if (finished) {
		i->finish = 1;
		pthread_cond_signal(&i->cond);
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

	dbg("dequeued output buffer %d", n);

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

	i->finish = 1;
	pthread_cond_signal(&i->cond);

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
	pfd[0].events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLPRI;

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

		ret = poll(pfd, nfds, -1);
		if (ret <= 0) {
			err("poll error");
			break;
		}

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

	if (!i->finish) {
		i->finish = 1;
		pthread_cond_signal(&i->cond);
	}

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
		i->finish = 1;
		pthread_cond_signal(&i->cond);
		break;

	case KEY_SPACE:
		info("%s", i->paused ? "Resume" : "Pause");
		i->paused = !i->paused;
		break;
	}
}

static int
setup_display(struct instance *i)
{
	struct video *vid = &i->video;
	int n;

	i->display = display_create();
	if (!i->display)
		return -1;

	i->window = display_create_window(i->display);
	if (!i->window)
		return -1;

	window_set_user_data(i->window, i);
	window_set_key_callback(i->window, handle_window_key);

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		i->disp_buffers[n] =
			window_create_buffer(i->window, n, vid->cap_ion_fd,
					     vid->cap_buf_off[n][0],
					     vid->cap_buf_format,
					     vid->cap_w, vid->cap_h,
					     vid->cap_buf_stride[0]);

		if (!i->disp_buffers[n])
			return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct instance inst;
	struct video *vid = &inst.video;
	pthread_t parser_thread;
	int ret, n;

	inst.sigfd = -1;

	ret = parse_args(&inst, argc, argv);
	if (ret) {
		print_usage(argv[0]);
		return 1;
	}

	info("decoding resolution is %dx%d", inst.width, inst.height);

	pthread_mutex_init(&inst.lock, 0);
	pthread_cond_init(&inst.cond, 0);

	vid->total_captured = 0;

	ret = input_open(&inst, inst.in.name);
	if (ret)
		goto err;

	ret = video_open(&inst, inst.video.name);
	if (ret)
		goto err;

	ret = subscribe_for_events(vid->fd);
	if (ret)
		goto err;

	ret = video_setup_output(&inst, inst.parser.codec,
				 STREAM_BUUFER_SIZE, 6);
	if (ret)
		goto err;

	ret = parse_stream_init(&inst.parser.ctx);
	if (ret)
		goto err;

	ret = video_setup_capture(&inst, 20, inst.width, inst.height);
	if (ret)
		goto err;

	ret = setup_display(&inst);
	if (ret)
		goto err;

	ret = video_set_control(&inst);
	if (ret)
		goto err;

	ret = extract_and_process_header(&inst);
	if (ret)
		goto err;

	/* queue all capture buffers */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		ret = video_queue_buf_cap(&inst, n);
		if (ret)
			goto err;
		vid->cap_buf_flag[n] = 1;
	}

	ret = video_stream(&inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			   VIDIOC_STREAMON);
	if (ret)
		goto err;

	dbg("Launching threads");

	setup_signal(&inst);

	if (pthread_create(&parser_thread, NULL, parser_thread_func, &inst))
		goto err;

	main_loop(&inst);

	pthread_join(parser_thread, 0);

	dbg("Threads have finished");

	video_stop(&inst);

	cleanup(&inst);

	pthread_cond_destroy(&inst.cond);
	pthread_mutex_destroy(&inst.lock);

	info("Total frames captured %ld", vid->total_captured);

	return 0;
err:
	cleanup(&inst);
	return 1;
}

