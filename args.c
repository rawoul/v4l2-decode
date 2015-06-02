/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Argument parser
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
#include <unistd.h>
#include <stdlib.h>
#include <linux/videodev2.h>

#include "common.h"
#include "parser.h"


void print_usage(char *name)
{
	printf("Usage:\n");
	printf("\t%s\n", name);
	printf("\t-c <codec> - The codec of the encoded stream\n");
	printf("\t\t     Available codecs: mpeg4, h264\n");
	printf("\t-d <device>  - Frame buffer device (e.g. /dev/fb0)\n");
	printf("\t-i <file> - Input file name\n");
	printf("\t-m <device> - video decoder device (e.g. /dev/video8)\n");
	printf("\t-w video width\n");
	printf("\t-h video height\n");
	printf("\t-f save frames on disk\n");

	printf("\n");
}

void init_to_defaults(struct instance *i)
{
	memset(i, 0, sizeof(*i));
}

int get_codec(char *str)
{
	if (strncasecmp("mpeg4", str, 5) == 0) {
		return V4L2_PIX_FMT_MPEG4;
	} else if (strncasecmp("h264", str, 5) == 0) {
		return V4L2_PIX_FMT_H264;
	} else if (strncasecmp("h263", str, 5) == 0) {
		return V4L2_PIX_FMT_H263;
	} else if (strncasecmp("xvid", str, 5) == 0) {
		return V4L2_PIX_FMT_XVID;
	} else if (strncasecmp("mpeg2", str, 5) == 0) {
		return V4L2_PIX_FMT_MPEG2;
	} else if (strncasecmp("mpeg1", str, 5) == 0) {
		return V4L2_PIX_FMT_MPEG1;
	}
	return 0;
}

int parse_args(struct instance *i, int argc, char **argv)
{
	int c;

	init_to_defaults(i);

	while ((c = getopt(argc, argv, "w:h:c:d:i:m:f:")) != -1) {
		switch (c) {
		case 'c':
			i->parser.codec = get_codec(optarg);
			break;
		case 'd':
			i->fb.name = optarg;
			break;
		case 'i':
			i->in.name = optarg;
			break;
		case 'm':
			i->video.name = optarg;
			break;
		case 'w':
			i->width = atoi(optarg);
			break;
		case 'h':
			i->height = atoi(optarg);
			break;
		case 'f':
			i->save_frames = 1;
			i->save_path = optarg;
			break;
		default:
			err("Bad argument");
			return -1;
		}
	}

	if (!i->in.name || !i->video.name) {
		err("The following arguments are required: -i -m -c");
		return -1;
	}

	if (!i->parser.codec) {
		err("Unknown or not set codec (-c)");
		return -1;
	}

	switch (i->parser.codec) {
	case V4L2_PIX_FMT_XVID:
	case V4L2_PIX_FMT_H263:
	case V4L2_PIX_FMT_MPEG4:
		i->parser.func = parse_mpeg4_stream;
		break;
	case V4L2_PIX_FMT_H264:
		i->parser.func = parse_h264_stream;
		break;
	case V4L2_PIX_FMT_MPEG1:
	case V4L2_PIX_FMT_MPEG2:
		i->parser.func = parse_mpeg2_stream;
		break;
	}

	return 0;
}

