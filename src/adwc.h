
#ifndef _WAYLAND_SYSTEM_ADWC_H_
#define _WAYLAND_SYSTEM_ADWC_H_

/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <linux/input.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <execinfo.h>
#include <time.h>

#include <libudev.h>
#include <pixman.h>
#include <wayland-server.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "matrix.h"
#include "../shared/config-parser.h"


typedef uint32_t tTags;

struct weston_process;
typedef void (*weston_process_cleanup_func_t)(struct weston_process *process,
					    int status);
struct weston_process {
	pid_t pid;
	weston_process_cleanup_func_t cleanup;
	struct wl_list link;
};


struct weston_transform {
	struct weston_matrix matrix;
	struct wl_list link;
};

struct weston_surface;
struct weston_input_device;

struct weston_mode {
	uint32_t flags;
	int32_t width, height;
	uint32_t refresh;
	struct wl_list link;
};

struct weston_border {
	int32_t left, right, top, bottom;
};

struct weston_output_zoom {
	int active;
	float increment;
	float level;
	float magnification;
	float trans_x, trans_y;
};

/* bit compatible with drm definitions. */
enum dpms_enum {
	WESTON_DPMS_ON,
	WESTON_DPMS_STANDBY,
	WESTON_DPMS_SUSPEND,
	WESTON_DPMS_OFF
};

struct weston_output {
	struct wl_list link;
	struct wl_global *global;
	struct weston_compositor *compositor;
	struct weston_matrix matrix;
	struct wl_list frame_callback_list;
	int32_t x, y, mm_width, mm_height;
	struct weston_border border;
	pixman_region32_t region;
	pixman_region32_t previous_damage;
	uint32_t flags;
	int repaint_needed;
	int repaint_scheduled;
	struct weston_output_zoom zoom;
	int dirty;

	char *make, *model;
	uint32_t subpixel;
	
	struct weston_mode *current;
	struct wl_list mode_list;

	void (*repaint)(struct weston_output *output,
			pixman_region32_t *damage);
	void (*destroy)(struct weston_output *output);
	void (*assign_planes)(struct weston_output *output);
	void (*read_pixels)(struct weston_output *output, void *data);

	/* backlight values are on 0-255 range, where higher is brighter */
	uint32_t backlight_current;
	void (*set_backlight)(struct weston_output *output, uint32_t value);
	void (*set_dpms)(struct weston_output *output, enum dpms_enum level);
	
	
	tTags Tags;
	
	struct wl_list surfaces;
};

struct weston_input_device {
	struct wl_input_device input_device;
	struct weston_compositor *compositor;
	struct weston_surface *sprite;
	struct weston_surface *drag_surface;
	struct wl_listener drag_surface_destroy_listener;
	int32_t hotspot_x, hotspot_y;
	struct wl_list link;
	uint32_t modifier_state;
	int hw_cursor;
	struct wl_surface *saved_kbd_focus;
	struct wl_listener saved_kbd_focus_listener;

	uint32_t num_tp;
	struct wl_surface *touch_focus;
	struct wl_listener touch_focus_listener;
	struct wl_resource *touch_focus_resource;
	struct wl_listener touch_focus_resource_listener;

	struct wl_listener new_drag_icon_listener;
};

struct weston_shader {
	GLuint program;
	GLuint vertex_shader, fragment_shader;
	GLint proj_uniform;
	GLint tex_uniform;
	GLint alpha_uniform;
	GLint color_uniform;
	GLint texwidth_uniform;
};


enum {
	WESTON_COMPOSITOR_ACTIVE,
	WESTON_COMPOSITOR_IDLE,		/* shell->unlock called on activity */
	WESTON_COMPOSITOR_SLEEPING	/* no rendering, no frame events */
};

struct screenshooter;

struct weston_layer {
	struct wl_list surface_list;
	struct wl_list link;
};

struct weston_compositor {
	struct wl_shm *shm;
	struct wl_signal destroy_signal;

	EGLDisplay display;
	EGLContext context;
	EGLConfig config;
	GLuint fbo;
	struct weston_shader texture_shader;
	struct weston_shader solid_shader;
	struct weston_shader *current_shader;
	struct wl_display *wl_display;

	struct wl_signal activate_signal;
	struct wl_signal lock_signal;
	struct wl_signal unlock_signal;

	struct wl_event_loop *input_loop;
	struct wl_event_source *input_loop_source;
	
	/* There can be more than one, but not right now... */
	struct wl_input_device *input_device;
	
//	struct weston_layer fade_layer;
//	struct weston_layer cursor_layer;	//shell attaches its layers to this?
	
	struct wl_list output_list;
	struct wl_list input_device_list;
	
//	struct wl_list layer_list;
	
	struct wl_list surface_list;	//list of what the backend draws?
	
	struct wl_list binding_list;
	uint32_t state;
	struct wl_event_source *idle_source;
	uint32_t idle_inhibit;
	int option_idle_time;		/* default timeout, s */
	int idle_time;			/* effective timeout, s */

	/* Repaint state. */
	struct wl_array vertices, indices;
	pixman_region32_t damage;

	uint32_t focus;

	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
		image_target_renderbuffer_storage;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNEGLBINDWAYLANDDISPLAYWL bind_display;
	PFNEGLUNBINDWAYLANDDISPLAYWL unbind_display;
	int has_bind_display;

	void (*destroy)(struct weston_compositor *ec);
	int (*authenticate)(struct weston_compositor *c, uint32_t id);

	struct screenshooter *screenshooter;
	int launcher_sock;
};

#define MODIFIER_CTRL	(1 << 8)
#define MODIFIER_ALT	(1 << 9)
#define MODIFIER_SUPER	(1 << 10)

enum weston_output_flags {
	WL_OUTPUT_FLIPPED = 0x01
};

struct weston_region {
	struct wl_resource resource;
	pixman_region32_t region;
};


typedef struct weston_surface tSurf;

struct weston_surface {
	struct wl_surface surface;
	
	struct wl_client* client;
	
	GLuint texture;
	pixman_region32_t clip;
	pixman_region32_t damage;
	pixman_region32_t opaque;
	pixman_region32_t input;
	int32_t pitch;
	struct wl_list link;
	
//	struct wl_list layer_link;
	
	struct weston_shader *shader;
	GLfloat color[4];
	uint32_t alpha;
	
	struct weston_border border;
	
	struct {
		int32_t x, y; /* surface translation on display */
		int32_t width, height;
		int dirty;
	} geometry, geometry_ours;
	
	struct weston_output *output;
	
	struct wl_list frame_callback_list;
	
	EGLImageKHR image;
	
	struct wl_buffer *buffer;
	struct wl_listener buffer_destroy_listener;
	
	/*
	 * If non-NULL, this function will be called on surface::attach after
	 * a new buffer has been set up for this surface. The integer params
	 * are the sx and sy paramerters supplied to surface::attach .
	 */
	void (*configure)(struct weston_surface *es, int32_t sx, int32_t sy);
	void *private;
};



#include "desktop-shell-server-protocol.h"
#include "../shared/config-parser.h"

struct shell_surface;


enum {
	L_eBelow,
	L_eNorm,
	L_eFloat,
	L_eAbove,
	L_NUM
};

struct wl_shell {
	struct weston_compositor *pEC;

	struct wl_listener lock_listener;
	struct wl_listener unlock_listener;
	struct wl_listener destroy_listener;

	struct weston_layer fullscreen_layer;
	struct weston_layer panel_layer;
	struct weston_layer toplevel_layer;
	struct weston_layer background_layer;
	struct weston_layer lock_layer;
	
//	struct weston_layer lock_layer;
	
	struct {
		struct weston_process process;
		struct wl_client *client;
		struct wl_resource *desktop_shell;

		unsigned deathcount;
		uint32_t deathstamp;
	} child;

	bool locked;
	bool prepare_event_sent;

	struct shell_surface *lock_surface;
	struct wl_listener lock_surface_listener;

	struct wl_list backgrounds;
	struct wl_list panels;
	
	struct {
		char *path;
		int duration;
		struct wl_resource *binding;
		struct wl_list surfaces;
		struct weston_process process;
	} screensaver;

	struct weston_surface *debug_repaint_surface;
	
	struct wl_list L[L_NUM];	//array with lists of windows
	
};
extern struct wl_shell gShell;

enum shell_surface_type {
	SHELL_SURFACE_NONE,

	SHELL_SURFACE_PANEL,
	SHELL_SURFACE_BACKGROUND,
	SHELL_SURFACE_LOCK,
	SHELL_SURFACE_SCREENSAVER,

	SHELL_SURFACE_TOPLEVEL,
	SHELL_SURFACE_TRANSIENT,
	SHELL_SURFACE_FULLSCREEN,
	SHELL_SURFACE_MAXIMIZED,
	SHELL_SURFACE_POPUP
};

struct shell_surface {
	struct wl_resource resource;

	struct weston_surface *surface;
	struct wl_listener surface_destroy_listener;
	struct shell_surface *parent;
	struct wl_shell *shell;

	enum shell_surface_type type;
	int32_t saved_x, saved_y;
	bool saved_position_valid;

/*	struct {
		struct weston_transform transform;
		struct weston_matrix rotation;
	} rotation;*/

	struct {
		struct wl_pointer_grab grab;
		uint32_t time;
		int32_t x, y;
		struct weston_transform parent_transform;
		int32_t initial_up;
	} popup;

	struct {
		enum wl_shell_surface_fullscreen_method type;
		struct weston_transform transform; /* matrix from x, y */
		uint32_t framerate;
		struct weston_surface *black_surface;
	} fullscreen;

	struct weston_output *fullscreen_output;
	struct weston_output *output;
	struct wl_list link;

	int force_configure;
	
	
	tTags Tags;
	uint8_t L;
	struct wl_list L_link;
	struct wl_list O_link;
};

struct shell_grab {
	struct wl_pointer_grab grab;
	struct shell_surface *shsurf;
	struct wl_listener shsurf_destroy_listener;
};

struct weston_move_grab {
	struct shell_grab base;
	int32_t dx, dy;
};

struct weston_swap_grab {
	struct shell_grab base;
	int32_t dx, dy;
};

struct rotate_grab {
	struct shell_grab base;
	struct weston_matrix rotation;
	struct {
		int32_t x;
		int32_t y;
	} center;
};


struct weston_binding;
typedef void (*weston_binding_handler_t)(struct wl_input_device *device,
					 uint32_t time, uint32_t key,
					 uint32_t button,
					 uint32_t axis,
					 int32_t state, void *data);

typedef struct {
	uint32_t Key, Button, Axis, Mods;
	weston_binding_handler_t Handler;
	uintptr_t Priv;
}tADWC_Binding;



struct xserver {
	struct wl_resource resource;
};

struct weston_xserver {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	struct wl_event_source *sigchld_source;
	int abstract_fd;
	struct wl_event_source *abstract_source;
	int unix_fd;
	struct wl_event_source *unix_source;
	int display;
	struct weston_process process;
	struct wl_resource *resource;
	struct wl_client *client;
	struct weston_compositor *compositor;
	struct weston_wm *wm;
	struct wl_listener activate_listener;
	struct wl_listener destroy_listener;
};



void
weston_surface_update_transform(struct weston_surface *surface);

void
weston_surface_to_global(struct weston_surface *surface,
			 int32_t sx, int32_t sy, int32_t *x, int32_t *y);
void
weston_surface_to_global_float(struct weston_surface *surface,
			       int32_t sx, int32_t sy, GLfloat *x, GLfloat *y);

void
weston_surface_from_global(struct weston_surface *surface,
			   int32_t x, int32_t y, int32_t *sx, int32_t *sy);

void
weston_surface_activate(struct weston_surface *surface,
			struct weston_input_device *device);
void
weston_surface_draw(struct weston_surface *es,
		    struct weston_output *output, pixman_region32_t *damage);

void
notify_motion(struct wl_input_device *device,
	      uint32_t time, int x, int y);
void
notify_button(struct wl_input_device *device,
	      uint32_t time, int32_t button, int32_t state);
void
notify_axis(struct wl_input_device *device,
	      uint32_t time, uint32_t axis, int32_t value);
void
notify_key(struct wl_input_device *device,
	   uint32_t time, uint32_t key, uint32_t state);

void
notify_pointer_focus(struct wl_input_device *device,
		     struct weston_output *output,
		     int32_t x, int32_t y);

void
notify_keyboard_focus(struct wl_input_device *device, struct wl_array *keys);

void
notify_touch(struct wl_input_device *device, uint32_t time, int touch_id,
	     int x, int y, int touch_type);

void
weston_layer_init(struct weston_layer *layer, struct wl_list *below);

void
weston_output_finish_frame(struct weston_output *output, int msecs);
void
weston_output_damage(struct weston_output *output);
void
weston_compositor_repick(struct weston_compositor *compositor);
void
weston_compositor_schedule_repaint(struct weston_compositor *compositor);
void
weston_compositor_fade(struct weston_compositor *compositor, float tint);
void
weston_compositor_damage_all(struct weston_compositor *compositor);
void
weston_compositor_unlock(struct weston_compositor *compositor);
void
weston_compositor_wake(struct weston_compositor *compositor);
void
weston_compositor_activity(struct weston_compositor *compositor);
void
weston_compositor_update_drag_surfaces(struct weston_compositor *compositor);


struct weston_binding *
weston_compositor_add_binding(struct weston_compositor *compositor,
			      uint32_t key, uint32_t button, uint32_t axis, uint32_t modifier,
			      weston_binding_handler_t binding, void *data);
void
weston_binding_destroy(struct weston_binding *binding);

void
weston_binding_list_destroy_all(struct wl_list *list);

void
weston_compositor_run_binding(struct weston_compositor *compositor,
			      struct weston_input_device *device,
			      uint32_t time,
			      uint32_t key, uint32_t button, uint32_t axis, int32_t state);
int
weston_environment_get_fd(const char *env);

struct wl_list *
weston_compositor_top(struct weston_compositor *compositor);

struct weston_surface *
weston_surface_create(struct weston_compositor *compositor);

void
weston_surface_configure(struct weston_surface *surface,
			 int32_t x, int32_t y, int width, int height);

void
weston_surface_restack(struct weston_surface *surface, struct wl_list *below);

void
weston_surface_set_position(struct weston_surface *surface,
			    int32_t x, int32_t y);

int
weston_surface_is_mapped(struct weston_surface *surface);

void
weston_surface_assign_output(struct weston_surface *surface);

void
weston_surface_damage(struct weston_surface *surface);

void
weston_surface_damage_below(struct weston_surface *surface);

void
weston_buffer_post_release(struct wl_buffer *buffer);

uint32_t
weston_compositor_get_time(void);

int
weston_compositor_init(struct weston_compositor *ec, struct wl_display *display);
void
weston_compositor_shutdown(struct weston_compositor *ec);
void
weston_output_update_zoom(struct weston_output *output, int x, int y);
void
weston_output_update_matrix(struct weston_output *output);
void
weston_output_move(struct weston_output *output, int x, int y);
void
weston_output_init(struct weston_output *output, struct weston_compositor *c,
		   int x, int y, int width, int height, uint32_t flags);
void
weston_output_destroy(struct weston_output *output);

void
weston_input_device_init(struct weston_input_device *device,
			 struct weston_compositor *ec);

void
weston_input_device_release(struct weston_input_device *device);

enum {
	TTY_ENTER_VT,
	TTY_LEAVE_VT
};

typedef void (*tty_vt_func_t)(struct weston_compositor *compositor, int event);

struct tty *
tty_create(struct weston_compositor *compositor,
	   tty_vt_func_t vt_func, int tty_nr);

void
tty_destroy(struct tty *tty);

int
tty_activate_vt(struct tty *tty, int vt);

void
screenshooter_create(struct weston_compositor *ec);


struct wl_client *
weston_client_launch(struct weston_compositor *compositor,
		     struct weston_process *proc,
		     const char *path,
		     weston_process_cleanup_func_t cleanup);

void
weston_watch_process(struct weston_process *process);


bool		weston_wm_window_resize			(struct weston_wm* wm, struct weston_surface* es, int32_t x, int32_t y, int32_t width, int32_t height, bool hints);
void*
weston_xserver_init(struct weston_compositor *compositor);
/*
struct weston_zoom;
typedef	void (*weston_zoom_done_func_t)(struct weston_zoom *zoom, void *data);

struct weston_zoom *
weston_zoom_run(struct weston_surface *surface, GLfloat start, GLfloat stop,
		weston_zoom_done_func_t done, void *data);
*/
void
weston_surface_set_color(struct weston_surface *surface,
			 GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);

void
weston_surface_destroy(struct weston_surface *surface);

struct weston_compositor *
backend_init(struct wl_display *display, int argc, char *argv[]);




void
shell_surface_set_toplevel(struct wl_client *client,
			   struct wl_resource *resource);

static void
shell_surface_configure(struct weston_surface *, int32_t, int32_t);
static void
shell_get_shell_surface(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t id,
			struct wl_resource *surface_resource);

struct shell_surface*
Shell_get_surface(struct wl_client *client, struct weston_surface *surface);

static struct weston_output*	CurrentOutput	();

void	ShSurf_LSet		(struct shell_surface* shsurf, uint8_t l);

#endif

