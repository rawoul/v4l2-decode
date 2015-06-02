/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Queue handling header file
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

#ifndef INCLUDE_QUEUE_H
#define INCLUDE_QUEUE_H

#include <pthread.h>

struct queue {
	int size;
	int head;
	int tail;
	int n;
	int *q;
	pthread_mutex_t mutex;
};

/* Initialize queue and allocate memory */
int queue_init(struct queue *q, int size);
/* Add an element to the queue */
int queue_add(struct queue *q, int e);
/* Remove the element form queue */
int queue_remove(struct queue *q);
/* Free the internal queue memory */
void queue_free(struct queue *q);
/* Check if the queue is empty */
int queue_empty(struct queue *q);

#endif /* INCLUDE_QUEUE_H */

