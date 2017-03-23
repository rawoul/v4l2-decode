#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <wayland-client.h>

#include "common.h"
#include "scaler-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-unstable-v6-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "linux-dmabuf-client-protocol.h"

#define DBG_TAG "  disp"

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_shell *wl_shell;
	struct zxdg_shell_v6 *xdg_shell;
	struct wl_scaler *scaler;
	struct wp_viewporter *viewporter;
	struct wp_presentation *presentation;
	struct zlinux_dmabuf *dmabuf_legacy;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	uint32_t drm_formats[32];
	int compositor_version;
	int seat_version;
	int drm_format_count;
	int running;

	struct window *keyboard_focus;
	struct wl_list window_list;
};

struct window {
	struct wl_list link;

	struct display *display;
	struct wl_surface *surface;
	struct wl_viewport *legacy_viewport;
	struct wp_viewport *viewport;
	struct wl_shell_surface *shell_surface;
	struct zxdg_surface_v6 *xdg_surface;
	struct zxdg_toplevel_v6 *xdg_toplevel;
	struct fb *buffer;
	int width, height;
	int saved_width, saved_height;
	int ar_x, ar_y;
	bool size_set;
	bool saved_size_set;
	bool configured;
	bool fullscreen;

	window_key_cb_t key_cb;
	void *user_data;
};

void
fb_destroy(struct fb *fb)
{
	if (fb->sync_callback)
		wl_callback_destroy(fb->sync_callback);
	if (fb->presentation_feedback)
		wp_presentation_feedback_destroy(fb->presentation_feedback);
	if (fb->buffer)
		wl_buffer_destroy(fb->buffer);
	free(fb);
}

void
window_set_user_data(struct window *w, void *data)
{
	w->user_data = data;
}

void *
window_get_user_data(struct window *w)
{
	return w->user_data;
}

void
window_set_key_callback(struct window *w, window_key_cb_t callback)
{
	w->key_cb = callback;
}

static void
handle_sync_output(void *data, struct wp_presentation_feedback *feedback,
		   struct wl_output *output)
{
}

static void
handle_presented(void *data, struct wp_presentation_feedback *feedback,
		 uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
		 uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
		 uint32_t flags)
{
	struct fb *fb = data;
	uint64_t tv_sec = (uint64_t)tv_sec_hi << 32 | tv_sec_lo;

	dbg("buffer %d displayed at %lu.%04u, %u.%04us till next refresh",
	    fb->index, tv_sec, tv_nsec / 1000000, refresh / 1000000000,
	    refresh / 1000000);

	wp_presentation_feedback_destroy(feedback);
	fb->presentation_feedback = NULL;
}

static void
handle_discarded(void *data, struct wp_presentation_feedback *feedback)
{
	struct fb *fb = data;

	dbg("buffer %d discarded", fb->index);

	wp_presentation_feedback_destroy(feedback);
	fb->presentation_feedback = NULL;
}

static const struct wp_presentation_feedback_listener presentation_feedback_listener = {
	handle_sync_output,
	handle_presented,
	handle_discarded,
};

static void
sync_callback(void * data, struct wl_callback * callback, uint32_t time)
{
	struct fb *fb = data;

	wl_callback_destroy(callback);
	if (fb)
		fb->sync_callback = NULL;
}

static const struct wl_callback_listener sync_listener = {
	sync_callback
};

static void
window_commit(struct window *w)
{
	struct display *display = w->display;
	struct fb *fb = w->buffer;
	struct wl_region *region;

	region = wl_compositor_create_region(display->compositor);
	wl_region_add(region, 0, 0, INT_MAX, INT_MAX);
	wl_surface_set_opaque_region(w->surface, region);
	wl_region_destroy(region);

	wl_surface_attach(w->surface, fb ? fb->buffer : NULL, 0, 0);
	wl_surface_damage(w->surface, 0, 0, INT_MAX, INT_MAX);

	if (fb && display->presentation) {
		if (fb->presentation_feedback)
			wp_presentation_feedback_destroy(fb->presentation_feedback);

		fb->presentation_feedback =
			wp_presentation_feedback(display->presentation,
						 w->surface);

		wp_presentation_feedback_add_listener(
			fb->presentation_feedback,
			&presentation_feedback_listener, fb);
	} else if (fb) {
		if (fb->sync_callback)
			wl_callback_destroy(fb->sync_callback);
		fb->sync_callback = wl_display_sync(display->display);
		wl_callback_add_listener(fb->sync_callback, &sync_listener, fb);
	}

	if (fb)
		fb->busy = 1;

	wl_surface_commit(w->surface);
}

static int
window_recenter(struct window *w)
{
	struct fb *fb = w->buffer;
	int video_w, video_h;
	int output_w, output_h;
	int ar_x, ar_y;
	int src_x, src_y, src_w, src_h;

	if (!fb || (!w->viewport && !w->legacy_viewport))
		return 0;

	if (fb->crop_w != 0 && fb->crop_h != 0) {
		src_x = fb->crop_x;
		src_y = fb->crop_y;
		src_w = fb->crop_w;
		src_h = fb->crop_h;
	} else {
		src_x = 0;
		src_y = 0;
		src_w = fb->width;
		src_h = fb->height;
	}

	ar_x = w->ar_x * fb->ar_x;
	ar_y = w->ar_y * fb->ar_y;

	if (src_w * ar_y > src_h * ar_x) {
		video_w = src_w * ar_x / ar_y;
		video_h = src_h;
	} else {
		video_w = src_w;
		video_h = src_h * ar_y / ar_x;
	}

	if (!w->size_set) {
		output_w = video_w;
		output_h = video_h;
	} else if (video_w * w->height > video_h * w->width) {
		output_w = w->width;
		output_h = w->width * video_h / video_w;
	} else {
		output_w = w->height * video_w / video_h;
		output_h = w->height;
	}

	if (w->viewport) {
		wp_viewport_set_destination(w->viewport, output_w, output_h);
		wp_viewport_set_source(w->viewport,
				       wl_fixed_from_int(src_x),
				       wl_fixed_from_int(src_y),
				       wl_fixed_from_int(src_w),
				       wl_fixed_from_int(src_h));
	} else {
		wl_viewport_set(w->legacy_viewport,
				wl_fixed_from_int(src_x),
				wl_fixed_from_int(src_y),
				wl_fixed_from_int(src_w),
				wl_fixed_from_int(src_h),
				output_w, output_h);
	}

	return 1;
}

void
window_set_aspect_ratio(struct window *w, int ar_x, int ar_y)
{
	if (ar_x == 0 || ar_y == 0)
		return;

	w->ar_x = ar_x;
	w->ar_y = ar_y;

	if (w->configured) {
		window_recenter(w);
		window_commit(w);
	}
}

void
window_toggle_fullscreen(struct window *w)
{
	if (w->xdg_toplevel) {
		if (w->fullscreen)
			zxdg_toplevel_v6_unset_fullscreen(w->xdg_toplevel);
		else
			zxdg_toplevel_v6_set_fullscreen(w->xdg_toplevel, NULL);
	} else if (w->shell_surface) {
		if (w->fullscreen)
			wl_shell_surface_set_toplevel(w->shell_surface);
		else
			wl_shell_surface_set_fullscreen(w->shell_surface,
							WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
							0, NULL);
	}
}

static void
xdg_toplevel_handle_configure(void *data, struct zxdg_toplevel_v6 *xdg_toplevel,
			      int32_t width, int32_t height,
			      struct wl_array *states)
{
	struct window *w = data;
	uint32_t *state_p;
	bool fullscreen = false;

	wl_array_for_each(state_p, states) {
		switch (*state_p) {
		case ZXDG_TOPLEVEL_V6_STATE_FULLSCREEN:
			fullscreen = true;
			break;
		default:
			break;
		}
	}

	if (fullscreen != w->fullscreen) {
		if (fullscreen) {
			w->saved_width = w->width;
			w->saved_height = w->height;
			w->saved_size_set = w->size_set;
		} else {
			w->width = w->saved_width;
			w->height = w->saved_height;
			w->size_set = w->saved_size_set;
		}
		w->fullscreen = fullscreen;
	}

	if (width <= 0 || height <= 0)
		return;

	if (w->width != width || w->height != height) {
		w->width = width;
		w->height = height;
		w->size_set = true;
	}
}

static void
xdg_toplevel_handle_close(void *data, struct zxdg_toplevel_v6 *xdg_toplevel)
{
	struct window *w = data;
	struct display *d = w->display;

	d->running = 0;
}

static const struct zxdg_toplevel_v6_listener xdg_toplevel_listener = {
	xdg_toplevel_handle_configure,
	xdg_toplevel_handle_close,
};

static void
xdg_surface_handle_configure(void *data, struct zxdg_surface_v6 *xdg_surface,
			     uint32_t serial)
{
	struct window *w = data;

	zxdg_surface_v6_ack_configure(xdg_surface, serial);

	w->configured = true;
	if (window_recenter(w))
		window_commit(w);
}

static const struct zxdg_surface_v6_listener xdg_surface_listener = {
	xdg_surface_handle_configure,
};


static void
shell_surface_handle_ping(void *data, struct wl_shell_surface *shell_surface,
			  uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_surface_handle_configure(void *data, struct wl_shell_surface *shell_surface,
			       uint32_t edges, int32_t width, int32_t height)
{
	struct window *w = data;

	if (w->width == 0 || w->height == 0)
		return;

	w->width = width;
	w->height = height;
	w->size_set = true;
	w->configured = true;

	if (window_recenter(w))
		window_commit(w);
}

static void
shell_surface_handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	shell_surface_handle_ping,
	shell_surface_handle_configure,
	shell_surface_handle_popup_done
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
	window->ar_x = 1;
	window->ar_y = 1;

	if (display->xdg_shell) {
		window->xdg_surface =
			zxdg_shell_v6_get_xdg_surface(display->xdg_shell,
						      window->surface);

		zxdg_surface_v6_add_listener(window->xdg_surface,
					     &xdg_surface_listener, window);

		window->xdg_toplevel =
			zxdg_surface_v6_get_toplevel(window->xdg_surface);

		zxdg_toplevel_v6_add_listener(window->xdg_toplevel,
					      &xdg_toplevel_listener, window);
		zxdg_toplevel_v6_set_title(window->xdg_toplevel, "v4l-decode");

		wl_surface_commit(window->surface);
	} else if (display->wl_shell) {
		window->shell_surface =
			wl_shell_get_shell_surface(display->wl_shell,
						   window->surface);

		wl_shell_surface_add_listener(window->shell_surface,
					      &shell_surface_listener, window);

		wl_shell_surface_set_title(window->shell_surface, "v4l-decode");
		wl_shell_surface_set_toplevel(window->shell_surface);

		window->configured = true;
	}

	if (display->viewporter) {
		window->viewport =
			wp_viewporter_get_viewport(display->viewporter,
						   window->surface);
	} else if (display->scaler) {
		window->legacy_viewport =
			wl_scaler_get_viewport(display->scaler,
					       window->surface);
	}

	wl_list_insert(&display->window_list, &window->link);

	return window;
}

static struct window *
display_find_window_by_surface(struct display *display,
			       struct wl_surface *surface)
{
	struct window *window;

	wl_list_for_each(window, &display->window_list, link) {
		if (window->surface == surface)
			return window;
	}

	return NULL;
}

void
window_destroy(struct window *window)
{
	wl_list_remove(&window->link);

	if (window->xdg_toplevel)
		zxdg_toplevel_v6_destroy(window->xdg_toplevel);
	if (window->xdg_surface)
		zxdg_surface_v6_destroy(window->xdg_surface);
	if (window->shell_surface)
		wl_shell_surface_destroy(window->shell_surface);
	if (window->viewport)
		wp_viewport_destroy(window->viewport);
	if (window->legacy_viewport)
		wl_viewport_destroy(window->legacy_viewport);

	wl_surface_destroy(window->surface);

	free(window);
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct fb *fb = data;

	fb->busy = 0;

	dbg("buffer %d released", fb->index);

	if (fb->release_cb)
		fb->release_cb(fb, fb->cb_data);
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static void
create_succeeded(void *data,
		 struct zwp_linux_buffer_params_v1 *params,
		 struct wl_buffer *new_buffer)
{
	struct fb *fb = data;

	fb->buffer = new_buffer;
	wl_buffer_add_listener(fb->buffer, &buffer_listener, fb);

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

static void
dmabuf_legacy_create_succeeded(void *data,
			       struct zlinux_buffer_params *params,
			       struct wl_buffer *new_buffer)
{
	struct fb *fb = data;

	fb->buffer = new_buffer;
	wl_buffer_add_listener(fb->buffer, &buffer_listener, fb);

	zlinux_buffer_params_destroy(params);
}

static void
dmabuf_legacy_create_failed(void *data, struct zlinux_buffer_params *params)
{
	struct fb *fb = data;

	fb->buffer = NULL;

	zlinux_buffer_params_destroy(params);

	err("zlinux_buffer_params.create failed");

	fb->window->display->running = 0;
}

static const struct zlinux_buffer_params_listener dmabuf_legacy_params_listener = {
	dmabuf_legacy_create_succeeded,
	dmabuf_legacy_create_failed
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
window_create_buffer(struct window *window, int group, int index, int fd,
		     uint32_t format, int width, int height, int n_planes,
		     const int *plane_offsets, const int *plane_strides)
{
	struct display *display = window->display;
	struct fb *fb;

#if 0
	if (!format_is_supported(display, format)) {
		err("unsupported display format");
		return NULL;
	}
#endif

	if (n_planes <= 0 || n_planes > FB_MAX_PLANES) {
		err("invalid number of planes");
		return NULL;
	}

	fb = calloc(1, sizeof *fb);
	fb->group = group;
	fb->index = index;
	fb->fd = fd;
	fb->format = format;
	fb->width = width;
	fb->height = height;
	fb->window = window;
	fb->ar_x = 1;
	fb->ar_y = 1;

	fb->n_planes = n_planes;
	memcpy(fb->offsets, plane_offsets, n_planes * sizeof (int));
	memcpy(fb->strides, plane_strides, n_planes * sizeof (int));

	if (display->dmabuf) {
		struct zwp_linux_buffer_params_v1 *params =
			zwp_linux_dmabuf_v1_create_params(display->dmabuf);

		for (int i = 0; i < fb->n_planes; i++) {
			zwp_linux_buffer_params_v1_add(params, fb->fd, i,
						       fb->offsets[i],
						       fb->strides[i], 0, 0);
		}

		zwp_linux_buffer_params_v1_add_listener(params, &params_listener, fb);
		zwp_linux_buffer_params_v1_create(params, fb->width, fb->height,
						  fb->format, 0);
	} else {
		struct zlinux_buffer_params *params =
			zlinux_dmabuf_create_params(display->dmabuf_legacy);

		for (int i = 0; i < fb->n_planes; i++) {
			zlinux_buffer_params_add(params, fb->fd, i,
						 fb->offsets[i],
						 fb->strides[i], 0, 0);
		}

		zlinux_buffer_params_add_listener(params, &dmabuf_legacy_params_listener, fb);
		zlinux_buffer_params_create(params, fb->width, fb->height,
					    fb->format, 0);
	}

	wl_display_roundtrip(display->display);

	if (!fb->buffer) {
		fb_destroy(fb);
		return NULL;
	}

	return fb;
}

void
window_show_buffer(struct window *window, struct fb *fb,
		   fb_release_cb_t release_cb, void *cb_data)
{
	fb->release_cb = release_cb;
	fb->cb_data = cb_data;

	dbg("present buffer %d", fb->index);

	window->buffer = fb;

	if (window->configured) {
		window_recenter(window);
		window_commit(window);
	}
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
dmabuf_legacy_format(void *data, struct zlinux_dmabuf *zlinux_dmabuf,
		     uint32_t format)
{
	struct display *d = data;

	assert(d->drm_format_count <= 32);
	d->drm_formats[d->drm_format_count++] = format;
}

static const struct zlinux_dmabuf_listener dmabuf_legacy_listener = {
	dmabuf_legacy_format
};

static void
xdg_shell_handle_ping(void *data, struct zxdg_shell_v6 *xdg_shell,
		      uint32_t serial)
{
	zxdg_shell_v6_pong(xdg_shell, serial);
}

static const struct zxdg_shell_v6_listener xdg_shell_listener = {
	xdg_shell_handle_ping,
};

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
		       uint32_t format, int fd, uint32_t size)
{
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface,
		      struct wl_array *keys)
{
	struct display *display = data;
	struct window *window;

	window = display_find_window_by_surface(display, surface);

	display->keyboard_focus = window;
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface)
{
	struct display *display = data;

	display->keyboard_focus = NULL;
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
		    uint32_t serial, uint32_t time, uint32_t key,
		    uint32_t state)
{
	struct display *display = data;
	struct window *window = display->keyboard_focus;

	if (!window || !window->key_cb)
		return;

	window->key_cb(window, time, key, state);
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
			  uint32_t serial, uint32_t mods_depressed,
			  uint32_t mods_latched, uint32_t mods_locked,
			  uint32_t group)
{
}

static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
			    int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
	keyboard_handle_repeat_info
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	struct display *d = data;

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
		d->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);

	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
		if (d->seat_version >= 3)
			wl_keyboard_release(d->keyboard);
		else
			wl_keyboard_destroy(d->keyboard);
		d->keyboard = NULL;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	seat_handle_name,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (!strcmp(interface, "wl_compositor")) {
		d->compositor_version = MIN(version, 4);
		d->compositor = wl_registry_bind(registry, id,
						 &wl_compositor_interface,
						 d->compositor_version);
	} else if (!strcmp(interface, "wp_viewporter")) {
		d->viewporter = wl_registry_bind(registry, id,
						 &wp_viewporter_interface, 1);
	} else if (!strcmp(interface, "wl_scaler")) {
		d->scaler = wl_registry_bind(registry, id,
					     &wl_scaler_interface, 1);
	} else if (!strcmp(interface, "wp_presentation")) {
		d->presentation = wl_registry_bind(registry, id,
						   &wp_presentation_interface,
						   1);
	} else if (!strcmp(interface, "zxdg_shell_v6")) {
		d->xdg_shell = wl_registry_bind(registry, id,
						&zxdg_shell_v6_interface, 1);
		zxdg_shell_v6_add_listener(d->xdg_shell,
					   &xdg_shell_listener, d);
	} else if (!strcmp(interface, "wl_shell")) {
		d->wl_shell = wl_registry_bind(registry, id,
					       &wl_shell_interface, 1);
	} else if (!strcmp(interface, "zwp_linux_dmabuf_v1")) {
		d->dmabuf = wl_registry_bind(registry, id,
					     &zwp_linux_dmabuf_v1_interface, 1);
		zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener,
						 d);
	} else if (!strcmp(interface, "zlinux_dmabuf")) {
		d->dmabuf_legacy = wl_registry_bind(registry, id,
						    &zlinux_dmabuf_interface, 1);
		zlinux_dmabuf_add_listener(d->dmabuf_legacy,
					   &dmabuf_legacy_listener, d);
	} else if (!strcmp(interface, "wl_seat") && !d->seat) {
		d->seat_version = MIN(version, 5);
		d->seat = wl_registry_bind(registry, id, &wl_seat_interface,
					   d->seat_version);
		wl_seat_add_listener(d->seat, &seat_listener, d);
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
	if (display->seat) {
		seat_handle_capabilities(display, display->seat, 0);
		wl_seat_destroy(display->seat);
	}

	if (display->scaler)
		wl_scaler_destroy(display->scaler);
	if (display->viewporter)
		wp_viewporter_destroy(display->viewporter);
	if (display->presentation)
		wp_presentation_destroy(display->presentation);
	if (display->compositor)
		wl_compositor_destroy(display->compositor);
	if (display->xdg_shell)
		zxdg_shell_v6_destroy(display->xdg_shell);
	if (display->wl_shell)
		wl_shell_destroy(display->wl_shell);
	if (display->dmabuf)
		zwp_linux_dmabuf_v1_destroy(display->dmabuf);
	if (display->dmabuf_legacy)
		zlinux_dmabuf_destroy(display->dmabuf_legacy);
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

	if (!display->xdg_shell && !display->wl_shell) {
		err("missing wayland shell");
		goto fail;
	}

	if (!display->dmabuf && !display->dmabuf_legacy) {
		err("missing wayland dmabuf");
		goto fail;
	}

	wl_list_init(&display->window_list);

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

struct wl_display *
display_get_wl_display(struct display *display)
{
	return display->display;
}
