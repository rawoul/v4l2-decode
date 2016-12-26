/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Common stuff header file
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

#ifndef INCLUDE_COMMON_H
#define INCLUDE_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "display.h"
#include "list.h"

extern int debug_level;

#define ARRAY_LENGTH(x) (sizeof (x) / sizeof (*(x)))

#define print(l, msg, ...)						\
	do {								\
		if (debug_level >= l)					\
			fprintf(stderr, msg, ##__VA_ARGS__);		\
	} while (0)

#define err(msg, ...) \
	print(1, "error: " msg "\n", ##__VA_ARGS__)

#define info(msg, ...) \
	print(2, msg "\n", ##__VA_ARGS__)

#define dbg(msg, ...) \
	print(3, DBG_TAG ": " msg "\n", ##__VA_ARGS__)

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define memzero(x)	memset(&(x), 0, sizeof (x));

/* Maximum number of output buffers */
#define MAX_OUT_BUF		16

/* Maximum number of capture buffers (32 is the limit imposed by MFC */
#define MAX_CAP_BUF		32

/* Number of output planes */
#define OUT_PLANES		1

/* Number of capture planes */
#define CAP_PLANES		2

/* Maximum number of planes used in the application */
#define MAX_PLANES		CAP_PLANES

/* video decoder related parameters */
struct video {
	char *name;
	int fd;

	/* Output queue related */
	int out_buf_cnt;
	int out_buf_size;
	int out_buf_off[MAX_OUT_BUF];
	char *out_buf_addr[MAX_OUT_BUF];
	int out_buf_flag[MAX_OUT_BUF];
	int out_ion_fd;
	int out_ion_size;
	void *out_ion_addr;

	/* Capture queue related */
	int cap_w;
	int cap_h;
	int cap_buf_cnt;
	uint32_t cap_buf_format;
	int cap_buf_size[CAP_PLANES];
	int cap_buf_stride[CAP_PLANES];
	int cap_buf_off[MAX_CAP_BUF][CAP_PLANES];
	char *cap_buf_addr[MAX_CAP_BUF][CAP_PLANES];
	int cap_buf_flag[MAX_CAP_BUF];
	int cap_ion_fd;
	int cap_ion_size;
	void *cap_ion_addr;

	/* timestamp list for all pending frames */
	struct list_head pending_ts_list;
	uint64_t cap_last_pts;
	uint64_t pts_dts_delta;

	unsigned long total_captured;
};

struct instance {
	int width;
	int height;
	int fullscreen;
	uint32_t fourcc;
	int fps_n, fps_d;
	int depth;
	int interlaced;
	int save_frames;
	int decode_order;
	int skip_frames;
	int insert_sc;
	int need_header;
	int secure;
	char *save_path;
	char *url;

	/* video decoder related parameters */
	struct video	video;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	/* Control */
	int sigfd;
	int paused;
	int prerolled;
	int finish;  /* Flag set when decoding has been completed and all
			threads finish */

	int reconfigure_pending;
	int group;

	struct display *display;
	struct window *window;
	struct fb *disp_buffers[MAX_CAP_BUF];

	AVFormatContext *avctx;
	AVStream *stream;
	AVBSFContext *bsf;
	int bsf_data_pending;
};

#endif /* INCLUDE_COMMON_H */

