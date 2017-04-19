#ifndef DISPLAY_H_
# define DISPLAY_H_

#include <wayland-client.h>
#include <stdint.h>
#include <media/msm_vidc.h>

#include "list.h"

struct display;
struct window;
struct fb;

typedef void (*fb_release_cb_t)(struct fb *fb, void *data);

typedef void (*window_key_cb_t)(struct window *w, uint32_t time, uint32_t key,
				enum wl_keyboard_key_state state);

#define FB_MAX_PLANES 3

struct fb {
	struct window *window;
	int group;
	int index;
	int fd;
	int offsets[FB_MAX_PLANES];
	int strides[FB_MAX_PLANES];
	int n_planes;
	int width;
	int height;
	int busy;
	int ar_x, ar_y;
	int crop_x, crop_y, crop_w, crop_h;
	uint32_t format;
	struct list_head link;
	struct wl_buffer *buffer;
	struct wl_callback *sync_callback;
	struct wp_presentation_feedback *presentation_feedback;
	fb_release_cb_t release_cb;
	void *cb_data;
};

struct wl_display *display_get_wl_display(struct display *display);

struct display *display_create(void);
int display_is_running(struct display *display);
struct window *display_create_window(struct display *display);
void display_destroy(struct display *display);

void window_set_user_data(struct window *w, void *data);
void *window_get_user_data(struct window *w);
void window_set_key_callback(struct window *w, window_key_cb_t handler);
void window_set_aspect_ratio(struct window *w, int ar_x, int ar_y);
void window_toggle_fullscreen(struct window *w);

void window_show_buffer(struct window *window, struct fb *fb,
			fb_release_cb_t release_cb, void *cb_data);
struct fb *window_create_buffer(struct window *window, int group, int index,
				int fd, uint32_t format, int width, int height,
				int n_planes, const int *plane_offsets,
				const int *plane_strides);
void window_destroy(struct window *window);

void fb_apply_extradata(struct fb *fb,
			const struct msm_vidc_extradata_header *hdr);
void fb_destroy(struct fb *fb);

#endif /* !DISPLAY_H_ */
