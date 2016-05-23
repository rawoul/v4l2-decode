# V4L2 Codec decoding example application
# Kamil Debski <k.debski@samsung.com>
#
# Copyright 2012 Samsung Electronics Co., Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Toolchain path
CROSS ?= aarch64-linux-gnu-

CC = $(CROSS)gcc
AR = $(CROSS)ar rc
PKG_CONFIG ?= pkg-config

WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS_DATADIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

GENERATED_SOURCES = \
  protocol/xdg-shell-unstable-v5-protocol.c \
  protocol/xdg-shell-unstable-v5-client-protocol.h \
  protocol/linux-dmabuf-unstable-v1-protocol.c \
  protocol/linux-dmabuf-unstable-v1-client-protocol.h

SOURCES = main.c fileops.c args.c parser.c video.c queue.c display.c $(filter %.c,$(GENERATED_SOURCES))
OBJECTS := $(SOURCES:.c=.o)
EXEC = v4l2_decode

cflags = -Wall -pthread $(CFLAGS)
ldflags = -pthread $(LDFLAGS)
cppflags = -Iprotocol $(CPPFLAGS)
ldlibs = -lm -lwayland-client

all: $(EXEC)

%.o: %.c
	$(CC) -c $(cflags) -o $@ -MD -MP -MF $(@D)/.$(@F).d $(cppflags) $<

$(EXEC): $(GENERATED_SOURCES) $(OBJECTS)
	$(CC) $(ldflags) -o $(EXEC) $(OBJECTS) $(ldlibs)

clean:
	$(RM) *.o $(EXEC) $(GENERATED_SOURCES)

install:

.PHONY: clean all install

-include $(patsubst %,.%.d,$(OBJECTS))

.SECONDEXPANSION:

define protostability
$(if $(findstring unstable,$1),unstable,stable)
endef

define protoname
$(shell echo $1 | sed 's/\([a-z\-]\+\)-[a-z]\+-v[0-9]\+/\1/')
endef

protocol/%-protocol.c : $(WAYLAND_PROTOCOLS_DATADIR)/$$(call protostability,$$*)/$$(call protoname,$$*)/$$*.xml
	mkdir -p $(@D) && $(WAYLAND_SCANNER) code < $< > $@

protocol/%-server-protocol.h : $(WAYLAND_PROTOCOLS_DATADIR)/$$(call protostability,$$*)/$$(call protoname,$$*)/$$*.xml
	mkdir -p $(@D) && $(WAYLAND_SCANNER) server-header < $< > $@

protocol/%-client-protocol.h : $(WAYLAND_PROTOCOLS_DATADIR)/$$(call protostability,$$*)/$$(call protoname,$$*)/$$*.xml
	mkdir -p $(@D) && $(WAYLAND_SCANNER) client-header < $< > $@

protocol/%-protocol.c : $(top_srcdir)/protocol/%.xml
	mkdir -p $(@D) && $(WAYLAND_SCANNER) code < $< > $@

protocol/%-server-protocol.h : $(top_srcdir)/protocol/%.xml
	mkdir -p $(@D) && $(WAYLAND_SCANNER) server-header < $< > $@

protocol/%-client-protocol.h : $(top_srcdir)/protocol/%.xml
	mkdir -p $(@D) && $(WAYLAND_SCANNER) client-header < $< > $@
