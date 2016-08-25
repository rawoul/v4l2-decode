#ifndef DISPLAY_H_
# define DISPLAY_H_

#include <wayland-client.h>
#include <stdint.h>

struct display;
struct window;
struct fb;

typedef void (*fb_release_cb_t)(struct fb *fb, void *data);

typedef void (*window_key_cb_t)(struct window *w, uint32_t time, uint32_t key,
				enum wl_keyboard_key_state state);

struct fb {
	struct window *window;
	int index;
	int fd;
	int offset;
	int width;
	int height;
	int stride;
	uint32_t format;
	struct wl_buffer *buffer;
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

void window_show_buffer(struct window *window, struct fb *fb,
			fb_release_cb_t release_cb, void *cb_data);
struct fb *window_create_buffer(struct window *window, int index, int fd,
				int offset, uint32_t format,
				int width, int height, int stride);
void window_destroy(struct window *window);

void fb_destroy(struct fb *fb);

#endif /* !DISPLAY_H_ */
