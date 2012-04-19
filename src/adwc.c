/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
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
 
/*
 * Copyright © 2010-2011 Intel Corporation
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

#define _GNU_SOURCE

#include "config.h"

#include <wayland-server.h>
//#include "compositor.h"
#include "adwc.h"
#include "desktop-shell-server-protocol.h"
#include "../shared/config-parser.h"

#include "adwc_config.h"

static struct wl_list child_process_list;
static jmp_buf segv_jmp_buf;

struct weston_xserver *gpXServer;

typedef struct {
	unsigned long sec, nsec;
} tSC_time;

static inline void	SC_time_Start	(tSC_time* pt)
{
	struct timespec time;
	clock_gettime (CLOCK_REALTIME, &time);
	pt->sec = time.tv_sec;
	pt->nsec = time.tv_nsec;
}

static inline double	SC_time_Diff	(tSC_time* pt)
{
	struct timespec time;
	clock_gettime (CLOCK_REALTIME, &time);
	
	pt->sec = time.tv_sec - pt->sec;
	if (pt->nsec <= time.tv_nsec) {
		pt->nsec = time.tv_nsec - pt->nsec;
	} else {
		pt->sec -= 1;
		pt->nsec = 1000000000 - pt->nsec + time.tv_nsec;
	}
	return pt->sec*1000 + pt->nsec/1000000.0;
}

#define dts()	tSC_time _d_time;	SC_time_Start (&_d_time)
#define dte(name)	printf ("%s: " #name "	Time	%f\n", __FUNCTION__, SC_time_Diff (&_d_time))



#define dTrace_E(fmt,...)		\
	do {		\
		printf (">%s:\t" fmt "\n", __FUNCTION__ ,##__VA_ARGS__);		\
	}while(0)
#define dTrace_L(fmt,...)		\
	do {		\
		printf ("<%s:\t" fmt "\n", __FUNCTION__ ,##__VA_ARGS__);		\
	}while(0)

#define dTrace_E_Client(c)		\
	do {		\
		if (c)		\
			dTrace_E("name %s", c->name);		\
		else		\
			dTrace_E("c %lx", c);		\
	}while(0)
#define dTrace_L_Client(c)		\
	do {		\
		if (c)		\
			dTrace_L("name %s", c->name);		\
		else		\
			dTrace_L("c %lx", c);		\
	}while(0)



static int
sigchld_handler(int signal_number, void *data)
{
	struct weston_process *p;
	int status;
	pid_t pid;

	pid = waitpid(-1, &status, WNOHANG);
	if (!pid)
		return 1;

	wl_list_for_each(p, &child_process_list, link) {
		if (p->pid == pid)
			break;
	}

	if (&p->link == &child_process_list) {
		fprintf(stderr, "unknown child process exited\n");
		return 1;
	}

	wl_list_remove(&p->link);
	p->cleanup(p, status);

	return 1;
}

WL_EXPORT void
weston_watch_process(struct weston_process *process)
{
	wl_list_insert(&child_process_list, &process->link);
}

static void
child_client_exec(int sockfd, const char *path)
{
	int clientfd;
	char s[32];
	sigset_t allsigs;

	/* do not give our signal mask to the new process */
	sigfillset(&allsigs);
	sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

	/* Launch clients as the user. */
	seteuid(getuid());

	/* SOCK_CLOEXEC closes both ends, so we dup the fd to get a
	 * non-CLOEXEC fd to pass through exec. */
	clientfd = dup(sockfd);
	if (clientfd == -1) {
		fprintf(stderr, "compositor: dup failed: %m\n");
		return;
	}

	snprintf(s, sizeof s, "%d", clientfd);
	setenv("WAYLAND_SOCKET", s, 1);

	if (execl(path, path, NULL) < 0)
		fprintf(stderr, "compositor: executing '%s' failed: %m\n",
			path);
}

WL_EXPORT struct wl_client *
weston_client_launch(tComp *compositor,
			struct weston_process *proc,
			const char *path,
			weston_process_cleanup_func_t cleanup)
{
	int sv[2];
	pid_t pid;
	struct wl_client *client;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		fprintf(stderr, "weston_client_launch: "
			"socketpair failed while launching '%s': %m\n",
			path);
		return NULL;
	}

	pid = fork();
	if (pid == -1) {
		close(sv[0]);
		close(sv[1]);
		fprintf(stderr,  "weston_client_launch: "
			"fork failed while launching '%s': %m\n", path);
		return NULL;
	}

	if (pid == 0) {
		child_client_exec(sv[1], path);
		exit(-1);
	}

	close(sv[1]);

	client = wl_client_create(compositor->wl_display, sv[0]);
	if (!client) {
		close(sv[0]);
		fprintf(stderr, "weston_client_launch: "
			"wl_client_create failed while launching '%s'.\n",
			path);
		return NULL;
	}

	proc->pid = pid;
	proc->cleanup = cleanup;
	weston_watch_process(proc);

	return client;
}

static void
surface_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	tSurf *es =
		container_of(listener, tSurf, 
			     buffer_destroy_listener);

	es->buffer = NULL;
}

static const pixman_region32_data_t undef_region_data;

static void
undef_region(pixman_region32_t *region)
{
	pixman_region32_fini(region);
	region->data = (pixman_region32_data_t *) &undef_region_data;
}

static int
region_is_undefined(pixman_region32_t *region)
{
	return region->data == &undef_region_data;
}

static void
empty_region(pixman_region32_t *region)
{
	if (!region_is_undefined(region))
		pixman_region32_fini(region);

	pixman_region32_init(region);
}


/** ************************************ weston_surface ************************************ **/

WL_EXPORT tSurf *	weston_surface_create	(tComp *compositor)
{
	dTrace_E("");
	tSurf *surface;
	
	surface = calloc(1, sizeof *surface);
	if (surface == NULL)
		return NULL;
	
	wl_signal_init(&surface->surface.resource.destroy_signal);
	
	wl_list_init(&surface->link);
	
	surface->surface.resource.client = NULL;
	
	surface->image = EGL_NO_IMAGE_KHR;
	surface->alpha = 255;
	surface->pitch = 1;
	
	surface->buffer = NULL;
	surface->output = NULL;
	
	pixman_region32_init(&surface->damage);
	pixman_region32_init(&surface->opaque);
	pixman_region32_init(&surface->clip);
	undef_region(&surface->input);
	
	wl_list_init(&surface->frame_callback_list);
	
	surface->buffer_destroy_listener.notify = surface_handle_buffer_destroy;
	
	surface->geometry.dirty = 1;
	dTrace_L("");
	return surface;
}

WL_EXPORT void		weston_surface_set_color			(tSurf *surface, GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	surface->color[0] = red;
	surface->color[1] = green;
	surface->color[2] = blue;
	surface->color[3] = alpha;
	surface->shader = &gShell.pEC->solid_shader;
}

WL_EXPORT void		weston_surface_damage_below			(tSurf *surface)
{
	tComp *compositor = gShell.pEC;
	pixman_region32_t damage;

	pixman_region32_init(&damage);
	
	pixman_region32_t boundingbox;
	pixman_region32_init_rect(&boundingbox,
						surface->geometry.x,
						surface->geometry.y,
						surface->geometry.width,
						surface->geometry.height);
	
	pixman_region32_subtract(&damage, &boundingbox,
				 &surface->clip);
	pixman_region32_union(&compositor->damage,
			      &compositor->damage, &damage);
	pixman_region32_fini(&damage);
}

void				surface_compute_bbox				(tSurf *surface, int32_t sx, int32_t sy, int32_t width, int32_t height, pixman_region32_t *bbox)
{
	GLfloat min_x = HUGE_VALF,  min_y = HUGE_VALF;
	GLfloat max_x = -HUGE_VALF, max_y = -HUGE_VALF;
	int32_t s[4][2] = {
		{ sx,         sy },
		{ sx,         sy + height },
		{ sx + width, sy },
		{ sx + width, sy + height }
	};
	GLfloat int_x, int_y;
	int i;

	for (i = 0; i < 4; ++i) {
		int32_t x, y;
		weston_surface_to_global (surface, s[i][0], s[i][1], &x, &y);
		if (x < min_x)
			min_x = x;
		if (x > max_x)
			max_x = x;
		if (y < min_y)
			min_y = y;
		if (y > max_y)
			max_y = y;
	}
	
	int_x = floorf(min_x);
	int_y = floorf(min_y);
	pixman_region32_init_rect(bbox, int_x, int_y,
				  ceilf(max_x) - int_x, ceilf(max_y) - int_y);
}

WL_EXPORT void		weston_surface_update_transform		(tSurf *surface)
{
	if (!surface->geometry.dirty)
		return;

	surface->geometry.dirty = 0;

	weston_surface_damage_below(surface);
	
	if (region_is_undefined(&surface->input))
		pixman_region32_init_rect(&surface->input, 0, 0, 
					  surface->geometry.width,
					  surface->geometry.height);
	
	if (weston_surface_is_mapped(surface))
		weston_surface_assign_output(surface);
	
	weston_compositor_schedule_repaint(gShell.pEC);
}

	
WL_EXPORT void		weston_surface_to_global			(tSurf *surface, int32_t sx, int32_t sy, int32_t *x, int32_t *y)
{
	*x = sx + surface->geometry.x;
	*y = sy + surface->geometry.y;
}

WL_EXPORT void		weston_surface_from_global			(tSurf *surface, int32_t x, int32_t y, int32_t *sx, int32_t *sy)
{
	*sx = x - surface->geometry.x;
	*sy = y - surface->geometry.y;
}

void				weston_surface_damage_rectangle		(tSurf *surface, int32_t sx, int32_t sy, int32_t width, int32_t height)
{
	weston_surface_update_transform(surface);

	pixman_region32_union_rect(&surface->damage, &surface->damage,
						surface->geometry.x + sx,
						surface->geometry.y + sy,
						width, height);
	
	weston_compositor_schedule_repaint(gShell.pEC);
}

WL_EXPORT void		weston_surface_damage				(tSurf *surface)
{
	weston_surface_update_transform(surface);
	
	pixman_region32_t boundingbox;
	pixman_region32_init_rect(&boundingbox,
					  surface->geometry.x,
					  surface->geometry.y,
					  surface->geometry.width,
					  surface->geometry.height);
	pixman_region32_union(&surface->damage, &surface->damage,
			      &boundingbox);

	weston_compositor_schedule_repaint(gShell.pEC);
}

WL_EXPORT void		weston_surface_configure			(tSurf *surface, int32_t x, int32_t y, int width, int height)
{
//	surface->geometry_prev = surface->geometry;
	surface->geometry.x = x;
	surface->geometry.y = y;
	surface->geometry.width = width;
	surface->geometry.height = height;
	surface->geometry.dirty = 1;
}

WL_EXPORT void		weston_surface_set_position			(tSurf *surface, int32_t x, int32_t y)
{
	surface->geometry.x = x;
	surface->geometry.y = y;
	surface->geometry.dirty = 1;
}


WL_EXPORT int		weston_surface_is_mapped			(tSurf *surface)
{
	if (surface->output)
		return 1;
	else
		return 0;
}





WL_EXPORT uint32_t
weston_compositor_get_time(void)
{
       struct timeval tv;

       gettimeofday(&tv, NULL);

       return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

tSurf*	weston_compositor_pick_surface		(tComp *compositor, int32_t x, int32_t y, int32_t *sx, int32_t *sy)
{
	tSurf *surface;
//	dTrace_E("");
	
	wl_list_for_each(surface, &compositor->surface_list, link) {
	//	printf ("weston_compositor_pick_surface %d %d\n", surface->geometry.x, surface->geometry.y);
		weston_surface_from_global(surface, x, y, sx, sy);
		if (pixman_region32_contains_point(&surface->input,
						   *sx, *sy, NULL))
			return surface;
	}

	return NULL;
}

tWin *	get_shell_surface					(tSurf *surface);
void				activate						(struct wl_shell *shell, tSurf *es, tFocus *device);

void				weston_device_repick				(struct wl_input_device *device)
{
	tFocus *wd = (tFocus *) device;
	const struct wl_pointer_grab_interface *interface;
	tSurf *surface, *focus;

	surface = weston_compositor_pick_surface(wd->compositor,
						 device->x, device->y,
						 &device->current_x,
						 &device->current_y);

	if (&surface->surface != device->current) {
		interface = device->pointer_grab->interface;
		interface->focus(device->pointer_grab, &surface->surface,
				 device->current_x, device->current_y);
		device->current = &surface->surface;
	}

	focus = (tSurf *) device->pointer_grab->focus;
	
	if (focus) {
		tWin* shsurf = get_shell_surface(focus);
	//	if (shsurf)
	//		activate(shsurf->shell, focus, device);
	//	else
			weston_surface_activate(focus, device);
		
		weston_surface_from_global(focus, device->x, device->y,
			   &device->pointer_grab->x, &device->pointer_grab->y);
	}
}

WL_EXPORT void		weston_compositor_repick			(tComp *compositor)
{
	tFocus *device;

	if (!compositor->focus)
		return;

	wl_list_for_each(device, &compositor->input_device_list, link)
		weston_device_repick(&device->input_device);
}

void				weston_surface_unmap				(tSurf *surface)
{
	struct wl_input_device *device = gShell.pEC->input_device;

	weston_surface_damage_below(surface);
	surface->output = NULL;
	wl_list_remove(&surface->link);
	
	if (device->keyboard_focus == &surface->surface)
		wl_input_device_set_keyboard_focus(device, NULL);
	if (device->pointer_focus == &surface->surface)
		wl_input_device_set_pointer_focus(device, NULL, 0, 0);
	
	weston_compositor_schedule_repaint(gShell.pEC);
}

void				destroy_surface					(struct wl_resource *resource)
{
	tSurf *surface =
		container_of(resource,
			     tSurf, surface.resource);
	tComp *compositor = gShell.pEC;

	if (weston_surface_is_mapped(surface))
		weston_surface_unmap(surface);

	if (surface->texture)
		glDeleteTextures(1, &surface->texture);

	if (surface->buffer)
		wl_list_remove(&surface->buffer_destroy_listener.link);

	if (surface->image != EGL_NO_IMAGE_KHR)
		compositor->destroy_image(compositor->display,
					  surface->image);

//	pixman_region32_fini(&surface->transform.boundingbox);
	pixman_region32_fini(&surface->damage);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_fini(&surface->clip);
	if (!region_is_undefined(&surface->input))
		pixman_region32_fini(&surface->input);

	free(surface);
}

WL_EXPORT void		weston_surface_destroy				(tSurf *surface)
{
	/* Not a valid way to destroy a client surface */
	assert(surface->surface.resource.client == NULL);
	destroy_surface(&surface->surface.resource);
}

static void			weston_surface_attach				(struct wl_surface *surface, struct wl_buffer *buffer)
{
	tSurf *es = (tSurf *) surface;
	tComp *ec = gShell.pEC;

	if (es->buffer) {
		weston_buffer_post_release(es->buffer);
		wl_list_remove(&es->buffer_destroy_listener.link);
	}

	es->buffer = buffer;

	if (!buffer) {
		if (weston_surface_is_mapped(es))
			weston_surface_unmap(es);
		return;
	}

	buffer->busy_count++;
	wl_signal_add(&es->buffer->resource.destroy_signal,
		      &es->buffer_destroy_listener);

	if (es->geometry.width != buffer->width ||
	    es->geometry.height != buffer->height) {
		undef_region(&es->input);
		pixman_region32_fini(&es->opaque);
		pixman_region32_init(&es->opaque);
	}

	if (!es->texture) {
		glGenTextures(1, &es->texture);
		glBindTexture(GL_TEXTURE_2D, es->texture);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		es->shader = &ec->texture_shader;
	} else {
		glBindTexture(GL_TEXTURE_2D, es->texture);
	}

	if (wl_buffer_is_shm(buffer)) {
		es->pitch = wl_shm_buffer_get_stride(buffer) / 4;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
			     es->pitch, es->buffer->height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
	} else {
		if (es->image != EGL_NO_IMAGE_KHR)
			ec->destroy_image(ec->display, es->image);
		es->image = ec->create_image(ec->display, NULL,
					     EGL_WAYLAND_BUFFER_WL,
					     buffer, NULL);

		ec->image_target_texture_2d(GL_TEXTURE_2D, es->image);

		es->pitch = buffer->width;
	}
}

static int			texture_region					(tSurf *es, pixman_region32_t *region)
{
	tComp *ec = gShell.pEC;
	GLfloat *v, inv_width, inv_height;
	int32_t sx, sy;
	pixman_box32_t *rectangles;
	unsigned int *p;
	int i, n;

	rectangles = pixman_region32_rectangles(region, &n);
	v = wl_array_add(&ec->vertices, n * 16 * sizeof *v);
	p = wl_array_add(&ec->indices, n * 6 * sizeof *p);
	inv_width = 1.0 / es->pitch;
	inv_height = 1.0 / es->geometry.height;

	for (i = 0; i < n; i++, v += 16, p += 6) {
		weston_surface_from_global (es, rectangles[i].x1,
					  rectangles[i].y1, &sx, &sy);
		v[ 0] = rectangles[i].x1;
		v[ 1] = rectangles[i].y1;
		v[ 2] = sx * inv_width;
		v[ 3] = sy * inv_height;

		weston_surface_from_global (es, rectangles[i].x1,
					  rectangles[i].y2, &sx, &sy);
		v[ 4] = rectangles[i].x1;
		v[ 5] = rectangles[i].y2;
		v[ 6] = sx * inv_width;
		v[ 7] = sy * inv_height;

		weston_surface_from_global (es, rectangles[i].x2,
					  rectangles[i].y1, &sx, &sy);
		v[ 8] = rectangles[i].x2;
		v[ 9] = rectangles[i].y1;
		v[10] = sx * inv_width;
		v[11] = sy * inv_height;

		weston_surface_from_global (es, rectangles[i].x2,
					  rectangles[i].y2, &sx, &sy);
		v[12] = rectangles[i].x2;
		v[13] = rectangles[i].y2;
		v[14] = sx * inv_width;
		v[15] = sy * inv_height;

		p[0] = i * 4 + 0;
		p[1] = i * 4 + 1;
		p[2] = i * 4 + 2;
		p[3] = i * 4 + 2;
		p[4] = i * 4 + 1;
		p[5] = i * 4 + 3;
	}

	return n;
}


WL_EXPORT void		weston_surface_draw				(tSurf *es, tOutput *output, pixman_region32_t *damage)
{
	tComp *ec = gShell.pEC;
	GLfloat *v;
	pixman_region32_t repaint;
	GLint filter;
	int n;

	pixman_region32_init(&repaint);
	
	pixman_region32_t boundingbox;
	pixman_region32_init_rect(&boundingbox,
					  es->geometry.x,
					  es->geometry.y,
					  es->geometry.width,
					  es->geometry.height);
	
	pixman_region32_intersect(&repaint,
				  &boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &es->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);

	if (ec->current_shader != es->shader) {
		glUseProgram(es->shader->program);
		ec->current_shader = es->shader;
	}

	glUniformMatrix4fv(es->shader->proj_uniform,
			   1, GL_FALSE, output->matrix.d);
	glUniform1i(es->shader->tex_uniform, 0);
	glUniform4fv(es->shader->color_uniform, 1, es->color);
	glUniform1f(es->shader->alpha_uniform, es->alpha / 255.0);
	glUniform1f(es->shader->texwidth_uniform,
		    (GLfloat)es->geometry.width / es->pitch);

//	if (es->transform.enabled || output->zoom.active)
//		filter = GL_LINEAR;
//	else
		filter = GL_NEAREST;

	n = texture_region(es, &repaint);

	glBindTexture(GL_TEXTURE_2D, es->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

	v = ec->vertices.data;
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawElements(GL_TRIANGLES, n * 6, GL_UNSIGNED_INT, ec->indices.data);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	ec->vertices.size = 0;
	ec->indices.size = 0;

out:
	pixman_region32_fini(&repaint);
}



WL_EXPORT void		weston_surface_restack				(tSurf *surface, struct wl_list *below)
{
//	wl_list_insert(below, &surface->layer_link);
	
	weston_surface_damage_below (surface);
	weston_surface_damage (surface);
}




WL_EXPORT void		weston_compositor_damage_all			(tComp *compositor)
{
	tOutput *output;

	wl_list_for_each(output, &compositor->output_list, link)
		weston_output_damage(output);
}

WL_EXPORT void		weston_buffer_post_release			(struct wl_buffer *buffer)
{
	if (--buffer->busy_count > 0)
		return;

	assert(buffer->resource.client != NULL);
	wl_resource_queue_event(&buffer->resource, WL_BUFFER_RELEASE);
}





WL_EXPORT void		weston_output_damage				(tOutput *output)
{
	tComp *compositor = output->compositor;

	pixman_region32_union(&compositor->damage,
			      &compositor->damage, &output->region);
	weston_compositor_schedule_repaint(compositor);
}

/*
static void
fade_frame(struct weston_animation *animation,
	   tOutput *output, uint32_t msecs)
{
	tComp *compositor =
		container_of(animation,
			     tComp, fade.animation);
	tSurf *surface;

	surface = compositor->fade.surface;
	weston_spring_update(&compositor->fade.spring, msecs);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0,
				 compositor->fade.spring.current);
	weston_surface_damage(surface);

	if (weston_spring_done(&compositor->fade.spring)) {
		compositor->fade.spring.current =
			compositor->fade.spring.target;
		wl_list_remove(&animation->link);
		wl_list_init(&animation->link);

		if (compositor->fade.spring.current < 0.001) {
			destroy_surface(&surface->surface.resource);
			compositor->fade.surface = NULL;
		} else if (compositor->fade.spring.current > 0.999) {
			compositor->state = WESTON_COMPOSITOR_SLEEPING;
			wl_signal_emit(&compositor->lock_signal, compositor);
		}
	}
}*/

struct weston_frame_callback {
	struct wl_resource resource;
	struct wl_list link;
};

static void			weston_output_repaint				(tOutput *output, int msecs)
{
//	dTrace_E("output %lx", output);
	tComp *ec = output->compositor;
	tSurf *es;
	struct weston_layer *layer;
//	struct weston_animation *animation, *next;
	struct weston_frame_callback *cb, *cnext;
	pixman_region32_t opaque, new_damage, output_damage;
	int32_t width, height;

	weston_compositor_update_drag_surfaces(ec);

	width = output->current->width +
		output->border.left + output->border.right;
	height = output->current->height +
		output->border.top + output->border.bottom;
	glViewport(0, 0, width, height);

	/* Rebuild the surface list and update surface transforms up front. */
	
/*	wl_list_init(&ec->surface_list);
	wl_list_for_each(layer, &ec->layer_list, link) {
		wl_list_for_each(es, &layer->surface_list, layer_link) {
			weston_surface_update_transform(es);
			wl_list_insert(ec->surface_list.prev, &es->link);
		}
	}
	/**/
	
	
	if (output->assign_planes)
		/*
		 * This will queue flips for the fbs and sprites where
		 * applicable and clear the damage for those surfaces.
		 * The repaint loop below will repaint everything
		 * else.
		 */
		output->assign_planes(output);

	pixman_region32_init(&new_damage);
	pixman_region32_init(&opaque);

	wl_list_for_each(es, &ec->surface_list, link) {
		pixman_region32_subtract(&es->damage, &es->damage, &opaque);
		pixman_region32_union(&new_damage, &new_damage, &es->damage);
		empty_region(&es->damage);
		pixman_region32_copy(&es->clip, &opaque);
	//	pixman_region32_union(&opaque, &opaque, &es->transform.opaque);
	}

	pixman_region32_union(&ec->damage, &ec->damage, &new_damage);

	pixman_region32_init(&output_damage);
	pixman_region32_union(&output_damage,
			      &ec->damage, &output->previous_damage);
	pixman_region32_copy(&output->previous_damage, &ec->damage);
	pixman_region32_intersect(&output_damage,
				  &output_damage, &output->region);
	pixman_region32_subtract(&ec->damage, &ec->damage, &output->region);

	pixman_region32_fini(&opaque);
	pixman_region32_fini(&new_damage);

	if (output->dirty)
		weston_output_update_matrix(output);

	output->repaint(output, &output_damage);

	pixman_region32_fini(&output_damage);

	output->repaint_needed = 0;

	weston_compositor_repick(ec);
	wl_event_loop_dispatch(ec->input_loop, 0);

	wl_list_for_each_safe(cb, cnext, &output->frame_callback_list, link) {
		wl_callback_send_done(&cb->resource, msecs);
		wl_resource_destroy(&cb->resource);
	}

//	wl_list_for_each_safe(animation, next, &ec->animation_list, link)
//		animation->frame(animation, output, msecs);
	
//	dTrace_L("");
}

static int			weston_compositor_read_input			(int fd, uint32_t mask, void *data)
{
	tComp *compositor = data;

	wl_event_loop_dispatch(compositor->input_loop, 0);

	return 1;
}

WL_EXPORT void		weston_output_finish_frame			(tOutput *output, int msecs)
{
	tComp *compositor = output->compositor;
	struct wl_event_loop *loop =
		wl_display_get_event_loop(compositor->wl_display);
	int fd;

	if (output->repaint_needed) {
		weston_output_repaint(output, msecs);
		return;
	}

	output->repaint_scheduled = 0;
	if (compositor->input_loop_source)
		return;

	fd = wl_event_loop_get_fd(compositor->input_loop);
	compositor->input_loop_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				     weston_compositor_read_input, compositor);
}

static void			idle_repaint					(void *data)
{
	tOutput *output = data;

	weston_output_finish_frame(output, weston_compositor_get_time());
}

/*
WL_EXPORT void		weston_layer_init					(struct weston_layer *layer, struct wl_list *below)
{
	wl_list_init(&layer->surface_list);
	wl_list_insert(below, &layer->link);
}
*/
WL_EXPORT void
weston_compositor_schedule_repaint(tComp *compositor)
{
	tOutput *output;
	struct wl_event_loop *loop;

	if (compositor->state == WESTON_COMPOSITOR_SLEEPING)
		return;

	loop = wl_display_get_event_loop(compositor->wl_display);
	wl_list_for_each(output, &compositor->output_list, link) {
		output->repaint_needed = 1;
		if (output->repaint_scheduled)
			continue;

		wl_event_loop_add_idle(loop, idle_repaint, output);
		output->repaint_scheduled = 1;
	}

	if (compositor->input_loop_source) {
		wl_event_source_remove(compositor->input_loop_source);
		compositor->input_loop_source = NULL;
	}
}

/*
WL_EXPORT void
weston_compositor_fade(tComp *compositor, float tint)
{
	tSurf *surface;
	int done;

	done = weston_spring_done(&compositor->fade.spring);
	compositor->fade.spring.target = tint;
	if (weston_spring_done(&compositor->fade.spring))
		return;

	if (done)
		compositor->fade.spring.timestamp =
			weston_compositor_get_time();

	if (compositor->fade.surface == NULL) {
		surface = weston_surface_create(compositor);
		weston_surface_configure(surface, 0, 0, 8192, 8192);
		weston_surface_set_color(surface, 0.0, 0.0, 0.0, 0.0);
		wl_list_insert(&compositor->fade_layer.surface_list,
			       &surface->layer_link);
		weston_surface_assign_output(surface);
		compositor->fade.surface = surface;
		pixman_region32_init(&surface->input);
	}

	weston_surface_damage(compositor->fade.surface);
	if (wl_list_empty(&compositor->fade.animation.link))
		wl_list_insert(compositor->animation_list.prev,
			       &compositor->fade.animation.link);
}*/

static void
surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

WL_EXPORT void
weston_surface_assign_output(tSurf *es)
{
	tComp *ec = gShell.pEC;
	tOutput *output, *new_output;
	pixman_region32_t region;
	uint32_t max, area;
	pixman_box32_t *e;

	weston_surface_update_transform(es);

	new_output = NULL;
	max = 0;
	pixman_region32_init(&region);
	wl_list_for_each(output, &ec->output_list, link) {
		pixman_region32_t boundingbox;
		pixman_region32_init_rect(&boundingbox,
						  es->geometry.x,
						  es->geometry.y,
						  es->geometry.width,
						  es->geometry.height);
		
		pixman_region32_intersect(&region, &boundingbox,
					  &output->region);

		e = pixman_region32_extents(&region);
		area = (e->x2 - e->x1) * (e->y2 - e->y1);

		if (area >= max) {
			new_output = output;
			max = area;
		}
	}
	pixman_region32_fini(&region);

	es->output = new_output;
	if (!wl_list_empty(&es->frame_callback_list)) {
		wl_list_insert_list(new_output->frame_callback_list.prev,
				    &es->frame_callback_list);
		wl_list_init(&es->frame_callback_list);
	}
}

static void
surface_attach(struct wl_client *client,
	       struct wl_resource *resource,
	       struct wl_resource *buffer_resource, int32_t sx, int32_t sy)
{
	tSurf *es = resource->data;
	struct wl_buffer *buffer = NULL;

	if (buffer_resource)
		buffer = buffer_resource->data;

	weston_surface_attach(&es->surface, buffer);

	if (buffer && es->configure)
		es->configure(es, sx, sy);
}

static void
surface_damage(struct wl_client *client,
	       struct wl_resource *resource,
	       int32_t x, int32_t y, int32_t width, int32_t height)
{
	tSurf *es = resource->data;

	weston_surface_damage_rectangle(es, x, y, width, height);

	if (es->buffer && wl_buffer_is_shm(es->buffer)) {
		glBindTexture(GL_TEXTURE_2D, es->texture);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, es->pitch);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, y);

		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height,
				GL_BGRA_EXT, GL_UNSIGNED_BYTE,
				wl_shm_buffer_get_data(es->buffer));
	}
}

static void
destroy_frame_callback(struct wl_resource *resource)
{
	struct weston_frame_callback *cb = resource->data;

	wl_list_remove(&cb->link);
	free(cb);
}

static void
surface_frame(struct wl_client *client,
	      struct wl_resource *resource, uint32_t callback)
{
	struct weston_frame_callback *cb;
	tSurf *es = resource->data;

	cb = malloc(sizeof *cb);
	if (cb == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}
		
	cb->resource.object.interface = &wl_callback_interface;
	cb->resource.object.id = callback;
	cb->resource.destroy = destroy_frame_callback;
	cb->resource.client = client;
	cb->resource.data = cb;

	wl_client_add_resource(client, &cb->resource);

	if (es->output) {
		wl_list_insert(es->output->frame_callback_list.prev,
			       &cb->link);
	} else {
		wl_list_insert(es->frame_callback_list.prev, &cb->link);
	}
	
//	shell_restack ();
}

static void
surface_set_opaque_region(struct wl_client *client,
			  struct wl_resource *resource,
			  struct wl_resource *region_resource)
{
	tSurf *surface = resource->data;
	struct weston_region *region;

	pixman_region32_fini(&surface->opaque);

	if (region_resource) {
		region = region_resource->data;
		pixman_region32_init_rect(&surface->opaque, 0, 0,
					  surface->geometry.width,
					  surface->geometry.height);
		pixman_region32_intersect(&surface->opaque,
					  &surface->opaque, &region->region);
	} else {
		pixman_region32_init(&surface->opaque);
	}

	surface->geometry.dirty = 1;
	
//	shell_restack ();
}

static void
surface_set_input_region(struct wl_client *client,
			 struct wl_resource *resource,
			 struct wl_resource *region_resource)
{
	tSurf *surface = resource->data;
	struct weston_region *region;

	if (region_resource) {
		region = region_resource->data;
		pixman_region32_init_rect(&surface->input, 0, 0,
					  surface->geometry.width,
					  surface->geometry.height);
		pixman_region32_intersect(&surface->input,
					  &surface->input, &region->region);
	} else {
		pixman_region32_init_rect(&surface->input, 0, 0,
					  surface->geometry.width,
					  surface->geometry.height);
	}
	int restack = 0;
	if (surface->border.left == 0 && surface->input.extents.x1 != 0) {
		printf ("THIS IS IT  THIS IS IT  THIS IS IT  THIS IS IT  THIS IS IT  THIS IS IT  THIS IS IT  THIS IS IT  THIS IS IT  THIS IS IT\n");
		restack = 1;
	}
	surface->border.left = surface->input.extents.x1;
	surface->border.top = surface->input.extents.y1;
	
	surface->border.right = surface->geometry.width - surface->input.extents.x2;
	surface->border.bottom = surface->geometry.height - surface->input.extents.y2;
	
	if (restack)
		shell_restack ();
//	weston_compositor_schedule_repaint(gShell.pEC);
}

static const struct wl_surface_interface surface_interface = {
	surface_destroy,
	surface_attach,
	surface_damage,
	surface_frame,
	surface_set_opaque_region,
	surface_set_input_region
};

static void
compositor_create_surface(struct wl_client *client,
			  struct wl_resource *resource, uint32_t id)
{
	dTrace_E("");
	tComp *ec = resource->data;
	tSurf *surface;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}
	surface->client = client;
	surface->surface.resource.destroy = destroy_surface;

	surface->surface.resource.object.id = id;
	surface->surface.resource.object.interface = &wl_surface_interface;
	surface->surface.resource.object.implementation =
		(void (**)(void)) &surface_interface;
	surface->surface.resource.data = surface;

	wl_client_add_resource(client, &surface->surface.resource);
	dTrace_L("");
}

static void
destroy_region(struct wl_resource *resource)
{
	struct weston_region *region =
		container_of(resource, struct weston_region, resource);

	pixman_region32_fini(&region->region);
	free(region);
}

static void
region_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
region_add(struct wl_client *client, struct wl_resource *resource,
	   int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_region *region = resource->data;

	pixman_region32_union_rect(&region->region, &region->region,
				   x, y, width, height);
}

static void
region_subtract(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_region *region = resource->data;
	pixman_region32_t rect;

	pixman_region32_init_rect(&rect, x, y, width, height);
	pixman_region32_subtract(&region->region, &region->region, &rect);
	pixman_region32_fini(&rect);
}

static const struct wl_region_interface region_interface = {
	region_destroy,
	region_add,
	region_subtract
};

static void
compositor_create_region(struct wl_client *client,
			 struct wl_resource *resource, uint32_t id)
{
	struct weston_region *region;

	region = malloc(sizeof *region);
	if (region == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	region->resource.destroy = destroy_region;

	region->resource.object.id = id;
	region->resource.object.interface = &wl_region_interface;
	region->resource.object.implementation =
		(void (**)(void)) &region_interface;
	region->resource.data = region;

	pixman_region32_init(&region->region);

	wl_client_add_resource(client, &region->resource);
}

static const struct wl_compositor_interface compositor_interface = {
	compositor_create_surface,
	compositor_create_region
};

WL_EXPORT void
weston_compositor_wake(tComp *compositor)
{
	compositor->state = WESTON_COMPOSITOR_ACTIVE;
//	weston_compositor_fade(compositor, 0.0);

	wl_event_source_timer_update(compositor->idle_source,
				     compositor->idle_time * 1000);
}

static void
weston_compositor_dpms_on(tComp *compositor)
{
        tOutput *output;

        wl_list_for_each(output, &compositor->output_list, link)
		if (output->set_dpms)
			output->set_dpms(output, WESTON_DPMS_ON);
}

WL_EXPORT void
weston_compositor_activity(tComp *compositor)
{
	if (compositor->state == WESTON_COMPOSITOR_ACTIVE) {
		weston_compositor_wake(compositor);
	} else {
		weston_compositor_dpms_on(compositor);
		wl_signal_emit(&compositor->unlock_signal, compositor);
	}
}

static void
weston_compositor_idle_inhibit(tComp *compositor)
{
	weston_compositor_activity(compositor);
	compositor->idle_inhibit++;
}

static void
weston_compositor_idle_release(tComp *compositor)
{
	compositor->idle_inhibit--;
	weston_compositor_activity(compositor);
}

static int
idle_handler(void *data)
{
	tComp *compositor = data;

	if (compositor->idle_inhibit)
		return 1;

//	weston_compositor_fade(compositor, 1.0);

	return 1;
}

static  void
weston_input_update_drag_surface(struct wl_input_device *input_device,
				 int dx, int dy);

WL_EXPORT void
notify_motion(struct wl_input_device *device, uint32_t time, int x, int y)
{
	tOutput *output;
	const struct wl_pointer_grab_interface *interface;
	tFocus *wd = (tFocus *) device;
	tComp *ec = wd->compositor;
	int x_valid = 0, y_valid = 0;
	int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;

	weston_compositor_activity(ec);

	wl_list_for_each(output, &ec->output_list, link) {
		if (output->x <= x && x < output->x + output->current->width)
			x_valid = 1;

		if (output->y <= y && y < output->y + output->current->height)
			y_valid = 1;

		/* FIXME: calculate this only on output addition/deletion */
		if (output->x < min_x)
			min_x = output->x;
		if (output->y < min_y)
			min_y = output->y;

		if (output->x + output->current->width > max_x)
			max_x = output->x + output->current->width - 1;
		if (output->y + output->current->height > max_y)
			max_y = output->y + output->current->height - 1;
	}
	
	if (!x_valid) {
		if (x < min_x)
			x = min_x;
		else if (x >= max_x)
			x = max_x;
	}
	if (!y_valid) {
		if (y < min_y)
			y = min_y;
		else  if (y >= max_y)
			y = max_y;
	}

	weston_input_update_drag_surface(device,
					 x - device->x, y - device->y);

	device->x = x;
	device->y = y;

	wl_list_for_each(output, &ec->output_list, link)
		if (output->zoom.active &&
		    pixman_region32_contains_point(&output->region, x, y, NULL))
			weston_output_update_zoom(output, x, y);

	weston_device_repick(device);
	interface = device->pointer_grab->interface;
	interface->motion(device->pointer_grab, time,
			  device->pointer_grab->x, device->pointer_grab->y);

	if (wd->sprite) {
		weston_surface_set_position(wd->sprite,
					    device->x - wd->hotspot_x,
					    device->y - wd->hotspot_y);
		
		weston_surface_update_transform(wd->sprite);
		weston_compositor_schedule_repaint(ec);
	}
}

WL_EXPORT void
weston_surface_activate(tSurf *surface,
			tFocus *device)
{
	tComp *compositor = device->compositor;

	wl_input_device_set_keyboard_focus(&device->input_device,
					   &surface->surface);
	wl_data_device_set_keyboard_focus(&device->input_device);

	wl_signal_emit(&compositor->activate_signal, surface);
}

WL_EXPORT void
notify_button(struct wl_input_device *device,
	      uint32_t time, int32_t button, int32_t state)
{
	tFocus *wd = (tFocus *) device;
	tComp *compositor = wd->compositor;

	if (state) {
		weston_compositor_idle_inhibit(compositor);
		if (device->button_count == 0) {
			device->grab_button = button;
			device->grab_time = time;
			device->grab_x = device->x;
			device->grab_y = device->y;
		}
		device->button_count++;
	} else {
		weston_compositor_idle_release(compositor);
		device->button_count--;
	}

	weston_compositor_run_binding(compositor, wd, time, 0, button, 0, state);

	device->pointer_grab->interface->button(device->pointer_grab, time, button, state);

	if (device->button_count == 1)
		device->grab_serial =
			wl_display_get_serial(compositor->wl_display);
}

WL_EXPORT void
notify_axis(struct wl_input_device *device,
	      uint32_t time, uint32_t axis, int32_t value)
{
	tFocus *wd = (tFocus *) device;
	tComp *compositor = wd->compositor;

	weston_compositor_activity(compositor);

	if (value)
		weston_compositor_run_binding(compositor, wd,
						time, 0, 0, axis, value);
	else
		return;

	if (device->pointer_focus_resource)
		wl_resource_post_event(device->pointer_focus_resource,
				WL_INPUT_DEVICE_AXIS, time, axis, value);
}

static void
update_modifier_state(tFocus *device,
		      uint32_t key, uint32_t state)
{
	uint32_t modifier;

	switch (key) {
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		modifier = MODIFIER_CTRL;
		break;

	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		modifier = MODIFIER_ALT;
		break;

	case KEY_LEFTMETA:
	case KEY_RIGHTMETA:
		modifier = MODIFIER_SUPER;
		break;

	default:
		modifier = 0;
		break;
	}

	if (state)
		device->modifier_state |= modifier;
	else
		device->modifier_state &= ~modifier;
}

WL_EXPORT void
notify_key(struct wl_input_device *device,
	   uint32_t time, uint32_t key, uint32_t state)
{
	tFocus *wd = (tFocus *) device;
	tComp *compositor = wd->compositor;
	uint32_t *k, *end;

	if (state) {
		weston_compositor_idle_inhibit(compositor);
		device->grab_key = key;
		device->grab_time = time;
	} else {
		weston_compositor_idle_release(compositor);
	}

	update_modifier_state(wd, key, state);
	end = device->keys.data + device->keys.size;
	for (k = device->keys.data; k < end; k++) {
		if (*k == key)
			*k = *--end;
	}
	device->keys.size = (void *) end - device->keys.data;
	if (state) {
		k = wl_array_add(&device->keys, sizeof *k);
		*k = key;
	}

	if (device->keyboard_grab == &device->default_keyboard_grab)
		weston_compositor_run_binding(compositor, wd,
					      time, key, 0, 0, state);

	device->keyboard_grab->interface->key(device->keyboard_grab,
					      time, key, state);
}

WL_EXPORT void
notify_pointer_focus(struct wl_input_device *device,
		     tOutput *output, int32_t x, int32_t y)
{
	tFocus *wd = (tFocus *) device;
	tComp *compositor = wd->compositor;

	if (output) {
		weston_input_update_drag_surface(device, x - device->x,
						 y - device->y);

		device->x = x;
		device->y = y;
		compositor->focus = 1;
		weston_compositor_repick(compositor);
	} else {
		compositor->focus = 0;
		weston_compositor_repick(compositor);
	}
}

static void
destroy_device_saved_kbd_focus(struct wl_listener *listener, void *data)
{
	tFocus *wd;

	wd = container_of(listener, tFocus,
			  saved_kbd_focus_listener);

	wd->saved_kbd_focus = NULL;
}

WL_EXPORT void
notify_keyboard_focus(struct wl_input_device *device, struct wl_array *keys)
{
	tFocus *wd =
		(tFocus *) device;
	tComp *compositor = wd->compositor;
	struct wl_surface *surface;
	uint32_t *k;

	if (keys) {
		wl_array_copy(&wd->input_device.keys, keys);
		wd->modifier_state = 0;
		wl_array_for_each(k, &device->keys) {
			weston_compositor_idle_inhibit(compositor);
			update_modifier_state(wd, *k, 1);
		}

		surface = wd->saved_kbd_focus;

		if (surface) {
			wl_list_remove(&wd->saved_kbd_focus_listener.link);
			wl_input_device_set_keyboard_focus(&wd->input_device,
							   surface);
			wd->saved_kbd_focus = NULL;
		}
	} else {
		wl_array_for_each(k, &device->keys)
			weston_compositor_idle_release(compositor);

		wd->modifier_state = 0;

		surface = wd->input_device.keyboard_focus;

		if (surface) {
			wd->saved_kbd_focus = surface;
			wd->saved_kbd_focus_listener.notify =
				destroy_device_saved_kbd_focus;
			wl_signal_add(&surface->resource.destroy_signal,
				      &wd->saved_kbd_focus_listener);
		}

		wl_input_device_set_keyboard_focus(&wd->input_device, NULL);
		/* FIXME: We really need keyboard grab cancel here to
		 * let the grab shut down properly.  As it is we leak
		 * the grab data. */
		wl_input_device_end_keyboard_grab(&wd->input_device);
	}
}

/* TODO: share this function with wayland-server.c */
static struct wl_resource *
find_resource_for_surface(struct wl_list *list, struct wl_surface *surface)
{
        struct wl_resource *r;

        if (!surface)
                return NULL;

        wl_list_for_each(r, list, link) {
                if (r->client == surface->resource.client)
                        return r;
        }

        return NULL;
}

static void
lose_touch_focus_resource(struct wl_listener *listener, void *data)
{
	tFocus *device =
		container_of(listener, tFocus,
			     touch_focus_resource_listener);

	device->touch_focus_resource = NULL;
}

static void
lose_touch_focus(struct wl_listener *listener, void *data)
{
	tFocus *device =
		container_of(listener, tFocus,
			     touch_focus_listener);

	device->touch_focus = NULL;
}

static void
touch_set_focus(tFocus *device,
		struct wl_surface *surface)
{
	struct wl_input_device *input_device = &device->input_device;
	struct wl_resource *resource;

	if (device->touch_focus == surface)
		return;

	if (surface) {
		resource =
			find_resource_for_surface(&input_device->resource_list,
						  surface);
		if (!resource) {
			fprintf(stderr, "couldn't find resource\n");
			return;
		}

		device->touch_focus_resource_listener.notify =
			lose_touch_focus_resource;
		wl_signal_add(&resource->destroy_signal,
			      &device->touch_focus_resource_listener);
		device->touch_focus_listener.notify = lose_touch_focus;
		wl_signal_add(&surface->resource.destroy_signal,
			       &device->touch_focus_listener);

		device->touch_focus = surface;
		device->touch_focus_resource = resource;
	} else {
		if (device->touch_focus)
			wl_list_remove(&device->touch_focus_listener.link);
		if (device->touch_focus_resource)
			wl_list_remove(&device->touch_focus_resource_listener.link);
		device->touch_focus = NULL;
		device->touch_focus_resource = NULL;
	}
}

/**
 * notify_touch - emulates button touches and notifies surfaces accordingly.
 *
 * It assumes always the correct cycle sequence until it gets here: touch_down
 * → touch_update → ... → touch_update → touch_end. The driver is responsible
 * for sending along such order.
 *
 */
WL_EXPORT void
notify_touch(struct wl_input_device *device, uint32_t time, int touch_id,
             int x, int y, int touch_type)
{
	tFocus *wd = (tFocus *) device;
	tComp *ec = wd->compositor;
	tSurf *es;
	int32_t sx, sy;
	uint32_t serial = 0;

	switch (touch_type) {
	case WL_INPUT_DEVICE_TOUCH_DOWN:
		weston_compositor_idle_inhibit(ec);

		wd->num_tp++;

		/* the first finger down picks the surface, and all further go
		 * to that surface for the remainder of the touch session i.e.
		 * until all touch points are up again. */
		if (wd->num_tp == 1) {
			es = weston_compositor_pick_surface(ec, x, y, &sx, &sy);
			touch_set_focus(wd, &es->surface);
		} else if (wd->touch_focus) {
			es = (tSurf *) wd->touch_focus;
			weston_surface_from_global(es, x, y, &sx, &sy);
		}

		if (wd->touch_focus_resource && wd->touch_focus)
			wl_input_device_send_touch_down(wd->touch_focus_resource,
							serial, time, &wd->touch_focus->resource,
							touch_id, sx, sy);
		break;
	case WL_INPUT_DEVICE_TOUCH_MOTION:
		es = (tSurf *) wd->touch_focus;
		if (!es)
			break;

		weston_surface_from_global(es, x, y, &sx, &sy);
		if (wd->touch_focus_resource)
			wl_input_device_send_touch_motion(wd->touch_focus_resource,
							  time, touch_id, sx, sy);
		break;
	case WL_INPUT_DEVICE_TOUCH_UP:
		weston_compositor_idle_release(ec);
		wd->num_tp--;

		if (wd->touch_focus_resource)
			wl_input_device_send_touch_up(wd->touch_focus_resource,
						      serial, time, touch_id);
		if (wd->num_tp == 0)
			touch_set_focus(wd, NULL);
		break;
	}
}

static void
input_device_attach(struct wl_client *client,
		    struct wl_resource *resource,
		    uint32_t serial,
		    struct wl_resource *buffer_resource, int32_t x, int32_t y)
{
//	dTrace_E("");
	tFocus *device = resource->data;
	tComp *compositor = device->compositor;
	struct wl_buffer *buffer = NULL;

	if (serial < device->input_device.pointer_focus_serial)
		return;
	if (device->input_device.pointer_focus == NULL)
		return;
	if (device->input_device.pointer_focus->resource.client != client)
		return;

	if (buffer_resource)
		buffer = buffer_resource->data;

//	printf ("!!!!!!!!!!!! input_device_attach buffer %lx\n", buffer);
	weston_surface_attach(&device->sprite->surface, buffer);

	if (!buffer)
		return;

//	if (!weston_surface_is_mapped(device->sprite)) {
	//	wl_list_insert(&compositor->cursor_layer.surface_list,
	//		       &device->sprite->layer_link);
		weston_surface_assign_output(device->sprite);
	//	device->sprite->output = container_of(gShell.pEC->output_list.next, tOutput, link);
	//	printf ("!!!!!!!!!!!! input_device_attach %lx\n", device->sprite->output);
//	}
	

	device->hotspot_x = x;
	device->hotspot_y = y;
	weston_surface_configure(device->sprite,
				 device->input_device.x - device->hotspot_x,
				 device->input_device.y - device->hotspot_y,
				 buffer->width, buffer->height);
	
	surface_damage(NULL, &device->sprite->surface.resource,
		       0, 0, buffer->width, buffer->height);
}

static const struct wl_input_device_interface input_device_interface = {
	input_device_attach,
};

static void
handle_drag_surface_destroy(struct wl_listener *listener, void *data)
{
	tFocus *device;

	device = container_of(listener, tFocus,
			      drag_surface_destroy_listener);

	device->drag_surface = NULL;
}

static void unbind_input_device(struct wl_resource *resource)
{
	wl_list_remove(&resource->link);
	free(resource);
}

static void
bind_input_device(struct wl_client *client,
		  void *data, uint32_t version, uint32_t id)
{
	struct wl_input_device *device = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &wl_input_device_interface,
					&input_device_interface, id, data);
	wl_list_insert(&device->resource_list, &resource->link);
	resource->destroy = unbind_input_device;
}

static void
device_handle_new_drag_icon(struct wl_listener *listener, void *data)
{
	tFocus *device;

	device = container_of(listener, tFocus,
			      new_drag_icon_listener);

	weston_input_update_drag_surface(&device->input_device, 0, 0);
}

WL_EXPORT void
weston_input_device_init(tFocus *device,
			 tComp *ec)
{
	wl_input_device_init(&device->input_device);

	wl_display_add_global(ec->wl_display, &wl_input_device_interface,
			      device, bind_input_device);

	device->sprite = weston_surface_create(ec);
	device->sprite->surface.resource.data = device->sprite;
	
	device->compositor = ec;
	device->hotspot_x = 16;
	device->hotspot_y = 16;
	device->modifier_state = 0;
	device->num_tp = 0;

	device->drag_surface_destroy_listener.notify =
		handle_drag_surface_destroy;

	wl_list_insert(ec->input_device_list.prev, &device->link);

	device->new_drag_icon_listener.notify = device_handle_new_drag_icon;
	wl_signal_add(&device->input_device.drag_icon_signal,
		      &device->new_drag_icon_listener);
}

WL_EXPORT void
weston_input_device_release(tFocus *device)
{
	wl_list_remove(&device->link);
	/* The global object is destroyed at wl_display_destroy() time. */

	if (device->sprite)
		destroy_surface(&device->sprite->surface.resource);

	wl_input_device_release(&device->input_device);
}

static void
drag_surface_configure(tSurf *es, int32_t sx, int32_t sy)
{
	weston_surface_configure(es,
				 es->geometry.x + sx, es->geometry.y + sy,
				 es->buffer->width, es->buffer->height);
}

static int
device_setup_new_drag_surface(tFocus *device,
			      tSurf *surface)
{
	struct wl_input_device *input_device = &device->input_device;

	if (surface->configure) {
		wl_resource_post_error(&surface->surface.resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->configure already set");
		return 0;
	}

	device->drag_surface = surface;

	weston_surface_set_position(device->drag_surface,
				    input_device->x, input_device->y);

	surface->configure = drag_surface_configure;

	wl_signal_add(&surface->surface.resource.destroy_signal,
		       &device->drag_surface_destroy_listener);

	return 1;
}

static void
device_release_drag_surface(tFocus *device)
{
	device->drag_surface->configure = NULL;
	undef_region(&device->drag_surface->input);
	wl_list_remove(&device->drag_surface_destroy_listener.link);
	device->drag_surface = NULL;
}

static void
device_map_drag_surface(tFocus *device)
{
	if (weston_surface_is_mapped(device->drag_surface) || !device->drag_surface->buffer)
		return;
	
//	wl_list_insert(&device->sprite->layer_link, &device->drag_surface->layer_link);
	
	weston_surface_assign_output(device->drag_surface);
	empty_region(&device->drag_surface->input);
}

static  void
weston_input_update_drag_surface(struct wl_input_device *input_device,
				 int dx, int dy)
{
	int surface_changed = 0;
	tFocus *device = (tFocus *)
		input_device;

	if (!device->drag_surface && !input_device->drag_surface)
		return;

	if (device->drag_surface && input_device->drag_surface &&
	    (&device->drag_surface->surface.resource !=
	     &input_device->drag_surface->resource))
		/* between calls to this funcion we got a new drag_surface */
		surface_changed = 1;

	if (!input_device->drag_surface || surface_changed) {
		device_release_drag_surface(device);
		if (!surface_changed)
			return;
	}

	if (!device->drag_surface || surface_changed) {
		tSurf *surface = (tSurf *)
			input_device->drag_surface;
		if (!device_setup_new_drag_surface(device, surface))
			return;
	}

	/* the client may not have attached a buffer to the drag surface
	 * when we setup it up, so check if map is needed on every update */
	device_map_drag_surface(device);

	/* the client may have attached a buffer with a different size to
	 * the drag surface, causing the input region to be reset */
	if (region_is_undefined(&device->drag_surface->input))
		empty_region(&device->drag_surface->input);

	if (!dx && !dy)
		return;

	weston_surface_set_position(device->drag_surface,
				    device->drag_surface->geometry.x + dx,
				    device->drag_surface->geometry.y + dy);
}

WL_EXPORT void
weston_compositor_update_drag_surfaces(tComp *compositor)
{
	weston_input_update_drag_surface(compositor->input_device, 0, 0);
}

static void
bind_output(struct wl_client *client,
	    void *data, uint32_t version, uint32_t id)
{
	tOutput *output = data;
	struct weston_mode *mode;
	struct wl_resource *resource;

	resource = wl_client_add_object(client,
					&wl_output_interface, NULL, id, data);

	wl_output_send_geometry(resource,
				output->x,
				output->y,
				output->mm_width,
				output->mm_height,
				output->subpixel,
				output->make, output->model);

	wl_list_for_each (mode, &output->mode_list, link) {
		wl_output_send_mode(resource,
				    mode->flags,
				    mode->width,
				    mode->height,
				    mode->refresh);
	}
}

static const char vertex_shader[] =
	"uniform mat4 proj;\n"
	"attribute vec2 position;\n"
	"attribute vec2 texcoord;\n"
	"varying vec2 v_texcoord;\n"
	"void main()\n"
	"{\n"
	"   gl_Position = proj * vec4(position, 0.0, 1.0);\n"
	"   v_texcoord = texcoord;\n"
	"}\n";

static const char texture_fragment_shader[] =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"uniform float texwidth;\n"
	"void main()\n"
	"{\n"
	"   if (v_texcoord.x < 0.0 || v_texcoord.x > texwidth ||\n"
	"       v_texcoord.y < 0.0 || v_texcoord.y > 1.0)\n"
	"      discard;\n"
	"   gl_FragColor = texture2D(tex, v_texcoord)\n;"
	"   gl_FragColor = alpha * gl_FragColor;\n"
	"}\n";

static const char solid_fragment_shader[] =
	"precision mediump float;\n"
	"uniform vec4 color;\n"
	"uniform float alpha;\n"
	"void main()\n"
	"{\n"
	"   gl_FragColor = alpha * color\n;"
	"}\n";

static int
compile_shader(GLenum type, const char *source)
{
	GLuint s;
	char msg[512];
	GLint status;

	s = glCreateShader(type);
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(s, sizeof msg, NULL, msg);
		fprintf(stderr, "shader info: %s\n", msg);
		return GL_NONE;
	}

	return s;
}

static int
weston_shader_init(struct weston_shader *shader,
		   const char *vertex_source, const char *fragment_source)
{
	char msg[512];
	GLint status;

	shader->vertex_shader =
		compile_shader(GL_VERTEX_SHADER, vertex_source);
	shader->fragment_shader =
		compile_shader(GL_FRAGMENT_SHADER, fragment_source);

	shader->program = glCreateProgram();
	glAttachShader(shader->program, shader->vertex_shader);
	glAttachShader(shader->program, shader->fragment_shader);
	glBindAttribLocation(shader->program, 0, "position");
	glBindAttribLocation(shader->program, 1, "texcoord");

	glLinkProgram(shader->program);
	glGetProgramiv(shader->program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(shader->program, sizeof msg, NULL, msg);
		fprintf(stderr, "link info: %s\n", msg);
		return -1;
	}

	shader->proj_uniform = glGetUniformLocation(shader->program, "proj");
	shader->tex_uniform = glGetUniformLocation(shader->program, "tex");
	shader->alpha_uniform = glGetUniformLocation(shader->program, "alpha");
	shader->color_uniform = glGetUniformLocation(shader->program, "color");
	shader->texwidth_uniform = glGetUniformLocation(shader->program,
							"texwidth");

	return 0;
}

WL_EXPORT void
weston_output_destroy(tOutput *output)
{
	tComp *c = output->compositor;

	pixman_region32_fini(&output->region);
	pixman_region32_fini(&output->previous_damage);

	wl_display_remove_global(c->wl_display, output->global);
}

WL_EXPORT void
weston_output_update_zoom(tOutput *output, int x, int y)
{
	float ratio;

	if (output->zoom.level <= 0)
		return;

	output->zoom.magnification = 1 / output->zoom.level;
	ratio = 1 - (1 / output->zoom.magnification);

	output->zoom.trans_x = (((float)(x - output->x) / output->current->width) * (ratio * 2)) - ratio;
	output->zoom.trans_y = (((float)(y - output->y) / output->current->height) * (ratio * 2)) - ratio;

	output->dirty = 1;
	weston_output_damage(output);
}

WL_EXPORT void
weston_output_update_matrix(tOutput *output)
{
	int flip;
	struct weston_matrix camera;
	struct weston_matrix modelview;

	weston_matrix_init(&output->matrix);
	weston_matrix_translate(&output->matrix,
				-(output->x + (output->border.right + output->current->width - output->border.left) / 2.0),
				-(output->y + (output->border.bottom + output->current->height - output->border.top) / 2.0), 0);

	flip = (output->flags & WL_OUTPUT_FLIPPED) ? -1 : 1;
	weston_matrix_scale(&output->matrix,
			    2.0 / (output->current->width + output->border.left + output->border.right),
			    flip * 2.0 / (output->current->height + output->border.top + output->border.bottom), 1);
	if (output->zoom.active) {
		weston_matrix_init(&camera);
		weston_matrix_init(&modelview);
		weston_matrix_translate(&camera, output->zoom.trans_x, flip * output->zoom.trans_y, 0);
		weston_matrix_invert(&modelview, &camera);
		weston_matrix_scale(&modelview, output->zoom.magnification, output->zoom.magnification, 1.0);
		weston_matrix_multiply(&output->matrix, &modelview);
	}

	output->dirty = 0;
}

WL_EXPORT void
weston_output_move(tOutput *output, int x, int y)
{
	output->x = x;
	output->y = y;

	pixman_region32_init(&output->previous_damage);
	pixman_region32_init_rect(&output->region, x, y,
				  output->current->width,
				  output->current->height);
}

WL_EXPORT void
weston_output_init(tOutput *output, tComp *c,
		   int x, int y, int width, int height, uint32_t flags)
{
	output->compositor = c;
	output->x = x;
	output->y = y;
	output->border.top = 0;
	output->border.bottom = 0;
	output->border.left = 0;
	output->border.right = 0;
	output->mm_width = width;
	output->mm_height = height;
	output->dirty = 1;

	output->zoom.active = 0;
	output->zoom.increment = 0.05;
	output->zoom.level = 1.0;
	output->zoom.magnification = 1.0;
	output->zoom.trans_x = 0.0;
	output->zoom.trans_y = 0.0;

	output->flags = flags;
	weston_output_move(output, x, y);
	weston_output_damage(output);

	wl_list_init(&output->frame_callback_list);

	output->global =
		wl_display_add_global(c->wl_display, &wl_output_interface,
				      output, bind_output);
	
	{
		static tTags s_tags = 1;
		output->Tags = s_tags;
		s_tags <<= 1;
	}
	
	wl_list_init(&output->surfaces);
}

static void
compositor_bind(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	tComp *compositor = data;

	wl_client_add_object(client, &wl_compositor_interface,
			     &compositor_interface, id, compositor);
}

WL_EXPORT int
weston_compositor_init(tComp *ec, struct wl_display *display)
{
	struct wl_event_loop *loop;
	const char *extensions;

	ec->wl_display = display;
	wl_signal_init(&ec->destroy_signal);
	wl_signal_init(&ec->activate_signal);
	wl_signal_init(&ec->lock_signal);
	wl_signal_init(&ec->unlock_signal);
	ec->launcher_sock = weston_environment_get_fd("WESTON_LAUNCHER_SOCK");

	if (!wl_display_add_global(display, &wl_compositor_interface,
				   ec, compositor_bind))
		return -1;

	wl_display_init_shm(display);

	ec->image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	ec->image_target_renderbuffer_storage = (void *)
		eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	ec->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	ec->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");
	ec->bind_display =
		(void *) eglGetProcAddress("eglBindWaylandDisplayWL");
	ec->unbind_display =
		(void *) eglGetProcAddress("eglUnbindWaylandDisplayWL");

	extensions = (const char *) glGetString(GL_EXTENSIONS);
	if (!strstr(extensions, "GL_EXT_texture_format_BGRA8888")) {
		fprintf(stderr,
			"GL_EXT_texture_format_BGRA8888 not available\n");
		return -1;
	}

	if (!strstr(extensions, "GL_EXT_read_format_bgra")) {
		fprintf(stderr, "GL_EXT_read_format_bgra not available\n");
		return -1;
	}

	if (!strstr(extensions, "GL_EXT_unpack_subimage")) {
		fprintf(stderr, "GL_EXT_unpack_subimage not available\n");
		return -1;
	}

	extensions =
		(const char *) eglQueryString(ec->display, EGL_EXTENSIONS);
	if (strstr(extensions, "EGL_WL_bind_wayland_display"))
		ec->has_bind_display = 1;
	if (ec->has_bind_display)
		ec->bind_display(ec->display, ec->wl_display);

	wl_list_init(&ec->surface_list);
	wl_list_init(&ec->input_device_list);
	wl_list_init(&ec->output_list);
	wl_list_init(&ec->binding_list);
//	wl_list_init(&ec->animation_list);
//	weston_spring_init(&ec->fade.spring, 30.0, 1.0, 1.0);
//	ec->fade.animation.frame = fade_frame;
//	wl_list_init(&ec->fade.animation.link);

//	weston_layer_init(&ec->fade_layer, &ec->layer_list);
//	weston_layer_init(&ec->cursor_layer, &ec->fade_layer.link);

	screenshooter_create(ec);

	wl_data_device_manager_init(ec->wl_display);

	glActiveTexture(GL_TEXTURE0);

	if (weston_shader_init(&ec->texture_shader,
			     vertex_shader, texture_fragment_shader) < 0)
		return -1;
	if (weston_shader_init(&ec->solid_shader,
			     vertex_shader, solid_fragment_shader) < 0)
		return -1;

	loop = wl_display_get_event_loop(ec->wl_display);
	ec->idle_source = wl_event_loop_add_timer(loop, idle_handler, ec);
	wl_event_source_timer_update(ec->idle_source, ec->idle_time * 1000);

	ec->input_loop = wl_event_loop_create();

	weston_compositor_schedule_repaint(ec);

	return 0;
}

WL_EXPORT void
weston_compositor_shutdown(tComp *ec)
{
	tOutput *output, *next;

	wl_event_source_remove(ec->idle_source);
	if (ec->input_loop_source)
		wl_event_source_remove(ec->input_loop_source);

	/* Destroy all outputs associated with this compositor */
	wl_list_for_each_safe(output, next, &ec->output_list, link)
		output->destroy(output);

	weston_binding_list_destroy_all(&ec->binding_list);

	wl_array_release(&ec->vertices);
	wl_array_release(&ec->indices);

	wl_event_loop_destroy(ec->input_loop);
}

static int on_term_signal(int signal_number, void *data)
{
	struct wl_display *display = data;

	fprintf(stderr, "caught signal %d\n", signal_number);
	wl_display_terminate(display);

	return 1;
}

static void
on_segv_signal(int s, siginfo_t *siginfo, void *context)
{
	void *buffer[32];
	int i, count;
	Dl_info info;

	fprintf(stderr, "caught segv\n");

	count = backtrace(buffer, ARRAY_LENGTH(buffer));
	for (i = 0; i < count; i++) {
		dladdr(buffer[i], &info);
		fprintf(stderr, "  [%016lx]  %s  (%s)\n",
			(long) buffer[i],
			info.dli_sname ? info.dli_sname : "--",
			info.dli_fname);
	}
	longjmp(segv_jmp_buf, 1);
}


static void *
load_module(const char *name, const char *entrypoint, void **handle)
{
	char path[PATH_MAX];
	void *module, *init;

	if (name[0] != '/')
		snprintf(path, sizeof path, MODULEDIR "/%s", name);
	else
		snprintf(path, sizeof path, "%s", name);

	module = dlopen(path, RTLD_LAZY);
	if (!module) {
		fprintf(stderr,
			"failed to load module: %s\n", dlerror());
		return NULL;
	}

	init = dlsym(module, entrypoint);
	if (!init) {
		fprintf(stderr,
			"failed to lookup init function: %s\n", dlerror());
		return NULL;
	}

	return init;
}



int
shell_init(tComp *ec);

int main(int argc, char *argv[])
{
	struct wl_display *display;
	tComp *ec;
	struct wl_event_source *signals[4];
	struct wl_event_loop *loop;
	struct sigaction segv_action;
	void *shell_module, *backend_module, *xserver_module;
//	int (*shell_init)(tComp *ec);
	int (*xserver_init)(tComp *ec);
	tComp
		*(*backend_init)(struct wl_display *display,
				 int argc, char *argv[]);
	int i;
	char *backend = NULL;
	char *shell = NULL;
	int32_t idle_time = 300;
	int32_t xserver;
	char *socket_name = NULL;
	char *config_file;

	const struct config_key shell_config_keys[] = {
		{ "type", CONFIG_KEY_STRING, &shell },
	};

	const struct config_section cs[] = {
		{ "shell",
		  shell_config_keys, ARRAY_LENGTH(shell_config_keys) },
	};

	const struct weston_option core_options[] = {
		{ WESTON_OPTION_STRING, "backend", 'B', &backend },
		{ WESTON_OPTION_STRING, "socket", 'S', &socket_name },
		{ WESTON_OPTION_INTEGER, "idle-time", 'i', &idle_time },
		{ WESTON_OPTION_BOOLEAN, "xserver", 0, &xserver },
	};

	argc = parse_options(core_options,
			     ARRAY_LENGTH(core_options), argc, argv);

	display = wl_display_create();

	loop = wl_display_get_event_loop(display);
	signals[0] = wl_event_loop_add_signal(loop, SIGTERM, on_term_signal,
					      display);
	signals[1] = wl_event_loop_add_signal(loop, SIGINT, on_term_signal,
					      display);
	signals[2] = wl_event_loop_add_signal(loop, SIGQUIT, on_term_signal,
					      display);

	wl_list_init(&child_process_list);
	signals[3] = wl_event_loop_add_signal(loop, SIGCHLD, sigchld_handler,
					      NULL);

	segv_action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	segv_action.sa_sigaction = on_segv_signal;
	sigemptyset(&segv_action.sa_mask);
	sigaction(SIGSEGV, &segv_action, NULL);

	if (!backend) {
		if (getenv("WAYLAND_DISPLAY"))
			backend = "wayland-backend.so";
		else if (getenv("DISPLAY"))
			backend = "x11-backend.so";
		else if (getenv("OPENWFD"))
			backend = "openwfd-backend.so";
		else
			backend = "drm-backend.so";
	}

	config_file = config_file_path("weston.ini");
	parse_config_file(config_file, cs, ARRAY_LENGTH(cs), shell);
	free(config_file);

//	if (!shell)
//		shell = "desktop-shell.so";

	backend_init = load_module(backend, "backend_init", &backend_module);
	if (!backend_init)
		exit(EXIT_FAILURE);

//	shell_init = load_module(shell, "shell_init", &shell_module);
//	if (!shell_init)
//		exit(EXIT_FAILURE);

	ec = backend_init(display, argc, argv);
	if (ec == NULL) {
		fprintf(stderr, "failed to create compositor\n");
		exit(EXIT_FAILURE);
	}

	for (i = 1; argv[i]; i++)
		fprintf(stderr, "unhandled option: %s\n", argv[i]);
	if (argv[1])
		exit(EXIT_FAILURE);

	ec->option_idle_time = idle_time;
	ec->idle_time = idle_time;

	if (shell_init(ec) < 0)
		exit(EXIT_FAILURE);

	xserver_init = NULL;
//	if (xserver)
//		xserver_init = load_module("xserver-launcher.so",
//					   "weston_xserver_init",
//					   &xserver_module);
	xserver_init = weston_xserver_init;
	if (xserver_init)
		gpXServer = xserver_init(ec);

	if (wl_display_add_socket(display, socket_name)) {
		fprintf(stderr, "failed to add socket: %m\n");
		exit(EXIT_FAILURE);
	}

	weston_compositor_dpms_on(ec);
	weston_compositor_wake(ec);
	if (setjmp(segv_jmp_buf) == 0)
		wl_display_run(display);

	/* prevent further rendering while shutting down */
	ec->state = WESTON_COMPOSITOR_SLEEPING;

	wl_signal_emit(&ec->destroy_signal, ec);

	if (ec->has_bind_display)
		ec->unbind_display(ec->display, display);

	for (i = ARRAY_LENGTH(signals); i;)
		wl_event_source_remove(signals[--i]);

	ec->destroy(ec);
	wl_display_destroy(display);

	return 0;
}







/** *********************************** shell ************************************ **/

struct wl_shell gShell;

static void
destroy_shell_grab_shsurf(struct wl_listener *listener, void *data)
{
	struct shell_grab *grab;

	grab = container_of(listener, struct shell_grab,
			    shsurf_destroy_listener);

	grab->shsurf = NULL;
}

static void
shell_grab_init(struct shell_grab *grab,
		const struct wl_pointer_grab_interface *interface,
		tWin *shsurf)
{
	grab->grab.interface = interface;
	grab->shsurf = shsurf;
	grab->shsurf_destroy_listener.notify = destroy_shell_grab_shsurf;
	wl_signal_add(&shsurf->resource.destroy_signal,
		      &grab->shsurf_destroy_listener);

}

static void
shell_grab_finish(struct shell_grab *grab)
{
	wl_list_remove(&grab->shsurf_destroy_listener.link);
}

static void
center_on_output(tSurf *surface,
		 tOutput *output);

tWin*		get_shell_surface			(tSurf *surface);

static void
shell_configuration(struct wl_shell *shell)
{
	char *config_file;
	char *path = NULL;
	int duration = 60;

	struct config_key saver_keys[] = {
		{ "path",       CONFIG_KEY_STRING,  &path },
		{ "duration",   CONFIG_KEY_INTEGER, &duration },
	};

	struct config_section cs[] = {
		{ "screensaver", saver_keys, ARRAY_LENGTH(saver_keys), NULL },
	};

	config_file = config_file_path("weston.ini");
	parse_config_file(config_file, cs, ARRAY_LENGTH(cs), shell);
	free(config_file);

	gShell.screensaver.path = path;
	gShell.screensaver.duration = duration;
}


/** *********************************** shell grabs manipulation ************************************ **/

static void
noop_grab_focus(struct wl_pointer_grab *grab,
		struct wl_surface *surface, int32_t x, int32_t y)
{
	grab->focus = NULL;
}

static void
move_grab_motion(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t x, int32_t y)
{
	struct weston_move_grab *move = (struct weston_move_grab *) grab;
	struct wl_input_device *device = grab->input_device;
	tWin *shsurf = move->base.shsurf;
	tSurf *es;

	if (!shsurf)
		return;
	
	ShSurf_LSet (shsurf, L_eFloat);
	L_ShellToTop (shsurf);
	
	es = shsurf->surface;
	
	weston_surface_configure(es,
				 device->x + move->dx,
				 device->y + move->dy,
				 es->geometry.width, es->geometry.height);
	
}

static void
move_grab_button(struct wl_pointer_grab *grab,
		 uint32_t time, uint32_t button, int32_t state)
{
	struct shell_grab *shell_grab = container_of(grab, struct shell_grab,
						    grab);
	struct wl_input_device *device = grab->input_device;

	if (device->button_count == 0 && state == 0) {
		shell_grab_finish(shell_grab);
		wl_input_device_end_pointer_grab(device);
		free(grab);
	}
}

static const struct wl_pointer_grab_interface move_grab_interface = {
	noop_grab_focus,
	move_grab_motion,
	move_grab_button,
};

static int
weston_surface_move(tSurf *es,
		    tFocus *wd)
{
	struct weston_move_grab *move;
	tWin *shsurf = get_shell_surface(es);

	if (!shsurf)
		return -1;

	move = malloc(sizeof *move);
	if (!move)
		return -1;

	shell_grab_init(&move->base, &move_grab_interface, shsurf);

	move->dx = es->geometry.x - wd->input_device.grab_x;
	move->dy = es->geometry.y - wd->input_device.grab_y;

	wl_input_device_start_pointer_grab(&wd->input_device,
					   &move->base.grab);

	wl_input_device_set_pointer_focus(&wd->input_device, NULL, 0, 0);

	return 0;
}

static void
shell_surface_move(struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *input_resource, uint32_t serial)
{
	tFocus *wd = input_resource->data;
	tWin *shsurf = resource->data;

	if (wd->input_device.button_count == 0 ||
	    wd->input_device.grab_serial != serial ||
	    wd->input_device.pointer_focus != &shsurf->surface->surface)
		return;

	if (weston_surface_move(shsurf->surface, wd) < 0)
		wl_resource_post_no_memory(resource);
}

struct weston_resize_grab {
	struct shell_grab base;
	uint32_t edges;
	int32_t width, height;
};

static void
resize_grab_motion(struct wl_pointer_grab *grab,
		   uint32_t time, int32_t x, int32_t y)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;
	struct wl_input_device *device = grab->input_device;
	int32_t width, height;
	int32_t from_x, from_y;
	int32_t to_x, to_y;

	if (!resize->base.shsurf)
		return;

	ShSurf_LSet (resize->base.shsurf, L_eFloat);
	
	weston_surface_from_global(resize->base.shsurf->surface,
				   device->grab_x, device->grab_y,
				   &from_x, &from_y);
	weston_surface_from_global(resize->base.shsurf->surface,
				   device->x, device->y, &to_x, &to_y);

	if (resize->edges & WL_SHELL_SURFACE_RESIZE_LEFT) {
		width = resize->width + from_x - to_x;
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_RIGHT) {
		width = resize->width + to_x - from_x;
	} else {
		width = resize->width;
	}

	if (resize->edges & WL_SHELL_SURFACE_RESIZE_TOP) {
		height = resize->height + from_y - to_y;
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_BOTTOM) {
		height = resize->height + to_y - from_y;
	} else {
		height = resize->height;
	}
	if (resize->base.shsurf->surface->client == gpXServer->client) {
		weston_wm_window_resize (gpXServer->wm, resize->base.shsurf->surface,
			resize->base.shsurf->surface->geometry.x,
			resize->base.shsurf->surface->geometry.y,
			width, height, 0);
	}else {
		wl_shell_surface_send_configure(&resize->base.shsurf->resource,
						resize->edges, width, height);
	}
}

static void
resize_grab_button(struct wl_pointer_grab *grab,
		   uint32_t time, uint32_t button, int32_t state)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;
	struct wl_input_device *device = grab->input_device;

	if (device->button_count == 0 && state == 0) {
		shell_grab_finish(&resize->base);
		wl_input_device_end_pointer_grab(device);
		free(grab);
	}
}

static const struct wl_pointer_grab_interface resize_grab_interface = {
	noop_grab_focus,
	resize_grab_motion,
	resize_grab_button,
};

static int
weston_surface_resize(tWin *shsurf,
		      tFocus *wd, uint32_t edges)
{
	struct weston_resize_grab *resize;

	if (shsurf->type == SHELL_SURFACE_FULLSCREEN)
		return 0;

	if (edges == 0 || edges > 15 ||
	    (edges & 3) == 3 || (edges & 12) == 12)
		return 0;

	resize = malloc(sizeof *resize);
	if (!resize)
		return -1;

	shell_grab_init(&resize->base, &resize_grab_interface, shsurf);

	resize->edges = edges;
	resize->width = shsurf->surface->geometry.width;
	resize->height = shsurf->surface->geometry.height;

	wl_input_device_start_pointer_grab(&wd->input_device,
					   &resize->base.grab);

	wl_input_device_set_pointer_focus(&wd->input_device, NULL, 0, 0);

	return 0;
}

static void
shell_surface_resize(struct wl_client *client, struct wl_resource *resource,
		     struct wl_resource *input_resource, uint32_t serial,
		     uint32_t edges)
{
	tFocus *wd = input_resource->data;
	tWin *shsurf = resource->data;

	if (shsurf->type == SHELL_SURFACE_FULLSCREEN)
		return;

	if (wd->input_device.button_count == 0 ||
	    wd->input_device.grab_serial != serial ||
	    wd->input_device.pointer_focus != &shsurf->surface->surface)
		return;

	if (weston_surface_resize(shsurf, wd, edges) < 0)
		wl_resource_post_no_memory(resource);
}




static void
swap_grab_motion(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t x, int32_t y)
{
//	dTrace_E ("xy %d %d\n", x, y);
	struct weston_swap_grab *move = (struct weston_swap_grab *) grab;
	struct wl_input_device *device = grab->input_device;
	tWin *shsurf = move->base.shsurf;
	tSurf *es;
	
	if (!shsurf)
		return;
	
//	es = shsurf->surface;
	int32_t sx = device->x, sy = device->y;
	es = weston_compositor_pick_surface(gShell.pEC,
						 device->x, device->y,
						 &sx,
						 &sy);/**/
	
	if (!es) {
		
		return;
	}
	
	tOutput* newout = CurrentOutput ();
	if (newout != shsurf->surface->output) {
		shsurf->Tags = newout->Tags;
		shell_restack();
	}
	
	tWin *shsurf_to = get_shell_surface(es);
	if (shsurf_to->type != SHELL_SURFACE_TOPLEVEL) {
		return;
	}
	if (es != shsurf->surface) {
		struct wl_list *s0 = &shsurf->L_link, *s1 = &shsurf_to->L_link;
	//	printf ("YAAAAAAAAAAAAAAAAAAAAAAAAAAYYYYYY s0 %d %d		s1 %d %d\n", s0->surface->geometry.x, s0->surface->geometry.y, s1->surface->geometry.x, s1->surface->geometry.y);
		if (s1->next == s0) {
		//	printf ("MUHAHAHAHAH 0\n");
			tWin *t = s0;
			s0 = s1;
			s1 = t;
		}
		if (s0->next == s1) {
		//	printf ("MUHAHAHAHAH 1\n");
			s0->next = s1->next;
			s1->prev = s0->prev;
			
			s0->prev = s1;
			s1->next = s0;
			
			s0->next->prev = s0;
			s1->prev->next = s1;
			
			shell_restack ();
		}else {
		//	printf ("MUHAHAHAHAH 3\n");
			struct wl_list *lp0 = s0->prev, *lp1 = s1->prev;
		//	printf ("MUHAHAHAHAH %lx %lx\n", lp0, lp1);
			wl_list_remove (s0);
			wl_list_remove (s1);
			
			wl_list_insert (lp0, s1);
			wl_list_insert (lp1, s0);/**/
			
			shell_restack ();
		//	printf ("MUHAHAHAHAH 1\n");
		}
		
	/*	if (shsurf->Tags != shsurf_to->Tags) {
			tTags t = shsurf->Tags;
			shsurf->Tags = shsurf_to->Tags;
			shsurf_to->Tags = t;
		}/**/
	}else {
	//	printf ("YAAAAAAAAAAAAAAAAAAAAAAAAAAYYYYYY NO\n");
		
	}
//	weston_surface_configure(es,
//				 device->x + move->dx,
//				 device->y + move->dy,
//				 es->geometry.width, es->geometry.height);
}

static void
swap_grab_button(struct wl_pointer_grab *grab,
		 uint32_t time, uint32_t button, int32_t state)
{
	struct shell_grab *shell_grab = container_of(grab, struct shell_grab,
						    grab);
	struct wl_input_device *device = grab->input_device;

	if (device->button_count == 0 && state == 0) {
		shell_grab_finish(shell_grab);
		wl_input_device_end_pointer_grab(device);
		free(grab);
	}
}

static const struct wl_pointer_grab_interface swap_grab_interface = {
	noop_grab_focus,
	swap_grab_motion,
	swap_grab_button,
};

static int
weston_surface_swap(tSurf *es,
		    tFocus *wd)
{
	struct weston_swap_grab *move;
	tWin *shsurf = get_shell_surface(es);
	
	if (!shsurf)
		return -1;
	
	move = malloc(sizeof *move);
	if (!move)
		return -1;
	
	shell_grab_init(&move->base, &swap_grab_interface, shsurf);
	
	move->dx = es->geometry.x - wd->input_device.grab_x;
	move->dy = es->geometry.y - wd->input_device.grab_y;
	
	wl_input_device_start_pointer_grab(&wd->input_device,
					   &move->base.grab);
	
	wl_input_device_set_pointer_focus(&wd->input_device, NULL, 0, 0);
	
	return 0;
}





/** *********************************** shell other ************************************ **/

static tOutput *
get_default_output(tComp *compositor)
{
	return container_of(compositor->output_list.next,
			    tOutput, link);
}

void
shell_unset_fullscreen(tWin *shsurf)
{
	/* undo all fullscreen things here */
	shsurf->fullscreen.type = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;
	shsurf->fullscreen.framerate = 0;
	wl_list_remove(&shsurf->fullscreen.transform.link);
	wl_list_init(&shsurf->fullscreen.transform.link);
	weston_surface_destroy(shsurf->fullscreen.black_surface);
	shsurf->fullscreen.black_surface = NULL;
	shsurf->fullscreen_output = NULL;
	shsurf->force_configure = 1;
	weston_surface_set_position(shsurf->surface,
				    shsurf->saved_x, shsurf->saved_y);
}

int
reset_shell_surface_type(tWin *surface)
{
	switch (surface->type) {
	case SHELL_SURFACE_FULLSCREEN:
		shell_unset_fullscreen(surface);
		break;
	case SHELL_SURFACE_MAXIMIZED:
	//	surface->output = get_default_output(surface->gShell.pEC);
		surface->output = CurrentOutput ();
		weston_surface_set_position(surface->surface,
					    surface->saved_x,
					    surface->saved_y);
		break;
	case SHELL_SURFACE_PANEL:
	case SHELL_SURFACE_BACKGROUND:
		wl_list_remove(&surface->link);
		wl_list_init(&surface->link);
		break;
	case SHELL_SURFACE_SCREENSAVER:
	case SHELL_SURFACE_LOCK:
		wl_resource_post_error(&surface->resource,
				       WL_DISPLAY_ERROR_INVALID_METHOD,
				       "cannot reassign surface type");
		return -1;
	case SHELL_SURFACE_NONE:
	case SHELL_SURFACE_TOPLEVEL:
	case SHELL_SURFACE_TRANSIENT:
	case SHELL_SURFACE_POPUP:
		break;
	}

	surface->type = SHELL_SURFACE_NONE;
	return 0;
}

void
shell_surface_set_toplevel(struct wl_client *client,
			   struct wl_resource *resource)

{
	tWin *surface = resource->data;

	if (reset_shell_surface_type(surface))
		return;

	surface->type = SHELL_SURFACE_TOPLEVEL;
}

void
shell_surface_set_transient(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *parent_resource,
			    int x, int y, uint32_t flags)
{
	tWin *shsurf = resource->data;
	tSurf *es = shsurf->surface;
	tWin *pshsurf = parent_resource->data;
	tSurf *pes = pshsurf->surface;

	if (reset_shell_surface_type(shsurf))
		return;

	/* assign to parents output */
	shsurf->output = pes->output;
 	weston_surface_set_position(es, pes->geometry.x + x,
					pes->geometry.y + y);

	shsurf->type = SHELL_SURFACE_TRANSIENT;
}

struct wl_shell *
shell_surface_get_shell(tWin *shsurf)
{
	return shsurf->shell;
}

static int
get_output_panel_height(struct wl_shell *wlshell,tOutput *output)
{
	tWin *priv;
	int panel_height = 0;

	if (!output)
		return 0;

	wl_list_for_each(priv, &gShell.panels, link) {
		if (priv->output == output) {
			panel_height = priv->surface->geometry.height;
			break;
		}
	}
	return panel_height;
}

static void
shell_surface_set_maximized(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *output_resource )
{
	tWin *shsurf = resource->data;
	tSurf *es = shsurf->surface;
	struct wl_shell *wlshell = NULL;
	uint32_t edges = 0, panel_height = 0;

	/* get the default output, if the client set it as NULL
	   check whether the ouput is available */
	if (output_resource)
		shsurf->output = output_resource->data;
	else
		shsurf->output = get_default_output(gShell.pEC);

	if (reset_shell_surface_type(shsurf))
		return;

	shsurf->saved_x = es->geometry.x;
	shsurf->saved_y = es->geometry.y;
	shsurf->saved_position_valid = true;

	wlshell = shell_surface_get_shell(shsurf);
	panel_height = get_output_panel_height(wlshell, es->output);
	edges = WL_SHELL_SURFACE_RESIZE_TOP|WL_SHELL_SURFACE_RESIZE_LEFT;

	wl_shell_surface_send_configure(&shsurf->resource, edges,
					es->output->current->width,
					es->output->current->height - panel_height);

	shsurf->type = SHELL_SURFACE_MAXIMIZED;
}

static void
black_surface_configure(tSurf *es, int32_t sx, int32_t sy);

static tSurf *
create_black_surface(tComp *ec,
		     tSurf *fs_surface,
		     GLfloat x, GLfloat y, int w, int h)
{
	tSurf *surface = NULL;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		fprintf(stderr, "no memory\n");
		return NULL;
	}

	surface->configure = black_surface_configure;
	surface->private = fs_surface;
	weston_surface_configure(surface, x, y, w, h);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1);
	return surface;
}

/* Create black surface and append it to the associated fullscreen surface.
 * Handle size dismatch and positioning according to the method. */
static void
shell_configure_fullscreen(tWin *shsurf)
{
	tOutput *output = shsurf->fullscreen_output;
	tSurf *surface = shsurf->surface;
	struct weston_matrix *matrix;
	float scale;

	center_on_output(surface, output);

	if (!shsurf->fullscreen.black_surface)
		shsurf->fullscreen.black_surface =
			create_black_surface(gShell.pEC,
					     surface,
					     output->x, output->y,
					     output->current->width,
					     output->current->height);

//	wl_list_remove(&shsurf->fullscreen.black_surface->layer_link);
//	wl_list_insert(&surface->layer_link, &shsurf->fullscreen.black_surface->layer_link);
	shsurf->fullscreen.black_surface->output = output;

	switch (shsurf->fullscreen.type) {
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT:
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE:
		matrix = &shsurf->fullscreen.transform.matrix;
		weston_matrix_init(matrix);
		scale = (float)output->current->width/(float)surface->geometry.width;
		weston_matrix_scale(matrix, scale, scale, 1);
		wl_list_remove(&shsurf->fullscreen.transform.link);
	//	wl_list_insert(surface->geometry.transformation_list.prev,
	//		       &shsurf->fullscreen.transform.link);
		weston_surface_set_position(surface, output->x, output->y);
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER:
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_FILL:
		break;
	default:
		break;
	}
}

/* make the fullscreen and black surface at the top */
static void
shell_stack_fullscreen(tWin *shsurf)
{
	tSurf *surface = shsurf->surface;
	struct wl_shell *shell = shell_surface_get_shell(shsurf);

//	wl_list_remove(&surface->layer_link);
//	wl_list_remove(&shsurf->fullscreen.black_surface->layer_link);

//	wl_list_insert(&gShell.fullscreen_layer.surface_list, &surface->layer_link);
//	wl_list_insert(&surface->layer_link, &shsurf->fullscreen.black_surface->layer_link);

	weston_surface_damage(surface);
	weston_surface_damage(shsurf->fullscreen.black_surface);
}

static void
shell_map_fullscreen(tWin *shsurf)
{
	shell_configure_fullscreen(shsurf);
	shell_stack_fullscreen(shsurf);
}

static void
shell_surface_set_fullscreen(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t method,
			     uint32_t framerate,
			     struct wl_resource *output_resource)
{
	tWin *shsurf = resource->data;
	tSurf *es = shsurf->surface;

	if (output_resource)
		shsurf->output = output_resource->data;
	else
		shsurf->output = get_default_output(gShell.pEC);

	if (reset_shell_surface_type(shsurf))
		return;

	shsurf->fullscreen_output = shsurf->output;
	shsurf->fullscreen.type = method;
	shsurf->fullscreen.framerate = framerate;
	shsurf->type = SHELL_SURFACE_FULLSCREEN;

	shsurf->saved_x = es->geometry.x;
	shsurf->saved_y = es->geometry.y;
	shsurf->saved_position_valid = true;

	if (weston_surface_is_mapped(es))
		shsurf->force_configure = 1;

	wl_shell_surface_send_configure(&shsurf->resource, 0,
					shsurf->output->current->width,
					shsurf->output->current->height);
}

static void
popup_grab_focus(struct wl_pointer_grab *grab,
		 struct wl_surface *surface, int32_t x, int32_t y)
{
	struct wl_input_device *device = grab->input_device;
	tWin *priv =
		container_of(grab, tWin, popup.grab);
	struct wl_client *client = priv->surface->surface.resource.client;

	if (surface && surface->resource.client == client) {
		wl_input_device_set_pointer_focus(device, surface, x, y);
		grab->focus = surface;
	} else {
		wl_input_device_set_pointer_focus(device, NULL, 0, 0);
		grab->focus = NULL;
	}
}

static void
popup_grab_motion(struct wl_pointer_grab *grab,
		  uint32_t time, int32_t sx, int32_t sy)
{
	struct wl_resource *resource;

	resource = grab->input_device->pointer_focus_resource;
	if (resource)
		wl_input_device_send_motion(resource, time, sx, sy);
}

static void
popup_grab_button(struct wl_pointer_grab *grab,
		  uint32_t time, uint32_t button, int32_t state)
{
	struct wl_resource *resource;
	tWin *shsurf =
		container_of(grab, tWin, popup.grab);
	struct wl_display *display;
	uint32_t serial;

	resource = grab->input_device->pointer_focus_resource;
	if (resource) {
		display = wl_client_get_display(resource->client);
		serial = wl_display_get_serial(display);
		wl_input_device_send_button(resource, serial,
					    time, button, state);
	} else if (state == 0 &&
		   (shsurf->popup.initial_up ||
		    time - shsurf->popup.time > 500)) {
		wl_shell_surface_send_popup_done(&shsurf->resource);
		wl_input_device_end_pointer_grab(grab->input_device);
		shsurf->popup.grab.input_device = NULL;
	}

	if (state == 0)
		shsurf->popup.initial_up = 1;
}

static const struct wl_pointer_grab_interface popup_grab_interface = {
	popup_grab_focus,
	popup_grab_motion,
	popup_grab_button,
};

static void
shell_map_popup(tWin *shsurf, uint32_t serial)
{
	struct wl_input_device *device;
	tSurf *es = shsurf->surface;
	tSurf *parent = shsurf->parent->surface;

	es->output = parent->output;

	shsurf->popup.grab.interface = &popup_grab_interface;
	device = gShell.pEC->input_device;

	weston_surface_update_transform(parent);
//	if (parent->transform.enabled) {
//		shsurf->popup.parent_transform.matrix =
//			parent->transform.matrix;
//	} else {
		/* construct x, y translation matrix */
		weston_matrix_init(&shsurf->popup.parent_transform.matrix);
		shsurf->popup.parent_transform.matrix.d[12] =
			parent->geometry.x;
		shsurf->popup.parent_transform.matrix.d[13] =
			parent->geometry.y;
//	}
//	wl_list_insert(es->geometry.transformation_list.prev,
//		       &shsurf->popup.parent_transform.link);
	weston_surface_set_position(es, shsurf->popup.x, shsurf->popup.y);

	shsurf->popup.grab.input_device = device;
	shsurf->popup.time = device->grab_time;
	shsurf->popup.initial_up = 0;

	wl_input_device_start_pointer_grab(shsurf->popup.grab.input_device,
					   &shsurf->popup.grab);
}

static void
shell_surface_set_popup(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *input_device_resource,
			uint32_t time,
			struct wl_resource *parent_resource,
			int32_t x, int32_t y, uint32_t flags)
{
	tWin *shsurf = resource->data;

	shsurf->type = SHELL_SURFACE_POPUP;
	shsurf->parent = parent_resource->data;
	shsurf->popup.x = x;
	shsurf->popup.y = y;
}

static const struct wl_shell_surface_interface shell_surface_implementation = {
	shell_surface_move,
	shell_surface_resize,
	shell_surface_set_toplevel,
	shell_surface_set_transient,
	shell_surface_set_fullscreen,
	shell_surface_set_popup,
	shell_surface_set_maximized
};

void					destroy_shell_surface			(struct wl_resource *resource)
{
	tWin *shsurf = resource->data;

	if (shsurf->popup.grab.input_device)
		wl_input_device_end_pointer_grab(shsurf->popup.grab.input_device);

	/* in case cleaning up a dead client destroys shell_surface first */
	if (shsurf->surface) {
		wl_list_remove(&shsurf->surface_destroy_listener.link);
		shsurf->surface->configure = NULL;
	}

	if (shsurf->fullscreen.black_surface)
		weston_surface_destroy(shsurf->fullscreen.black_surface);

	wl_list_remove(&shsurf->link);
        wl_list_remove(&shsurf->L_link);
        shell_restack();
	free(shsurf);
}

void					shell_handle_surface_destroy		(struct wl_listener *listener, void *data)
{
	tWin *shsurf = container_of(listener,
						    tWin,
						    surface_destroy_listener);

	shsurf->surface = NULL;
	wl_resource_destroy(&shsurf->resource);
}

tWin *		get_shell_surface				(tSurf *surface)
{
	struct wl_listener *listener;
	
	listener = wl_signal_get(&surface->surface.resource.destroy_signal, shell_handle_surface_destroy);
	if (listener)
		return container_of(listener, tWin, surface_destroy_listener);

	return NULL;
}



static const struct wl_shell_interface shell_implementation = {
	shell_get_shell_surface
};

static void
handle_screensaver_sigchld(struct weston_process *proc, int status)
{
	proc->pid = 0;
}

static void
launch_screensaver(struct wl_shell *shell)
{
	if (gShell.screensaver.binding)
		return;

	if (!gShell.screensaver.path)
		return;

	if (gShell.screensaver.process.pid != 0) {
		fprintf(stderr, "old screensaver still running\n");
		return;
	}

	weston_client_launch(gShell.pEC,
			   &gShell.screensaver.process,
			   gShell.screensaver.path,
			   handle_screensaver_sigchld);
}

static void
terminate_screensaver(struct wl_shell *shell)
{
	if (gShell.screensaver.process.pid == 0)
		return;

	kill(gShell.screensaver.process.pid, SIGTERM);
}

static void
show_screensaver(struct wl_shell *shell, tWin *surface)
{
	struct wl_list *list;

//	if (gShell.lock_surface)
//		list = &gShell.lock_surface->surface->layer_link;
//	else
//		list = &gShell.lock_layer.surface_list;

//	wl_list_remove(&surface->surface->layer_link);
//	wl_list_insert(list, &surface->surface->layer_link);
//	surface->surface->output = surface->output;
//	weston_surface_damage(surface->surface);
}

static void
hide_screensaver(struct wl_shell *shell, tWin *surface)
{
//	wl_list_remove(&surface->surface->layer_link);
//	wl_list_init(&surface->surface->layer_link);
//	surface->surface->output = NULL;
}

static void
desktop_shell_set_background(struct wl_client *client,
			     struct wl_resource *resource,
			     struct wl_resource *output_resource,
			     struct wl_resource *surface_resource)
{
	struct wl_shell *shell = resource->data;
	tWin *shsurf = surface_resource->data;
	tSurf *surface = shsurf->surface;
	tWin *priv;

	if (reset_shell_surface_type(shsurf))
		return;

	wl_list_for_each(priv, &gShell.backgrounds, link) {
		if (priv->output == output_resource->data) {
			priv->surface->output = NULL;
		//	wl_list_remove(&priv->surface->layer_link);
			wl_list_remove(&priv->link);
			break;
		}
	}

	shsurf->type = SHELL_SURFACE_BACKGROUND;
	shsurf->output = output_resource->data;

	wl_list_insert(&gShell.backgrounds, &shsurf->link);

	weston_surface_set_position(surface, shsurf->output->x,
				    shsurf->output->y);

	desktop_shell_send_configure(resource, 0,
				     surface_resource,
				     shsurf->output->current->width,
				     shsurf->output->current->height);
}

static void
desktop_shell_set_panel(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *output_resource,
			struct wl_resource *surface_resource)
{
	struct wl_shell *shell = resource->data;
	tWin *shsurf = surface_resource->data;
	tSurf *surface = shsurf->surface;
	tWin *priv;

	if (reset_shell_surface_type(shsurf))
		return;

	wl_list_for_each(priv, &gShell.panels, link) {
		if (priv->output == output_resource->data) {
			priv->surface->output = NULL;
		//	wl_list_remove(&priv->surface->layer_link);
			wl_list_remove(&priv->link);
			break;
		}
	}

	shsurf->type = SHELL_SURFACE_PANEL;
	shsurf->output = output_resource->data;

	wl_list_insert(&gShell.panels, &shsurf->link);

	weston_surface_set_position(surface, shsurf->output->x,
				    shsurf->output->y);

	desktop_shell_send_configure(resource, 0,
				     surface_resource,
				     shsurf->output->current->width,
				     shsurf->output->current->height);
}

static void
handle_lock_surface_destroy(struct wl_listener *listener, void *data)
{
	struct wl_shell *shell =
		container_of(listener, struct wl_shell, lock_surface_listener);

	fprintf(stderr, "lock surface gone\n");
	gShell.lock_surface = NULL;
}

static void
desktop_shell_set_lock_surface(struct wl_client *client,
			       struct wl_resource *resource,
			       struct wl_resource *surface_resource)
{
	struct wl_shell *shell = resource->data;
	tWin *surface = surface_resource->data;

	if (reset_shell_surface_type(surface))
		return;

	gShell.prepare_event_sent = false;

	if (!gShell.locked)
		return;

	gShell.lock_surface = surface;

	gShell.lock_surface_listener.notify = handle_lock_surface_destroy;
	wl_signal_add(&surface_resource->destroy_signal,
		      &gShell.lock_surface_listener);

	gShell.lock_surface->type = SHELL_SURFACE_LOCK;
}

static void
resume_desktop(struct wl_shell *shell)
{
	tWin *tmp;

	wl_list_for_each(tmp, &gShell.screensaver.surfaces, link)
		hide_screensaver(shell, tmp);

	terminate_screensaver(shell);

//	wl_list_remove(&gShell.lock_layer.link);
//	wl_list_insert(&gShell.pEC->cursor_layer.link, &gShell.fullscreen_layer.link);
//	wl_list_insert(&gShell.fullscreen_layer.link, &gShell.panel_layer.link);
//	wl_list_insert(&gShell.panel_layer.link, &gShell.toplevel_layer.link);

	gShell.locked = false;
	gShell.pEC->idle_time = gShell.pEC->option_idle_time;
	weston_compositor_wake(gShell.pEC);
	weston_compositor_damage_all(gShell.pEC);
}

static void
desktop_shell_unlock(struct wl_client *client,
		     struct wl_resource *resource)
{
	struct wl_shell *shell = resource->data;

	gShell.prepare_event_sent = false;

	if (gShell.locked)
		resume_desktop(shell);
}

static void
desktop_shell_select_tag(struct wl_client *client,
			   struct wl_resource *resource,
			   uint32_t panel_no, uint32_t tag_no, uint32_t button, uint32_t modifier)
{
	printf("Tag No: %d %x %d\n", panel_no, tag_no, button);
	
	tOutput *out = CurrentOutput();
	switch (button) {
	case BTN_LEFT:
		Output_TagView (out, tag_no);
		break;
	case BTN_RIGHT:
		Output_TagView (out, out->Tags | tag_no);
		break;
	}
	shell_restack();
}

static const struct desktop_shell_interface desktop_shell_implementation = {
	desktop_shell_set_background,
	desktop_shell_set_panel,
	desktop_shell_set_lock_surface,
	desktop_shell_unlock,
	desktop_shell_select_tag
};

static enum shell_surface_type
get_shell_surface_type(tSurf *surface)
{
	tWin *shsurf;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return SHELL_SURFACE_NONE;
	return shsurf->type;
}


/** ***********************************  shell bindings ************************************ **/
static void
move_binding(struct wl_input_device *device, uint32_t time,
	     uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tSurf *surface =
		(tSurf *) device->pointer_focus;

	if (surface == NULL)
		return;

	switch (get_shell_surface_type(surface)) {
		case SHELL_SURFACE_PANEL:
		case SHELL_SURFACE_BACKGROUND:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_SCREENSAVER:
			return;
		default:
			break;
	}
	if (get_shell_surface(surface)->L == L_eFloat)
		weston_surface_move(surface, (tFocus *) device);
	else
		weston_surface_swap(surface, (tFocus *) device);
}

static void
resize_binding(struct wl_input_device *device, uint32_t time,
	       uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tSurf *surface =
		(tSurf *) device->pointer_focus;
	uint32_t edges = 0;
	int32_t x, y;
	tWin *shsurf;

	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return;
	
	switch (shsurf->type) {
		case SHELL_SURFACE_PANEL:
		case SHELL_SURFACE_BACKGROUND:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_SCREENSAVER:
			return;
		default:
			break;
	}
	
//	ShSurf_LSet (shsurf, L_eFloat);
	
	weston_surface_from_global(surface,
				   device->grab_x, device->grab_y, &x, &y);

	if (x < surface->geometry.width / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_LEFT;
	else if (x < 2 * surface->geometry.width / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_RIGHT;

	if (y < surface->geometry.height / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_TOP;
	else if (y < 2 * surface->geometry.height / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_BOTTOM;

	weston_surface_resize(shsurf, (tFocus *) device,
			      edges);
}

static void
surface_opacity_binding(struct wl_input_device *device, uint32_t time,
	       uint32_t key, uint32_t button, uint32_t axis, int32_t value, void *data)
{
	uint32_t step = 15;
	tWin *shsurf;
	tSurf *surface =
		(tSurf *) device->pointer_focus;

	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return;

	switch (shsurf->type) {
		case SHELL_SURFACE_BACKGROUND:
		case SHELL_SURFACE_SCREENSAVER:
			return;
		default:
			break;
	}

	surface->alpha += value * step;

	if (surface->alpha > 255)
		surface->alpha = 255;
	if (surface->alpha < step)
		surface->alpha = step;

	surface->geometry.dirty = 1;
	weston_surface_damage(surface);
}

static void
zoom_binding(struct wl_input_device *device, uint32_t time,
	       uint32_t key, uint32_t button, uint32_t axis, int32_t value, void *data)
{
	tFocus *wd = (tFocus *) device;
	tComp *compositor = wd->compositor;
	tOutput *output;

	wl_list_for_each(output, &compositor->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						device->x, device->y, NULL)) {
			output->zoom.active = 1;
			output->zoom.level += output->zoom.increment * -value;

			if (output->zoom.level >= 1.0) {
				output->zoom.active = 0;
				output->zoom.level = 1.0;
			}

			if (output->zoom.level < output->zoom.increment)
				output->zoom.level = output->zoom.increment;

			weston_output_update_zoom(output, device->x, device->y);
		}
	}
}

static void
terminate_binding(struct wl_input_device *device, uint32_t time,
		  uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tComp *compositor = data;

	if (state)
		wl_display_terminate(compositor->wl_display);
}

#if 0
static void
rotate_grab_motion(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t x, int32_t y)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);
	struct wl_input_device *device = grab->input_device;
	tWin *shsurf = rotate->base.shsurf;
	tSurf *surface;
	GLfloat cx, cy, dx, dy, cposx, cposy, dposx, dposy, r;

	if (!shsurf)
		return;

	surface = shsurf->surface;

	cx = 0.5f * surface->geometry.width;
	cy = 0.5f * surface->geometry.height;

	dx = device->x - rotate->center.x;
	dy = device->y - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);

//	wl_list_remove(&shsurf->rotation.transform.link);
	shsurf->surface->geometry.dirty = 1;

	if (r > 20.0f) {
		struct weston_matrix *matrix =
			&shsurf->rotation.transform.matrix;

		weston_matrix_init(&rotate->rotation);
		rotate->rotation.d[0] = dx / r;
		rotate->rotation.d[4] = -dy / r;
		rotate->rotation.d[1] = -rotate->rotation.d[4];
		rotate->rotation.d[5] = rotate->rotation.d[0];

		weston_matrix_init(matrix);
		weston_matrix_translate(matrix, -cx, -cy, 0.0f);
		weston_matrix_multiply(matrix, &shsurf->rotation.rotation);
		weston_matrix_multiply(matrix, &rotate->rotation);
		weston_matrix_translate(matrix, cx, cy, 0.0f);

	//	wl_list_insert(
	//		&shsurf->surface->geometry.transformation_list,
	//		&shsurf->rotation.transform.link);
	} else {
		wl_list_init(&shsurf->rotation.transform.link);
		weston_matrix_init(&shsurf->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	/* We need to adjust the position of the surface
	 * in case it was resized in a rotated state before */
	cposx = surface->geometry.x + cx;
	cposy = surface->geometry.y + cy;
	dposx = rotate->center.x - cposx;
	dposy = rotate->center.y - cposy;
	if (dposx != 0.0f || dposy != 0.0f) {
		weston_surface_set_position(surface,
					    surface->geometry.x + dposx,
					    surface->geometry.y + dposy);
	}

	/* Repaint implies weston_surface_update_transform(), which
	 * lazily applies the damage due to rotation update.
	 */
	weston_compositor_schedule_repaint(shsurf->gShell.pEC);
}

static void
rotate_grab_button(struct wl_pointer_grab *grab,
		 uint32_t time, uint32_t button, int32_t state)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);
	struct wl_input_device *device = grab->input_device;
	tWin *shsurf = rotate->base.shsurf;

	if (device->button_count == 0 && state == 0) {
		if (shsurf)
			weston_matrix_multiply(&shsurf->rotation.rotation,
					       &rotate->rotation);
		shell_grab_finish(&rotate->base);
		wl_input_device_end_pointer_grab(device);
		free(rotate);
	}
}

static const struct wl_pointer_grab_interface rotate_grab_interface = {
	noop_grab_focus,
	rotate_grab_motion,
	rotate_grab_button,
};

static void
rotate_binding(struct wl_input_device *device, uint32_t time,
	       uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tSurf *base_surface =
		(tSurf *) device->pointer_focus;
	tWin *surface;
	struct rotate_grab *rotate;
	GLfloat dx, dy;
	GLfloat r;

	if (base_surface == NULL)
		return;

	surface = get_shell_surface(base_surface);
	if (!surface)
		return;

	switch (surface->type) {
		case SHELL_SURFACE_PANEL:
		case SHELL_SURFACE_BACKGROUND:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_SCREENSAVER:
			return;
		default:
			break;
	}

	rotate = malloc(sizeof *rotate);
	if (!rotate)
		return;

	shell_grab_init(&rotate->base, &rotate_grab_interface, surface);

	weston_surface_to_global(surface->surface,
				 surface->surface->geometry.width / 2,
				 surface->surface->geometry.height / 2,
				 &rotate->center.x, &rotate->center.y);

	wl_input_device_start_pointer_grab(device, &rotate->base.grab);

	dx = device->x - rotate->center.x;
	dy = device->y - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);
	if (r > 20.0f) {
		struct weston_matrix inverse;

		weston_matrix_init(&inverse);
		inverse.d[0] = dx / r;
		inverse.d[4] = dy / r;
		inverse.d[1] = -inverse.d[4];
		inverse.d[5] = inverse.d[0];
		weston_matrix_multiply(&surface->rotation.rotation, &inverse);

		weston_matrix_init(&rotate->rotation);
		rotate->rotation.d[0] = dx / r;
		rotate->rotation.d[4] = -dy / r;
		rotate->rotation.d[1] = -rotate->rotation.d[4];
		rotate->rotation.d[5] = rotate->rotation.d[0];
	} else {
		weston_matrix_init(&surface->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	wl_input_device_set_pointer_focus(device, NULL, 0, 0);
}
#endif



/** ***********************************  shell surface ************************************ **/

void		L_ShellToTop		(tWin *shsurf)
{
	if (shsurf->L != L_eNorm)
		return;
	
	wl_list_remove(&shsurf->L_link);
	wl_list_insert(gShell.L[L_eFloat].prev, &shsurf->L_link);
	
	wl_list_remove(&shsurf->surface->link);
	wl_list_insert(&gShell.pEC->surface_list, &shsurf->surface->link);
	
//	weston_compositor_damage_all(gShell.pEC);
}
void		activate			(struct wl_shell *shell, tSurf *es, tFocus *device)
{
	tSurf *surf, *prev;

	weston_surface_activate(es, device);

	switch (get_shell_surface_type(es)) {
	case SHELL_SURFACE_BACKGROUND:
	case SHELL_SURFACE_PANEL:
	case SHELL_SURFACE_LOCK:
		break;

	case SHELL_SURFACE_SCREENSAVER:
		/* always below lock surface */
	//	if (gShell.lock_surface)
	//		weston_surface_restack(es, &gShell.lock_surface->surface->layer_link);
		break;
	case SHELL_SURFACE_FULLSCREEN:
		/* should on top of panels */
		shell_stack_fullscreen(get_shell_surface(es));
		break;
	default:
		/* move the fullscreen surfaces down into the toplevel layer */
	/*	if (!wl_list_empty(&gShell.fullscreen_layer.surface_list)) {
			wl_list_for_each_reverse_safe(surf,
								prev, 
								&gShell.fullscreen_layer.surface_list, 
								layer_link
			) {
				weston_surface_restack(surf, &gShell.toplevel_layer.surface_list); 
			}
		}*/
		
	//	weston_surface_restack(es, &gShell.toplevel_layer.surface_list);
	//	tWin* shsurf = get_shell_surface(es);
		
		break;
	}
}

void		shell_L_print		(struct wl_shell *shell)
{
	puts("Printing all shell surfaces");
	tWin *shsurf;
	int i;
	for (i=0; i<L_NUM; i++) {
		wl_list_for_each(shsurf, &gShell.L[i], L_link) {
			printf("%d: %d %d %d %d %x\n", 
				i,
				shsurf->surface->geometry.x,
				shsurf->surface->geometry.y,
				shsurf->surface->geometry.width,
				shsurf->surface->geometry.height,
				shsurf->Tags
			);
		}
	}
}


void		map			(struct wl_shell *shell, tSurf *surface, int32_t width, int32_t height, int32_t sx, int32_t sy)
{
	dTrace_E("");
	tComp *compositor = gShell.pEC;
	tWin *shsurf;
	enum shell_surface_type surface_type = SHELL_SURFACE_NONE;
	tSurf *parent;
	int panel_height = 0;
	
	shsurf = get_shell_surface(surface);
	if (shsurf) {
		surface_type = shsurf->type;
	}
	surface->geometry.width = width;
	surface->geometry.height = height;
	surface->geometry.dirty = 1;
	
	/* initial positioning, see also configure() */
	switch (surface_type) {
	case SHELL_SURFACE_TOPLEVEL:
		if (shsurf) {
			shsurf->Tags = CurrentOutput()->Tags;
		}
	//	weston_surface_set_position(surface, 10 + random() % 400,
	//				    10 + random() % 400);
		break;
	case SHELL_SURFACE_SCREENSAVER:
		center_on_output(surface, shsurf->fullscreen_output);
		break;
	case SHELL_SURFACE_FULLSCREEN:
		shell_map_fullscreen(shsurf);
		break;
	case SHELL_SURFACE_MAXIMIZED:
		/* use surface configure to set the geometry */
		panel_height = get_output_panel_height(shell,surface->output);
		weston_surface_set_position(surface, surface->output->x,
					    surface->output->y + panel_height);
		break;
	case SHELL_SURFACE_LOCK:
		center_on_output(surface, get_default_output(compositor));
		break;
	case SHELL_SURFACE_POPUP:
		shell_map_popup(shsurf, shsurf->popup.time);
	case SHELL_SURFACE_NONE:
		weston_surface_set_position(surface,
					    surface->geometry.x + sx,
					    surface->geometry.y + sy);
		break;
	default:
		;
	}

	/* surface stacking order, see also activate() */
	switch (surface_type) {
	case SHELL_SURFACE_BACKGROUND:
		/* background always visible, at the bottom */
	//	wl_list_insert(&gShell.background_layer.surface_list, &surface->layer_link);
		break;
	case SHELL_SURFACE_PANEL:
		/* panel always on top, hidden while locked */
	//	wl_list_insert(&gShell.panel_layer.surface_list, &surface->layer_link);
		break;
	case SHELL_SURFACE_LOCK:
		/* lock surface always visible, on top */
	//	wl_list_insert(&gShell.lock_layer.surface_list, &surface->layer_link);
		weston_compositor_wake(compositor);
		break;
	case SHELL_SURFACE_SCREENSAVER:
		/* If locked, show it. */
		if (gShell.locked) {
			show_screensaver(shell, shsurf);
			compositor->idle_time = gShell.screensaver.duration;
			weston_compositor_wake(compositor);
			if (!gShell.lock_surface)
				compositor->state = WESTON_COMPOSITOR_IDLE;
		}
		break;
	case SHELL_SURFACE_POPUP:
	case SHELL_SURFACE_TRANSIENT:
	//	parent = shsurf->parent->surface;
	//	wl_list_insert(parent->layer_link.prev, &surface->layer_link);
		break;
	case SHELL_SURFACE_FULLSCREEN:
	case SHELL_SURFACE_NONE:
		break;
	default:
	//	wl_list_insert(&gShell.toplevel_layer.surface_list, &surface->layer_link); 
		break;
	}

	if (surface_type != SHELL_SURFACE_NONE) {
		weston_surface_assign_output(surface);
		if (surface_type == SHELL_SURFACE_MAXIMIZED)
			surface->output = shsurf->output;
	}

	switch (surface_type) {
	case SHELL_SURFACE_TOPLEVEL:
	case SHELL_SURFACE_TRANSIENT:
	case SHELL_SURFACE_FULLSCREEN:
	case SHELL_SURFACE_MAXIMIZED:
		if (!gShell.locked)
			activate(shell, surface,
				 (tFocus *)
				 compositor->input_device);
		break;
	default:
		break;
	}
	dTrace_L("");
	
	shell_restack();
	
//	if (surface_type == SHELL_SURFACE_TOPLEVEL)
//		weston_zoom_run(surface, 0.8, 1.0, NULL, NULL);
}

void		configure		(struct wl_shell *shell, tSurf *surface, GLfloat x, GLfloat y, int32_t width, int32_t height)
{
	enum shell_surface_type surface_type = SHELL_SURFACE_NONE;
	enum shell_surface_type prev_surface_type = SHELL_SURFACE_NONE;
	tWin *shsurf;
	
	shsurf = get_shell_surface(surface);
	if (shsurf)
		surface_type = shsurf->type;
	
//	surface->geometry_prev = surface->geometry;
	
	surface->geometry.x = x;
	surface->geometry.y = y;
	surface->geometry.width = width;
	surface->geometry.height = height;
	surface->geometry.dirty = 1;

	switch (surface_type) {
	case SHELL_SURFACE_SCREENSAVER:
		center_on_output(surface, shsurf->fullscreen_output);
		break;
	case SHELL_SURFACE_FULLSCREEN:
		shell_configure_fullscreen(shsurf);
		if (prev_surface_type != SHELL_SURFACE_FULLSCREEN)
			shell_stack_fullscreen(shsurf);
		break;
	case SHELL_SURFACE_MAXIMIZED:
		/* setting x, y and using configure to change that geometry */
		surface->geometry.x = surface->output->x;
		surface->geometry.y = surface->output->y +
			get_output_panel_height(shell,surface->output);
		break;
	case SHELL_SURFACE_TOPLEVEL:
		break;
	default:
		break;
	}

	/* XXX: would a fullscreen surface need the same handling? */
	if (surface->output) {
		weston_surface_assign_output(surface);

		if (surface_type == SHELL_SURFACE_SCREENSAVER)
			surface->output = shsurf->output;
		else if (surface_type == SHELL_SURFACE_MAXIMIZED)
			surface->output = shsurf->output;
	}
	
}


void		shell_surface_configure		(tSurf *es, int32_t sx, int32_t sy)
{
	tWin *shsurf = get_shell_surface(es);
	struct wl_shell *shell = shsurf->shell;

	if (!weston_surface_is_mapped(es)) {
		map(shell, es, es->buffer->width, es->buffer->height, sx, sy);
	} else if (shsurf->force_configure || sx != 0 || sy != 0 ||
		   es->geometry.width != es->buffer->width ||
		   es->geometry.height != es->buffer->height
	) {
		int32_t from_x, from_y;
		int32_t to_x, to_y;
		
		weston_surface_to_global (es, 0, 0, &from_x, &from_y);
		weston_surface_to_global (es, sx, sy, &to_x, &to_y);
		configure(shell, es,
			  es->geometry.x + to_x - from_x,
			  es->geometry.y + to_y - from_y,
			  es->buffer->width, es->buffer->height);
		shsurf->force_configure = 0;
	}
}



void		shell_get_shell_surface		(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t id,
			struct wl_resource *surface_resource)
{
	dTrace_E("");
	tSurf *surface = surface_resource->data;
	tWin *shsurf;

	if (get_shell_surface(surface)) {
		wl_resource_post_error(surface_resource,
			WL_DISPLAY_ERROR_INVALID_OBJECT,
			"wl_shell::get_shell_surface already requested");
		return;
	}

	if (surface->configure) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->configure already set");
		return;
	}

	shsurf = calloc(1, sizeof *shsurf);
	if (!shsurf) {
		wl_resource_post_no_memory(resource);
		return;
	}

	surface->configure = shell_surface_configure;

	shsurf->resource.destroy = destroy_shell_surface;
	shsurf->resource.object.id = id;
	shsurf->resource.object.interface = &wl_shell_surface_interface;
	shsurf->resource.object.implementation =
		(void (**)(void)) &shell_surface_implementation;
	shsurf->resource.data = shsurf;

	shsurf->shell = resource->data;
	shsurf->saved_position_valid = false;
	shsurf->surface = surface;
	shsurf->fullscreen.type = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;
	shsurf->fullscreen.framerate = 0;
	shsurf->fullscreen.black_surface = NULL;
	wl_list_init(&shsurf->fullscreen.transform.link);

	shsurf->surface_destroy_listener.notify = shell_handle_surface_destroy;
	wl_signal_add(&surface->surface.resource.destroy_signal,
		      &shsurf->surface_destroy_listener);

	/* init link so its safe to always remove it in destroy_shell_surface */
	wl_list_init(&shsurf->link);
	
	/* empty when not in use */
//	wl_list_init(&shsurf->rotation.transform.link);
//	weston_matrix_init(&shsurf->rotation.rotation);
	
	shsurf->type = SHELL_SURFACE_NONE;
	
	shsurf->L = L_eNorm;
	wl_list_insert(&gShell.L[shsurf->L], &shsurf->L_link);
	
	
	
	shsurf->Tags = 0;
	wl_list_init(&shsurf->O_link);
	
	wl_client_add_resource(client, &shsurf->resource);
	dTrace_L("");
}

tWin*
Shell_get_surface(struct wl_client *client, tSurf *surface)
{
	dTrace_E("");
	tWin *shsurf;

	if (get_shell_surface(surface)) {
	//	wl_resource_post_error(surface_resource,
	//		WL_DISPLAY_ERROR_INVALID_OBJECT,
	//		"wl_shell::get_shell_surface already requested");
		return;
	}
	
	if (surface->configure) {
	//	wl_resource_post_error(surface_resource,
	//			       WL_DISPLAY_ERROR_INVALID_OBJECT,
	//			       "surface->configure already set");
		return;
	}
	
	shsurf = calloc(1, sizeof *shsurf);
	if (!shsurf) {
	//	wl_resource_post_no_memory(resource);
		return;
	}
	
	surface->configure = shell_surface_configure;
	
	shsurf->resource.destroy = destroy_shell_surface;
	shsurf->resource.object.id = 0;
	shsurf->resource.object.interface = &wl_shell_surface_interface;
	shsurf->resource.object.implementation =
		(void (**)(void)) &shell_surface_implementation;
	shsurf->resource.data = shsurf;
	
	shsurf->shell = &gShell;
	shsurf->saved_position_valid = false;
	shsurf->surface = surface;
	shsurf->fullscreen.type = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;
	shsurf->fullscreen.framerate = 0;
	shsurf->fullscreen.black_surface = NULL;
	wl_list_init(&shsurf->fullscreen.transform.link);

	shsurf->surface_destroy_listener.notify = shell_handle_surface_destroy;
	wl_signal_add(&surface->surface.resource.destroy_signal,
		      &shsurf->surface_destroy_listener);
	
	/* init link so its safe to always remove it in destroy_shell_surface */
	wl_list_init(&shsurf->link);
	
	shsurf->type = SHELL_SURFACE_NONE;
	
	shsurf->L = L_eNorm;
	wl_list_insert(&gShell.L[shsurf->L], &shsurf->L_link);
	
	
	
	shsurf->Tags = 0;
	wl_list_init(&shsurf->O_link);

	wl_client_add_resource(client, &shsurf->resource);
	dTrace_L("");
	return shsurf;
}


/** ***********************************  shell other ************************************ **/


/* no-op func for checking black surface */
static void
black_surface_configure(tSurf *es, int32_t sx, int32_t sy)
{
}

static bool 
is_black_surface (tSurf *es, tSurf **fs_surface)
{
	if (es->configure == black_surface_configure) {
		if (fs_surface)
			*fs_surface = (tSurf *)es->private;
		return true;
	}
	return false;
}

static void
click_to_activate_binding(struct wl_input_device *device,
			  uint32_t time, uint32_t key,
			  uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tFocus *wd = (tFocus *) device;
	struct wl_shell *shell = data;
	tSurf *focus;
	tSurf *upper;

	focus = (tSurf *) device->pointer_focus;
	if (!focus)
		return;

	if (is_black_surface(focus, &upper))
		focus = upper;

	if (state && device->pointer_grab == &device->default_pointer_grab)
		activate(shell, focus, wd);
}

static void
lock(struct wl_listener *listener, void *data)
{
	exit(1);
	struct wl_shell *shell =
		container_of(listener, struct wl_shell, lock_listener);
	tFocus *device;
	tWin *shsurf;
	tOutput *output;

	if (gShell.locked) {
		wl_list_for_each(output, &gShell.pEC->output_list, link)
			/* TODO: find a way to jump to other DPMS levels */
			if (output->set_dpms)
				output->set_dpms(output, WESTON_DPMS_STANDBY);
		return;
	}

	gShell.locked = true;

	/* Hide all surfaces by removing the fullscreen, panel and
	 * toplevel layers.  This way nothing else can show or receive
	 * input events while we are locked. */

//	wl_list_remove(&gShell.panel_layer.link);
//	wl_list_remove(&gShell.toplevel_layer.link);
//	wl_list_remove(&gShell.fullscreen_layer.link);
//	wl_list_insert(&gShell.pEC->cursor_layer.link, &gShell.lock_layer.link);

	launch_screensaver(shell);

	wl_list_for_each(shsurf, &gShell.screensaver.surfaces, link)
		show_screensaver(shell, shsurf);

	if (!wl_list_empty(&gShell.screensaver.surfaces)) {
		gShell.pEC->idle_time = gShell.screensaver.duration;
		weston_compositor_wake(gShell.pEC);
		gShell.pEC->state = WESTON_COMPOSITOR_IDLE;
	}

	/* reset pointer foci */
	weston_compositor_schedule_repaint(gShell.pEC);

	/* reset keyboard foci */
	wl_list_for_each(device, &gShell.pEC->input_device_list, link) {
		wl_input_device_set_keyboard_focus(&device->input_device,
						   NULL);
	}

	/* TODO: disable bindings that should not work while locked. */

	/* All this must be undone in resume_desktop(). */
}

static void
unlock(struct wl_listener *listener, void *data)
{
	struct wl_shell *shell =
		container_of(listener, struct wl_shell, unlock_listener);

	if (!gShell.locked || gShell.lock_surface) {
		weston_compositor_wake(gShell.pEC);
		return;
	}

	/* If desktop-shell client has gone away, unlock immediately. */
	if (!gShell.child.desktop_shell) {
		resume_desktop(shell);
		return;
	}

	if (gShell.prepare_event_sent)
		return;

	desktop_shell_send_prepare_lock_surface(gShell.child.desktop_shell);
	gShell.prepare_event_sent = true;
}

static void
center_on_output(tSurf *surface, tOutput *output)
{
	struct weston_mode *mode = output->current;
	GLfloat x = (mode->width - surface->geometry.width) / 2;
	GLfloat y = (mode->height - surface->geometry.height) / 2;

	weston_surface_set_position(surface, output->x + x, output->y + y);
}


static int launch_desktop_shell_process(struct wl_shell *shell);

static void
desktop_shell_sigchld(struct weston_process *process, int status)
{
	uint32_t time;
	struct wl_shell *shell =
		container_of(process, struct wl_shell, child.process);

	gShell.child.process.pid = 0;
	gShell.child.client = NULL; /* already destroyed by wayland */

	/* if desktop-shell dies more than 5 times in 30 seconds, give up */
	time = weston_compositor_get_time();
	if (time - gShell.child.deathstamp > 30000) {
		gShell.child.deathstamp = time;
		gShell.child.deathcount = 0;
	}

	gShell.child.deathcount++;
	if (gShell.child.deathcount > 5) {
		fprintf(stderr, "weston-desktop-shell died, giving up.\n");
		return;
	}

	fprintf(stderr, "weston-desktop-shell died, respawning...\n");
	launch_desktop_shell_process(shell);
}

static int
launch_desktop_shell_process(struct wl_shell *shell)
{
	const char *shell_exe = LIBEXECDIR "/weston-desktop-shell";

	gShell.child.client = weston_client_launch(gShell.pEC,
						 &gShell.child.process,
						 shell_exe,
						 desktop_shell_sigchld);

	if (!gShell.child.client)
		return -1;
	return 0;
}

static void
bind_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_shell *shell = data;

	wl_client_add_object(client, &wl_shell_interface,
			     &shell_implementation, id, shell);
}

static void
unbind_desktop_shell(struct wl_resource *resource)
{
	struct wl_shell *shell = resource->data;

	if (gShell.locked)
		resume_desktop(shell);

	gShell.child.desktop_shell = NULL;
	gShell.prepare_event_sent = false;
	free(resource);
}

static void
bind_desktop_shell(struct wl_client *client,
		   void *data, uint32_t version, uint32_t id)
{
	struct wl_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &desktop_shell_interface,
					&desktop_shell_implementation,
					id, shell);

	if (client == gShell.child.client) {
		resource->destroy = unbind_desktop_shell;
		gShell.child.desktop_shell = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "permission to bind desktop_shell denied");
	wl_resource_destroy(resource);
}

/*
static void
screensaver_set_surface(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *shell_surface_resource,
			struct wl_resource *output_resource)
{
	struct wl_shell *shell = resource->data;
	tWin *surface = shell_surface_resource->data;
	tOutput *output = output_resource->data;

	if (reset_shell_surface_type(surface))
		return;

	surface->type = SHELL_SURFACE_SCREENSAVER;

	surface->fullscreen_output = output;
	surface->output = output;
	wl_list_insert(gShell.screensaver.surfaces.prev, &surface->link);
}

static const struct screensaver_interface screensaver_implementation = {
	screensaver_set_surface
};

static void
unbind_screensaver(struct wl_resource *resource)
{
	struct wl_shell *shell = resource->data;

	gShell.screensaver.binding = NULL;
	free(resource);
}

static void
bind_screensaver(struct wl_client *client,
		 void *data, uint32_t version, uint32_t id)
{
	struct wl_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &screensaver_interface,
					&screensaver_implementation,
					id, shell);

	if (gShell.screensaver.binding == NULL) {
		resource->destroy = unbind_screensaver;
		gShell.screensaver.binding = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "interface object already bound");
	wl_resource_destroy(resource);
}
/**/


/** *********************************** shell tab_switcher ************************************ **/
struct switcher {
	tSurf *current;
	struct wl_listener listener;
	struct wl_keyboard_grab grab;
};

static void
switcher_next(struct switcher *switcher)
{
	tComp *compositor = gShell.pEC;
	tSurf *surface;
	tSurf *first = NULL, *prev = NULL, *next = NULL;
	tWin *shsurf;

	wl_list_for_each(surface, &compositor->surface_list, link) {
		switch (get_shell_surface_type(surface)) {
		case SHELL_SURFACE_TOPLEVEL:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_MAXIMIZED:
			if (first == NULL)
				first = surface;
			if (prev == switcher->current)
				next = surface;
			prev = surface;
			surface->alpha = 64;
			surface->geometry.dirty = 1;
			weston_surface_damage(surface);
			break;
		default:
			break;
		}

		if (is_black_surface(surface, NULL)) {
			surface->alpha = 64;
			surface->geometry.dirty = 1;
			weston_surface_damage(surface);
		}
	}

	if (next == NULL)
		next = first;

	if (next == NULL)
		return;

	wl_list_remove(&switcher->listener.link);
	wl_signal_add(&next->surface.resource.destroy_signal,
		      &switcher->listener);

	switcher->current = next;
	next->alpha = 255;

	shsurf = get_shell_surface(switcher->current);
	if (shsurf && shsurf->type ==SHELL_SURFACE_FULLSCREEN)
		shsurf->fullscreen.black_surface->alpha = 255;
}

static void
switcher_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct switcher *switcher =
		container_of(listener, struct switcher, listener);

	switcher_next(switcher);
}

static void
switcher_destroy(struct switcher *switcher, uint32_t time)
{
	tComp *compositor = gShell.pEC;
	tSurf *surface;
	tFocus *device =
		(tFocus *) switcher->grab.input_device;

	wl_list_for_each(surface, &compositor->surface_list, link) {
		surface->alpha = 255;
		weston_surface_damage(surface);
	}

	if (switcher->current)
		activate(&gShell, switcher->current, device);
	wl_list_remove(&switcher->listener.link);
	wl_input_device_end_keyboard_grab(&device->input_device);
	free(switcher);
}

static void
switcher_key(struct wl_keyboard_grab *grab,
	     uint32_t time, uint32_t key, int32_t state)
{
	struct switcher *switcher = container_of(grab, struct switcher, grab);
	tFocus *device =
		(tFocus *) grab->input_device;

	if ((device->modifier_state & MODIFIER_SUPER) == 0) {
		switcher_destroy(switcher, time);
	} else if (key == KEY_TAB && state) {
		switcher_next(switcher);
	}
};

static const struct wl_keyboard_grab_interface switcher_grab = {
	switcher_key
};

static void
switcher_binding(struct wl_input_device *device, uint32_t time,
		 uint32_t key, uint32_t button, uint32_t axis,
		 int32_t state, void *data)
{
	struct switcher *switcher;

	switcher = malloc(sizeof *switcher);
	switcher->current = NULL;
	switcher->listener.notify = switcher_handle_surface_destroy;
	wl_list_init(&switcher->listener.link);

	switcher->grab.interface = &switcher_grab;
	wl_input_device_start_keyboard_grab(device, &switcher->grab);
	wl_input_device_set_keyboard_focus(device, NULL);
	switcher_next(switcher);
}



/** *********************************** NEW ************************************ **/

/** *********************************** GLOBAL ************************************ **/

tOutput*	CurrentOutput	()
{
	if (gShell.pEC->input_device
		&& ((tFocus*) gShell.pEC->input_device)->sprite
		&& ((tFocus*) gShell.pEC->input_device)->sprite->output
	)
		return ((tFocus*) gShell.pEC->input_device)->sprite->output;
	
	return container_of(&gShell.pEC->output_list, tOutput, link);
}


bool		Tag_isVisible		(tTags tags)
{
	tOutput *iout;
	wl_list_for_each(iout, &gShell.pEC->output_list, link) {
		if (iout->Tags & tags) {
			return 1;
		}
	}
	return 0;
}

Bool		Tag_isVisibleOnOther	(tTags tags, tOutput *output)
{
	tOutput *iout;
	wl_list_for_each(iout, &gShell.pEC->output_list, link) {
		if (iout == output)
			continue;
		if (iout->Tags & tags) {
			return 1;
		}
	}
	return 0;
}



/** *********************************** Output ************************************ **/


bool		Output_TagisVisible		(tOutput *out, tTags tags)
{
	if (out->Tags & tags)
		return 1;
	return 0;
}

void		Output_TagSet			(tOutput *out, tTags tags)
{
	out->Tags = tags;
	
	desktop_shell_send_select_tag(gShell.child.desktop_shell, out->x, out->Tags);
	gShell.prepare_event_sent = true;
}

void		Output_TagView			(tOutput *out, tTags newtag)
{
	tOutput *iout;
	wl_list_for_each(iout, &gShell.pEC->output_list, link) {
		
		if (iout != out && Output_TagisVisible (iout, newtag)) {
			Output_TagSet (iout, out->Tags);
			Output_TagSet (out, newtag);
			
			shell_restack();
		//	arrange (iout);
		//	arrange (curout);
			
		//	Queue_DrawBar (iout);
		//	Queue_DrawBar (selmon);
			return;
		}
	}
	if (newtag) {
		Output_TagSet (out, newtag);
	}
	shell_restack();
//	arrange (curout);
	
//	Queue_DrawBar (curout);
}



tWin*	
Output_PanelGet		(tOutput *output)
{
	tWin *priv;
	
	wl_list_for_each(priv, &gShell.panels, link) {
		if (priv->output == output) {
			return priv;
		}
	}
	return 0;
}



/** *********************************** Shell_Surf ************************************ **/

void		ShSurf_LSet		(tWin* shsurf, uint8_t l)
{
	if (l == shsurf->L)
		return;
	wl_list_remove (&shsurf->L_link);
	shsurf->L = l;
	wl_list_insert(gShell.L[shsurf->L].prev, &shsurf->L_link);
        shell_restack();
}



/** *********************************** Act ************************************ **/

static void		Act_Client_TagSet		(struct wl_input_device *device, uint32_t time, uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tSurf* surf = gShell.pEC->input_device->current;
	printf ("Act_Client_TagSet surf %lx\n", surf);
	if (!surf)
		return;
	
	tWin* shsurf = get_shell_surface(surf);
	shsurf->Tags = data;
	shell_restack();
	
}

static void		Act_Client_Unfloat	(struct wl_input_device *device, uint32_t time, uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tSurf* surf = gShell.pEC->input_device->current;
	if (!surf)
		return;
	tWin* shsurf = get_shell_surface(surf);
        if(surf)
          ShSurf_LSet(shsurf, L_eNorm);
}

static void		Act_Output_TagSet		(struct wl_input_device *device, uint32_t time, uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tSurf* surf = gShell.pEC->input_device->current;
	tOutput* out = CurrentOutput();
	
	Output_TagView (out, data);
	
	printf ("Output_TagSet %lx\n", out->Tags);
}

static void		Act_Surf_Teleport		(struct wl_input_device *device, uint32_t time, uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tSurf* surf = gShell.pEC->input_device->current;
	if (!surf)
		return;
	tWin* shsurf = get_shell_surface(surf);
	struct wl_list* list;
	if ((int)data > 0) {
		list = surf->output->link.next;
		if (list == &gShell.pEC->output_list) {
			list = gShell.pEC->output_list.next;
		}
	}else {
		list = surf->output->link.prev;
		if (list == &gShell.pEC->output_list) {
			list = gShell.pEC->output_list.prev;
		}
	}
	tOutput* tgt = container_of(list, tOutput, link);
	shsurf->Tags = tgt->Tags;
	shsurf->output = tgt;
	shell_restack();
}




/** *********************************** shell main ************************************ **/

void layout(tOutput* output)
{
	int n, cols, rows, cn, rn, i, cx, cy, cw, ch, mw, mh, mx, my;
	tWin* c;
	
	mw = output->region.extents.x2 - output->region.extents.x1;
	mh = output->region.extents.y2 - output->region.extents.y1 - 32;
	mx = output->region.extents.x1;
	my = output->region.extents.y1 + 32;
	printf("mw:%d mh:%d mx:%d my:%d\n", mw, mh, mx, my);
	
	n = wl_list_length(&output->surfaces);
	if(n == 0)
		return;
	
	/* grid dimensions */
	for(cols = 0; cols <= n/2; cols++)
		if(cols*cols >= n)
			break;
	if(n == 5) /* set layout against the general calculation: not 1:2:2, but 2:3 */
		cols = 2;
	rows = n/cols;
	
	/* window geometries */
	cw = cols ? mw / cols : mw;
	cn = 0; /* current column number */
	rn = 0; /* current row number */
	i = 0; 
	wl_list_for_each(c, &output->surfaces, O_link) {
		if(i/rows + 1 > cols - n%cols)
			rows = n/cols + 1;
		ch = rows ? mh / rows : mh;
		cx = mx + cn*cw;
		cy = my + rn*ch;
		//resize(c, cx, cy, cw, ch, False);
	//	printf("x:%d y:%d w:%d h:%d", cx, cy, cw, ch);
		tSurf* es = c->surface;
		int32_t esml = es->border.left;
		int32_t esmt = es->border.top;
		int32_t esmr = es->border.right;
		int32_t esmb = es->border.bottom;/**/
	/*	int32_t esml = 0;
		int32_t esmt = 0;
		int32_t esmr = 0;
		int32_t esmb = 0;/**/
	/*	int32_t esml = 32;
		int32_t esmt = 32;
		int32_t esmr = 32;
		int32_t esmb = 32;/**/
		if (es->geometry_ours.x != cx - esml
			|| es->geometry_ours.y != cy - esmt
			|| es->geometry_ours.width != cw + esml + esmr
			|| es->geometry_ours.height != ch + esmt + esmb
		) {
			es->geometry_ours.x = cx - esml;
			es->geometry_ours.y = cy - esmt;
			es->geometry_ours.width = cw + esml + esmr;
			es->geometry_ours.height = ch + esmt + esmb;
			
			if (es->client == gpXServer->client) {
				weston_surface_configure(es, cx - esml, cy - esmt, es->geometry.width, es->geometry.height);
			//	printf ("HAUHTEONSUHETONSUHTENSOUHTEOTNSUHEOTNSUHTEOSUHTNSEOU\n");
				weston_wm_window_resize (gpXServer->wm, es, cx - esml, cy - esmt, cw + esml + esmr, ch + esmt + esmb, 0);
			}else {
				weston_surface_configure(es, cx - esml, cy - esmt, cw + esml + esmr, ch + esmt + esmb);
				wl_shell_surface_send_configure(&c->resource, 0, cw + esml + esmr, ch + esmt + esmb);
			}
			weston_surface_assign_output (es);
		}
		rn++;
		if(rn >= rows) {
			rn = 0;
			cn++;
		}
		i++;
	}
}

void shell_restack()
{
//	return;
	dTrace_E("");
	
	tOutput *output, *tmp_output;
	tSurf* es;
	tWin *shsurf;
	
	shell_L_print (&gShell);
	
	wl_list_for_each(output, &gShell.pEC->output_list, link) {
		printf("\noutput TAG: %lx\n", output->Tags);
		printf("xy %d %d	wh %d %d\n",
			output->region.extents.x1,
			output->region.extents.y1,
			output->region.extents.x2,
			output->region.extents.y2
		);
	//	printf("mw:%d mh:%d mx:%d my:%d\n", output->mm_width, output->mm_height, output->x, output->y);
		wl_list_init(&output->surfaces);
	}
	printf("\n");
	
	wl_list_init(&gShell.pEC->surface_list);
	
	wl_list_for_each(shsurf, &gShell.L[L_eNorm], L_link) {
		if (shsurf->type == SHELL_SURFACE_BACKGROUND
			|| shsurf->type == SHELL_SURFACE_PANEL
		) {
		//	printf("surf TAG: %x\n", shsurf->Tags);
			wl_list_insert(&gShell.pEC->surface_list, &shsurf->surface->link);
		}
	}
	int i;
	for (i = 0; i < L_NUM; ++i) {
		wl_list_for_each(shsurf, &gShell.L[i], L_link) {
			wl_list_for_each(output, &gShell.pEC->output_list, link) {
				if (shsurf->type == SHELL_SURFACE_BACKGROUND
					|| shsurf->type == SHELL_SURFACE_PANEL
				)
					continue;
				if (shsurf->Tags & output->Tags) {
				//	printf("surf TAG: %x\n", shsurf->Tags);
				//	shsurf->output = output;
					wl_list_insert(&gShell.pEC->surface_list, &shsurf->surface->link);
					if (i == L_eNorm)
						wl_list_insert(&output->surfaces, &shsurf->O_link);
					break;
				}
			}
		}
	}
	
	tFocus* wid;
/*	wl_list_for_each(wid, &gShell.pEC->input_device_list, link) {
		printf ("wid %lx\n", wid);
		wl_list_insert(&gShell.pEC->surface_list, &wid->sprite->link);
	//	weston_surface_assign_output(wid->sprite);
	}*/
	wl_list_for_each(output, &gShell.pEC->output_list, link) {
		layout(output);
	}
	
//	weston_compositor_damage_all(gShell.pEC);
	dTrace_L("");
}

static void
backlight_binding(struct wl_input_device *device, uint32_t time,
		  uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	tComp *compositor = data;
	tOutput *output;
	long backlight_new = 0;

	/* TODO: we're limiting to simple use cases, where we assume just
	 * control on the primary display. We'd have to extend later if we
	 * ever get support for setting backlights on random desktop LCD
	 * panels though */
	output = get_default_output(compositor);
	if (!output)
		return;

	if (!output->set_backlight)
		return;

	if (key == KEY_F9 || key == KEY_BRIGHTNESSDOWN)
		backlight_new = output->backlight_current - 25;
	else if (key == KEY_F10 || key == KEY_BRIGHTNESSUP)
		backlight_new = output->backlight_current + 25;

	if (backlight_new < 5)
		backlight_new = 5;
	if (backlight_new > 255)
		backlight_new = 255;

	output->backlight_current = backlight_new;
	output->set_backlight(output, output->backlight_current);
}

static void
debug_repaint_binding(struct wl_input_device *device, uint32_t time,
		      uint32_t key, uint32_t button, uint32_t axis, int32_t state, void *data)
{
	struct wl_shell *shell = data;
	tComp *compositor = gShell.pEC;
	tSurf *surface;

	if (gShell.debug_repaint_surface) {
		weston_surface_destroy(gShell.debug_repaint_surface);
		gShell.debug_repaint_surface = NULL;
	} else {
		surface = weston_surface_create(compositor);
		weston_surface_set_color(surface, 1.0, 0.0, 0.0, 0.2);
		weston_surface_configure(surface, 0, 0, 8192, 8192);
	//	wl_list_insert(&compositor->fade_layer.surface_list, &surface->layer_link);
		weston_surface_assign_output(surface);
		pixman_region32_init(&surface->input);

		/* Here's the dirty little trick that makes the
		 * repaint debugging work: we force an
		 * update_transform first to update dependent state
		 * and clear the geometry.dirty bit.  Then we clear
		 * the surface damage so it only gets repainted
		 * piecewise as we repaint other things.  */

		weston_surface_update_transform(surface);
		pixman_region32_fini(&surface->damage);
		pixman_region32_init(&surface->damage);
		gShell.debug_repaint_surface = surface;
	}
}

static void
shell_destroy(struct wl_listener *listener, void *data)
{
	struct wl_shell *shell =
		container_of(listener, struct wl_shell, destroy_listener);

	if (gShell.child.client)
		wl_client_destroy(gShell.child.client);

	free(gShell.screensaver.path);
//	free(shell);
}

int
shell_init(tComp *ec);

WL_EXPORT int
shell_init(tComp *ec)
{
	struct wl_shell *shell = &gShell;

//	shell = malloc(sizeof *shell);
//	if (shell == NULL)
//		return -1;

	memset(shell, 0, sizeof *shell);
	gShell.pEC = ec;
	int i;
	for (i=0; i<L_NUM; i++) {
		wl_list_init(&gShell.L[i]);
	}
	gShell.destroy_listener.notify = shell_destroy;
	wl_signal_add(&ec->destroy_signal, &gShell.destroy_listener);
	gShell.lock_listener.notify = lock;
	wl_signal_add(&ec->lock_signal, &gShell.lock_listener);
	gShell.unlock_listener.notify = unlock;
	wl_signal_add(&ec->unlock_signal, &gShell.unlock_listener);

	wl_list_init(&gShell.backgrounds);
	wl_list_init(&gShell.panels);
	wl_list_init(&gShell.screensaver.surfaces);

//	weston_layer_init(&gShell.fullscreen_layer, &ec->cursor_layer.link);
//	weston_layer_init(&gShell.panel_layer, &gShell.fullscreen_layer.link);
//	weston_layer_init(&gShell.toplevel_layer, &gShell.panel_layer.link);
//	weston_layer_init(&gShell.background_layer, &gShell.toplevel_layer.link);
//	wl_list_init(&gShell.lock_layer.surface_list);

	shell_configuration(shell);

	if (wl_display_add_global(ec->wl_display, &wl_shell_interface,
				  shell, bind_shell) == NULL)
		return -1;

	if (wl_display_add_global(ec->wl_display,
				  &desktop_shell_interface,
				  shell, bind_desktop_shell) == NULL)
		return -1;

//	if (wl_display_add_global(ec->wl_display, &screensaver_interface,
//				  shell, bind_screensaver) == NULL)
//		return -1;

	gShell.child.deathstamp = weston_compositor_get_time();
	if (launch_desktop_shell_process(shell) != 0)
		return -1;
	
	weston_compositor_add_binding(ec, 0, BTN_LEFT, 0, dModKey,
					move_binding, shell);
	weston_compositor_add_binding(ec, 0, BTN_MIDDLE, 0, dModKey,
					resize_binding, shell);
	weston_compositor_add_binding(ec, 0, BTN_RIGHT, 0, dModKey,
					resize_binding, shell);
	weston_compositor_add_binding(ec, KEY_BACKSPACE, 0, 0,
					MODIFIER_CTRL | MODIFIER_ALT,
					terminate_binding, ec);
	weston_compositor_add_binding(ec, 0, BTN_LEFT, 0, 0,
					click_to_activate_binding, shell);
	weston_compositor_add_binding(ec, 0, 0, WL_INPUT_DEVICE_AXIS_VERTICAL_SCROLL,
					dModKey | MODIFIER_ALT,
					surface_opacity_binding, NULL);
	weston_compositor_add_binding(ec, 0, 0, WL_INPUT_DEVICE_AXIS_VERTICAL_SCROLL,
					dModKey, zoom_binding, NULL);
//	weston_compositor_add_binding(ec, 0, BTN_LEFT, 0,
//					dModKey | MODIFIER_ALT,
//					rotate_binding, NULL);
	weston_compositor_add_binding(ec, KEY_TAB, 0, 0, dModKey,
					switcher_binding, shell);

	/* brightness */
	weston_compositor_add_binding(ec, KEY_F9, 0, 0, MODIFIER_CTRL,
					backlight_binding, ec);
	weston_compositor_add_binding(ec, KEY_BRIGHTNESSDOWN, 0, 0, 0,
					backlight_binding, ec);
	weston_compositor_add_binding(ec, KEY_F10, 0, 0, MODIFIER_CTRL,
					backlight_binding, ec);
	weston_compositor_add_binding(ec, KEY_BRIGHTNESSUP, 0, 0, 0,
					backlight_binding, ec);

	weston_compositor_add_binding(ec, KEY_SPACE, 0, 0, dModKey,
					debug_repaint_binding, shell);
	
	
	
	weston_compositor_add_binding(ec, KEY_A, 0, 0, dModKey, Act_Output_TagSet, 1 << 0);
	weston_compositor_add_binding(ec, KEY_S, 0, 0, dModKey, Act_Output_TagSet, 1 << 1);
	weston_compositor_add_binding(ec, KEY_D, 0, 0, dModKey, Act_Output_TagSet, 1 << 2);
	weston_compositor_add_binding(ec, KEY_F, 0, 0, dModKey, Act_Output_TagSet, 1 << 3);
	weston_compositor_add_binding(ec, KEY_G, 0, 0, dModKey, Act_Output_TagSet, 1 << 4);
	weston_compositor_add_binding(ec, KEY_H, 0, 0, dModKey, Act_Output_TagSet, 1 << 5);
	weston_compositor_add_binding(ec, KEY_J, 0, 0, dModKey, Act_Output_TagSet, 1 << 6);
	weston_compositor_add_binding(ec, KEY_K, 0, 0, dModKey, Act_Output_TagSet, 1 << 7);
	
	
	weston_compositor_add_binding(ec, KEY_A, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 0);
	weston_compositor_add_binding(ec, KEY_S, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 1);
	weston_compositor_add_binding(ec, KEY_D, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 2);
	weston_compositor_add_binding(ec, KEY_F, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 3);
	weston_compositor_add_binding(ec, KEY_G, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 4);
	weston_compositor_add_binding(ec, KEY_H, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 5);
	weston_compositor_add_binding(ec, KEY_J, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 6);
	weston_compositor_add_binding(ec, KEY_K, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_TagSet, 1 << 7);
	
	weston_compositor_add_binding(ec, KEY_Q, 0, 0, dModKey | MODIFIER_CTRL, Act_Surf_Teleport, 1);
	weston_compositor_add_binding(ec, KEY_W, 0, 0, dModKey | MODIFIER_CTRL, Act_Surf_Teleport, -1);

	weston_compositor_add_binding(ec, KEY_Z, 0, 0, dModKey | MODIFIER_CTRL, Act_Client_Unfloat, 0);
	
	shell_restack();
	
	return 0;
}


