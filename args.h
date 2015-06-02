/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Argument parser header file
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
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
#ifndef INCLUDE_ARGS_H
#define INCLUDE_ARGS_H

#include "common.h"

/* Pritn usage information of the application */
void print_usage(char *name);
/* Parse the arguments that have been given to the application */
int parse_args(struct instance *i, int argc, char **argv);

#endif /* INCLUDE_FILEOPS_H */

