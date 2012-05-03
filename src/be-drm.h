

#ifndef _WAYLAND_SYSTEM_BE_DRM_H_
#define _WAYLAND_SYSTEM_BE_DRM_H_


#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <gbm.h>

struct be_compositor {
	struct weston_compositor base;
	
	struct udev *udev;
	struct wl_event_source *be_source;
	
	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_be_source;
	
	struct {
		int id;
		int fd;
	} drm;
	
	struct gbm_device *gbm;
	uint32_t *crtcs;
	int num_crtcs;
	uint32_t crtc_allocator;
	uint32_t connector_allocator;
	struct tty *tty;

	struct gbm_surface *dummy_surface;
	EGLSurface dummy_egl_surface;

	struct wl_list sprite_list;
	int sprites_are_broken;

	uint32_t prev_state;
};

struct be_mode {
	struct weston_mode base;
	drmModeModeInfo mode_info;
};

struct be_output {
	struct weston_output   base;
	
	uint32_t crtc_id;
	uint32_t connector_id;
	drmModeCrtcPtr original_crtc;
	
	struct gbm_surface *surface;
	EGLSurface egl_surface;
	
	uint32_t current_fb_id;
	uint32_t next_fb_id;
	struct gbm_bo *current_bo;
	struct gbm_bo *next_bo;
	
	struct wl_buffer *scanout_buffer;
	struct wl_listener scanout_buffer_destroy_listener;
	
	struct wl_buffer *pending_scanout_buffer;
	struct wl_listener pending_scanout_buffer_destroy_listener;
	
	struct backlight *backlight;
};

/*
 * An output has a primary display plane plus zero or more sprites for
 * blending display contents.
 */
struct be_sprite {
	struct wl_list link;
	
	uint32_t fb_id;
	uint32_t pending_fb_id;
	
	struct weston_surface *surface;
	struct weston_surface *pending_surface;
	
	struct be_compositor *compositor;
	
	struct wl_listener destroy_listener;
	struct wl_listener pending_destroy_listener;
	
	uint32_t possible_crtcs;
	uint32_t plane_id;
	uint32_t count_formats;
	
	int32_t src_x, src_y;
	uint32_t src_w, src_h;
	uint32_t dest_x, dest_y;
	uint32_t dest_w, dest_h;
	
	uint32_t formats[];
};

extern struct be_compositor gBE;
<<<<<<< HEAD
=======



int		be_output_set_cursor		(struct weston_output *output_base, struct weston_input_device *eid);


>>>>>>> 29b98d606c2aa382206d95d0e92c2b14679e77e9

#endif

