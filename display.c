#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <wayland-client.h>

#include "common.h"
#include "xdg-shell-unstable-v5-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_shell *shell;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	uint32_t drm_formats[32];
	int drm_format_count;
	int running;
};

struct fb {
	struct window *window;
	int fd;
	int offset;
	int width;
	int height;
	int stride;
	uint32_t format;
	struct wl_buffer *buffer;
};

struct window {
	struct display *display;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
};

void
fb_destroy(struct fb *fb)
{
	if (fb->buffer)
		wl_buffer_destroy(fb->buffer);
	free(fb);
}

static void
handle_configure(void *data, struct xdg_surface *surface,
		 int32_t width, int32_t height,
		 struct wl_array *states, uint32_t serial)
{
}

static void
handle_close(void *data, struct xdg_surface *xdg_surface)
{
	struct display *d = data;

	d->running = 0;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	handle_configure,
	handle_close,
};

struct window *
display_create_window(struct display *display)
{
	struct window *window;

	window = calloc(1, sizeof *window);
	if (!window)
		return NULL;

	window->display = display;
	window->surface = wl_compositor_create_surface(display->compositor);

	if (display->shell) {
		window->xdg_surface =
			xdg_shell_get_xdg_surface(display->shell,
			                          window->surface);

		xdg_surface_add_listener(window->xdg_surface,
		                         &xdg_surface_listener, window);

		xdg_surface_set_title(window->xdg_surface, "v4l-decode");
	}

	return window;
}

void
window_destroy(struct window *window)
{
	if (window->xdg_surface)
		xdg_surface_destroy(window->xdg_surface);

	wl_surface_destroy(window->surface);

	free(window);
}

static void
create_succeeded(void *data,
		 struct zwp_linux_buffer_params_v1 *params,
		 struct wl_buffer *new_buffer)
{
	struct fb *fb = data;

	fb->buffer = new_buffer;

	zwp_linux_buffer_params_v1_destroy(params);
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	struct fb *fb = data;

	fb->buffer = NULL;

	zwp_linux_buffer_params_v1_destroy(params);

	err("zwp_linux_buffer_params.create failed");

	fb->window->display->running = 0;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	create_succeeded,
	create_failed
};

static int
format_is_supported(struct display *display, uint32_t format)
{
	int i;

	for (i = 0; i < display->drm_format_count; i++) {
		if (display->drm_formats[i] == format)
			return 1;
	}

	return 0;
}

struct fb *
window_create_buffer(struct window *window, int fd, int offset,
		     uint32_t format, int width, int height, int stride)
{
	struct zwp_linux_buffer_params_v1 *params;
	struct fb *fb;

#if 0
	if (!format_is_supported(window->display, format)) {
		err("unsupported display format");
		return NULL;
	}
#endif

	fb = calloc(1, sizeof *fb);
	fb->fd = fd;
	fb->offset = offset;
	fb->format = format;
	fb->width = width;
	fb->height = height;
	fb->stride = stride;
	fb->window = window;

	params = zwp_linux_dmabuf_v1_create_params(window->display->dmabuf);
	zwp_linux_buffer_params_v1_add(params, fb->fd, 0, fb->offset,
				       fb->stride, 0, 0);
	zwp_linux_buffer_params_v1_add_listener(params, &params_listener, fb);
	zwp_linux_buffer_params_v1_create(params, fb->width, fb->height,
					  fb->format, 0);

	wl_display_roundtrip(window->display->display);

	if (!fb->buffer) {
		fb_destroy(fb);
		return NULL;
	}

	return fb;
}

void
window_show_buffer(struct window *window, struct fb *fb)
{
	wl_surface_attach(window->surface, fb->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, fb->width, fb->height);
	wl_surface_commit(window->surface);
	wl_display_roundtrip(window->display->display);
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
              uint32_t format)
{
	struct display *d = data;

	assert(d->drm_format_count <= 32);
	d->drm_formats[d->drm_format_count++] = format;
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format
};

static void
xdg_shell_ping(void *data, struct xdg_shell *shell, uint32_t serial)
{
	xdg_shell_pong(shell, serial);
}

static const struct xdg_shell_listener xdg_shell_listener = {
	xdg_shell_ping,
};

#define XDG_VERSION 5
#ifdef static_assert
static_assert(XDG_VERSION == XDG_SHELL_VERSION_CURRENT,
	      "Interface version doesn't match implementation version");
#endif

static void
registry_handle_global(void *data, struct wl_registry *registry,
                       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (!strcmp(interface, "wl_compositor")) {
		d->compositor = wl_registry_bind(registry, id,
						 &wl_compositor_interface, 1);
	} else if (!strcmp(interface, "xdg_shell")) {
		d->shell = wl_registry_bind(registry, id,
					    &xdg_shell_interface, 1);
		xdg_shell_use_unstable_version(d->shell, XDG_VERSION);
		xdg_shell_add_listener(d->shell, &xdg_shell_listener, d);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		d->dmabuf = wl_registry_bind(registry, id,
		                             &zwp_linux_dmabuf_v1_interface, 1);
		zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener,
		                                 d);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

void
display_destroy(struct display *display)
{
	if (display->compositor)
		wl_compositor_destroy(display->compositor);
	if (display->shell)
		xdg_shell_destroy(display->shell);
	if (display->dmabuf)
		zwp_linux_dmabuf_v1_destroy(display->dmabuf);
	if (display->registry)
		wl_registry_destroy(display->registry);
	if (display->display)
		wl_display_disconnect(display->display);
	free(display);
}

struct display *
display_create(void)
{
	struct display *display;

	display = calloc(1, sizeof *display);
	if (!display)
		return NULL;

	display->display = wl_display_connect(NULL);
	if (!display->display) {
		err("failed to connect to wayland display: %m");
		goto fail;
	}

	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry, &registry_listener,
				 display);

	wl_display_roundtrip(display->display);
	if (!display->shell || !display->dmabuf) {
		err("missing wayland globals");
		goto fail;
	}

	display->running = 1;

	return display;

fail:
	display_destroy(display);
	return NULL;
}

int
display_is_running(struct display *display)
{
	return display->running;
}
