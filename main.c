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
#include <linux/videodev2.h>
#include <media/msm-v4l2-controls.h>
#include <sys/ioctl.h>
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

static int handle_v4l_events(struct video *vid)
{
	struct v4l2_event event;
	int ret;

	memset(&event, 0, sizeof(event));
	ret = ioctl(vid->fd, VIDIOC_DQEVENT, &event);
	if (ret < 0) {
		err("vidioc_dqevent failed (%s) %d", strerror(errno), -errno);
		return -errno;
	}

	switch (event.type) {
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT:
		dbg("Port Reconfig recieved insufficient\n");
		break;
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT:
		dbg("Setting changed sufficient\n");
		break;
	case V4L2_EVENT_MSM_VIDC_FLUSH_DONE:
		dbg("Flush Done Recieved \n");
		break;
	case V4L2_EVENT_MSM_VIDC_CLOSE_DONE:
		dbg("Close Done Recieved \n");
		break;
	case V4L2_EVENT_MSM_VIDC_SYS_ERROR:
		dbg("SYS Error Recieved \n");
		break;
	default:
		dbg("unknown event type occurred %x\n", event.type);
		break;
	}

	return 0;
}

void cleanup(struct instance *i)
{
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

int save_frame(const void *buf, unsigned int size)
{
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	char filename[64];
	int fd;
	int ret;
	static unsigned int frame_num = 0;

	ret = sprintf(filename, "/mnt/frame%04d.nv12", frame_num);
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

	while (!i->error && !i->finish && !i->parser.finished) {
		n = 0;
		pthread_mutex_lock(&i->lock);
		while (n < vid->out_buf_cnt && vid->out_buf_flag[n])
			n++;
		pthread_mutex_unlock(&i->lock);

		if (n < vid->out_buf_cnt && !i->parser.finished) {

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
	}

	dbg("Parser thread finished");

	return NULL;
}

void *main_thread_func(void *args)
{
	struct instance *i = (struct instance *)args;
	struct video *vid = &i->video;
	struct pollfd pfd;
	short revents;
	int ret, n, finished;

	dbg("main thread started");

	pfd.fd = vid->fd;
	pfd.events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM |
		     POLLRDBAND | POLLPRI;

	while (1) {
		ret = poll(&pfd, 1, 10000);
		if (!ret) {
			err("poll timeout");
			break;
		} else if (ret < 0) {
			err("poll error");
			break;
		}

		revents = pfd.revents;

		if (revents & (POLLIN | POLLRDNORM)) {
			unsigned int bytesused;

			/* capture buffer is ready */

			ret = video_dequeue_capture(i, &n, &finished,
						    &bytesused);
			if (ret < 0)
				goto next_event;

			vid->cap_buf_flag[n] = 0;

			info("decoded frame %ld", vid->total_captured);

			if (finished)
				break;

			vid->total_captured++;

			if (i->save_frames)
				save_frame((void *)vid->cap_buf_addr[n][0],
					   bytesused);

			ret = video_queue_buf_cap(i, n);
			if (!ret)
				vid->cap_buf_flag[n] = 1;
		}

next_event:
		if (revents & (POLLOUT | POLLWRNORM)) {

			ret = video_dequeue_output(i, &n);
			if (ret < 0) {
				err("dequeue output buffer fail");
			} else {
				pthread_mutex_lock(&i->lock);
				vid->out_buf_flag[n] = 0;
				pthread_mutex_unlock(&i->lock);
			}

			dbg("dequeued output buffer %d", n);
		}

		if (revents & POLLPRI) {
			dbg("v4l2 event");
			handle_v4l_events(vid);
		}
	}

	dbg("main thread finished");

	return NULL;
}

int main(int argc, char **argv)
{
	struct instance inst;
	struct video *vid = &inst.video;
	pthread_t parser_thread;
	pthread_t main_thread;
	int ret, n;

	ret = parse_args(&inst, argc, argv);
	if (ret) {
		print_usage(argv[0]);
		return 1;
	}

	info("decoding resolution is %dx%d", inst.width, inst.height);

	pthread_mutex_init(&inst.lock, 0);

	vid->total_captured = 0;

	ret = input_open(&inst, inst.in.name);
	if (ret)
		goto err;

	ret = video_open(&inst, inst.video.name);
	if (ret)
		goto err;
#if 0
	/* TODO: */
	ret = subscribe_for_events(vid->fd);
	if (ret)
		goto err;
#endif
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

	if (pthread_create(&parser_thread, NULL, parser_thread_func, &inst))
		goto err;

	if (pthread_create(&main_thread, NULL, main_thread_func, &inst))
		goto err;

	pthread_join(parser_thread, 0);
	pthread_join(main_thread, 0);

	dbg("Threads have finished");

	video_stop(&inst);

	cleanup(&inst);

	pthread_mutex_destroy(&inst.lock);

	info("Total frames captured %ld", vid->total_captured);

	return 0;
err:
	cleanup(&inst);
	return 1;
}

