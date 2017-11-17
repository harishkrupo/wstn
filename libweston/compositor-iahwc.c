/*
 * Copyright © 2011-2017 Harish Krupo
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <assert.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <gbm.h>
#include <libudev.h>

#include <libsync.h>

#include <iahwc.h>

#include "compositor.h"
#include "compositor-iahwc.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "gl-renderer.h"
#include "weston-egl-ext.h"
#include "pixman-renderer.h"
#include "pixel-formats.h"
#include "libbacklight.h"
#include "libinput-seat.h"
#include "launcher-util.h"
#include "vaapi-recorder.h"
#include "presentation-time-server-protocol.h"
#include "linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"

#ifndef GBM_BO_USE_CURSOR
#define GBM_BO_USE_CURSOR GBM_BO_USE_CURSOR_64X64
#endif

struct iahwc_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

  iahwc_module_t* iahwc_module;
  iahwc_device_t* iahwc_device;

	struct udev *udev;
	struct wl_event_source *iahwc_source;

	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_iahwc_source;

	struct {
		int id;
		int fd;
		char *filename;
	} iahwc;

	struct gbm_device *gbm;
	struct wl_listener session_listener;
	uint32_t gbm_format;

  IAHWC_PFN_GET_NUM_DISPLAYS iahwc_get_num_displays;
  IAHWC_PFN_REGISTER_CALLBACK iahwc_register_callback;
  IAHWC_PFN_DISPLAY_GET_INFO iahwc_get_display_info;
  IAHWC_PFN_DISPLAY_GET_NAME iahwc_get_display_name;
  IAHWC_PFN_DISPLAY_GET_CONFIGS iahwc_get_display_configs;
  IAHWC_PFN_DISPLAY_SET_GAMMA iahwc_set_display_gamma;
  IAHWC_PFN_DISPLAY_SET_CONFIG iahwc_set_display_config;
  IAHWC_PFN_DISPLAY_GET_CONFIG iahwc_get_display_config;
  IAHWC_PFN_DISPLAY_CLEAR_ALL_LAYERS iahwc_display_clear_all_layers;
  IAHWC_PFN_PRESENT_DISPLAY iahwc_present_display;
  IAHWC_PFN_CREATE_LAYER iahwc_create_layer;
  IAHWC_PFN_LAYER_SET_BO iahwc_layer_set_bo;
  IAHWC_PFN_LAYER_SET_SOURCE_CROP iahwc_layer_set_source_crop;
  IAHWC_PFN_LAYER_SET_DISPLAY_FRAME iahwc_layer_set_display_frame;
  IAHWC_PFN_LAYER_SET_SURFACE_DAMAGE iahwc_layer_set_surface_damage;
  IAHWC_PFN_LAYER_SET_ACQUIRE_FENCE iahwc_layer_set_acquire_fence;
  IAHWC_PFN_LAYER_SET_USAGE iahwc_layer_set_usage;

	/* we need these parameters in order to not fail iahwcModeAddFB2()
	 * due to out of bounds dimensions, and then mistakenly set
	 * sprites_are_broken:
	 */
	int min_width, max_width;
	int min_height, max_height;
	int no_addfb2;

	struct wl_list plane_list;
	int sprites_are_broken;
	int sprites_hidden;

	void *repaint_data;

	int cursors_are_broken;

	bool universal_planes;

	int use_pixman;

	struct udev_input input;

	int32_t cursor_width;
	int32_t cursor_height;

	uint32_t pageflip_timeout;
};

struct iahwc_mode {
	struct weston_mode base;
	uint32_t config_id;
};

enum iahwc_fb_type {
	BUFFER_INVALID = 0, /**< never used */
	BUFFER_CLIENT, /**< directly sourced from client */
	BUFFER_PIXMAN_DUMB, /**< internal Pixman rendering */
	BUFFER_GBM_SURFACE, /**< internal EGL rendering */
	BUFFER_CURSOR, /**< internal cursor buffer */
};

struct iahwc_fb {
	enum iahwc_fb_type type;

	int refcnt;

	uint32_t fb_id, stride, handle, size;
	const struct pixel_format_info *format;
	int width, height;
	int fd;
	struct weston_buffer_reference buffer_ref;

	/* Used by gbm fbs */
	struct gbm_bo *bo;
	struct gbm_surface *gbm_surface;

	/* Used by dumb fbs */
	void *map;
};

struct iahwc_edid {
	char eisa_id[13];
	char monitor_name[13];
	char pnp_id[5];
	char serial_number[13];
};

/**
 * Pending state holds one or more iahwc_output_state structures, collected from
 * performing repaint. This pending state is transient, and only lives between
 * beginning a repaint group and flushing the results: after flush, each
 * output state will complete and be retired separately.
 */
struct iahwc_pending_state {
	struct iahwc_backend *backend;
};

struct iahwc_output {
	struct weston_output base;
	drmModeConnector *connector;

	uint32_t crtc_id; /* object ID to pass to IAHWC functions */
	int pipe; /* index of CRTC in resource array / bitmasks */
	uint32_t connector_id;
	drmModeCrtcPtr original_crtc;
	struct iahwc_edid edid;

	enum dpms_enum dpms;
	struct backlight *backlight;

	bool state_invalid;

	int vblank_pending;
	int page_flip_pending;
	int destroy_pending;
	int disable_pending;

  int64_t primary_layer_id;
  int64_t cursor_layer_id;
  struct gbm_bo* bo;

  struct gbm_bo *gbm_cursor_bo[2];
	struct weston_plane cursor_plane;
	struct weston_view *cursor_view;
	int current_cursor;

	struct gbm_surface *gbm_surface;
	uint32_t gbm_format;

	/* Plane for a fullscreen direct scanout view */
	struct weston_plane scanout_plane;

	/* The last framebuffer submitted to the kernel for this CRTC. */
	struct iahwc_fb *fb_current;
	/* The previously-submitted framebuffer, where the hardware has not
	 * yet acknowledged display of fb_current. */
	struct iahwc_fb *fb_last;
	/* Framebuffer we are going to submit to the kernel when the current
	 * repaint is flushed. */
	struct iahwc_fb *fb_pending;

	struct iahwc_fb *dumb[2];
	pixman_image_t *image[2];
	int current_image;
	pixman_region32_t previous_damage;

	struct vaapi_recorder *recorder;
	struct wl_listener recorder_frame_listener;

	struct wl_event_source *pageflip_timer;
  int frame_commited;
};

static struct gl_renderer_interface *gl_renderer;

static const char default_seat[] = "seat0";

static inline struct iahwc_output *
to_iahwc_output(struct weston_output *base)
{
	return container_of(base, struct iahwc_output, base);
}

static inline struct iahwc_backend *
to_iahwc_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct iahwc_backend, base);
}

static void
iahwc_fb_destroy(struct iahwc_fb *fb)
{
	if (fb->fb_id != 0)

		drmModeRmFB(fb->fd, fb->fb_id);
	weston_buffer_reference(&fb->buffer_ref, NULL);
	free(fb);
}

static void
iahwc_fb_destroy_dumb(struct iahwc_fb *fb)
{
	struct drm_mode_destroy_dumb destroy_arg;

	assert(fb->type == BUFFER_PIXMAN_DUMB);

	if (fb->map && fb->size > 0)
		munmap(fb->map, fb->size);

	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = fb->handle;
	drmIoctl(fb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);

	iahwc_fb_destroy(fb);
}

static void
iahwc_fb_destroy_gbm(struct gbm_bo *bo, void *data)
{
	struct iahwc_fb *fb = data;

	assert(fb->type == BUFFER_GBM_SURFACE || fb->type == BUFFER_CLIENT ||
	       fb->type == BUFFER_CURSOR);
	iahwc_fb_destroy(fb);
}

static struct iahwc_fb *
iahwc_fb_create_dumb(struct iahwc_backend *b, int width, int height,
		   uint32_t format)
{
	struct iahwc_fb *fb;
	int ret;

	struct drm_mode_create_dumb create_arg;
	struct drm_mode_destroy_dumb destroy_arg;
	struct drm_mode_map_dumb map_arg;

	fb = zalloc(sizeof *fb);
	if (!fb)
		return NULL;

	fb->refcnt = 1;

	fb->format = pixel_format_get_info(format);
	if (!fb->format) {
		weston_log("failed to look up format 0x%lx\n",
			   (unsigned long) format);
		goto err_fb;
	}

	if (!fb->format->depth || !fb->format->bpp) {
		weston_log("format 0x%lx is not compatible with dumb buffers\n",
			   (unsigned long) format);
		goto err_fb;
	}

	memset(&create_arg, 0, sizeof create_arg);
	create_arg.bpp = fb->format->bpp;
	create_arg.width = width;
	create_arg.height = height;

	ret = drmIoctl(b->iahwc.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
	if (ret)
		goto err_fb;

	fb->type = BUFFER_PIXMAN_DUMB;
	fb->handle = create_arg.handle;
	fb->stride = create_arg.pitch;
	fb->size = create_arg.size;
	fb->width = width;
	fb->height = height;
	fb->fd = b->iahwc.fd;

	ret = -1;

	if (!b->no_addfb2) {
		uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

		handles[0] = fb->handle;
		pitches[0] = fb->stride;
		offsets[0] = 0;

		ret = drmModeAddFB2(b->iahwc.fd, width, height,
				    fb->format->format,
				    handles, pitches, offsets,
				    &fb->fb_id, 0);
		if (ret) {
			weston_log("addfb2 failed: %m\n");
			b->no_addfb2 = 1;
		}
	}

	if (ret) {
		ret = drmModeAddFB(b->iahwc.fd, width, height,
				   fb->format->depth, fb->format->bpp,
				   fb->stride, fb->handle, &fb->fb_id);
	}

	if (ret)
		goto err_bo;

	memset(&map_arg, 0, sizeof map_arg);
	map_arg.handle = fb->handle;
	ret = drmIoctl(fb->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);
	if (ret)
		goto err_add_fb;

	fb->map = mmap(NULL, fb->size, PROT_WRITE,
		       MAP_SHARED, b->iahwc.fd, map_arg.offset);
	if (fb->map == MAP_FAILED)
		goto err_add_fb;

	return fb;

err_add_fb:
	drmModeRmFB(b->iahwc.fd, fb->fb_id);
err_bo:
	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = create_arg.handle;
	drmIoctl(b->iahwc.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);
err_fb:
	free(fb);
	return NULL;
}

static struct iahwc_fb *
iahwc_fb_ref(struct iahwc_fb *fb)
{
	fb->refcnt++;
	return fb;
}

static struct iahwc_fb *
iahwc_fb_get_from_bo(struct gbm_bo *bo, struct iahwc_backend *backend,
		   uint32_t format, enum iahwc_fb_type type)
{
	struct iahwc_fb *fb = gbm_bo_get_user_data(bo);

	if (fb) {
		assert(fb->type == type);
		return iahwc_fb_ref(fb);
	}

	fb = zalloc(sizeof *fb);
	if (fb == NULL)
		return NULL;

	fb->type = type;
	fb->refcnt = 1;
	fb->bo = bo;

	fb->width = gbm_bo_get_width(bo);
	fb->height = gbm_bo_get_height(bo);
	fb->stride = gbm_bo_get_stride(bo);
	fb->handle = gbm_bo_get_handle(bo).u32;
	fb->format = pixel_format_get_info(format);
	fb->size = fb->stride * fb->height;
	fb->fd = backend->iahwc.fd;

	if (!fb->format) {
		weston_log("couldn't look up format 0x%lx\n",
			   (unsigned long) format);
		goto err_free;
	}

	gbm_bo_set_user_data(bo, fb, iahwc_fb_destroy_gbm);

	return fb;

err_free:
	free(fb);
	return NULL;
}

static void
iahwc_fb_unref(struct iahwc_fb *fb)
{
	if (!fb)
		return;

	assert(fb->refcnt > 0);
	if (--fb->refcnt > 0)
		return;

	switch (fb->type) {
	case BUFFER_PIXMAN_DUMB:
		iahwc_fb_destroy_dumb(fb);
		break;
	case BUFFER_CURSOR:
	case BUFFER_CLIENT:
		gbm_bo_destroy(fb->bo);
		break;
	case BUFFER_GBM_SURFACE:
		gbm_surface_release_buffer(fb->gbm_surface, fb->bo);
		break;
	default:
		assert(NULL);
		break;
	}
}

/**
 * Allocate a new iahwc_pending_state
 *
 * Allocate a new, empty, 'pending state' structure to be used across a
 * repaint cycle or similar.
 *
 * @param backend IAHWC backend
 * @returns Newly-allocated pending state structure
 */
static struct iahwc_pending_state *
iahwc_pending_state_alloc(struct iahwc_backend *backend)
{
	struct iahwc_pending_state *ret;

	ret = calloc(1, sizeof(*ret));
	if (!ret)
		return NULL;

	ret->backend = backend;

	return ret;
}

/**
 * Free a iahwc_pending_state structure
 *
 * Frees a pending_state structure.
 *
 * @param pending_state Pending state structure to free
 */
static void
iahwc_pending_state_free(struct iahwc_pending_state *pending_state)
{
	if (!pending_state)
		return;

	free(pending_state);
}

static struct iahwc_fb *
iahwc_output_render_gl(struct iahwc_output *output, pixman_region32_t *damage)
{
	struct iahwc_backend *b = to_iahwc_backend(output->base.compositor);
	struct gbm_bo *bo;
	struct iahwc_fb *ret;

	output->base.compositor->renderer->repaint_output(&output->base,
							  damage);

	bo = gbm_surface_lock_front_buffer(output->gbm_surface);
	if (!bo) {
		weston_log("failed to lock front buffer: %m\n");
		return NULL;
	}

  weston_log("hkps here: %s %d\n", __func__, __LINE__);

  b->iahwc_layer_set_bo(b->iahwc_device, 0, output->primary_layer_id, bo);

  // XXX/TODO: need to get the acquire fence and pass it instead of -1
  b->iahwc_layer_set_acquire_fence(b->iahwc_device, 0, output->primary_layer_id, -1);

	ret = iahwc_fb_get_from_bo(bo, b, output->gbm_format, BUFFER_GBM_SURFACE);

	if (!ret) {
		weston_log("failed to get iahwc_fb for bo\n");
		gbm_surface_release_buffer(output->gbm_surface, bo);
		return NULL;
	}

  if(output->bo)
		gbm_surface_release_buffer(output->gbm_surface, output->bo);

  output->bo = bo;

	ret->gbm_surface = output->gbm_surface;

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
	return ret;
}

static void
iahwc_output_render(struct iahwc_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *c = output->base.compositor;
	struct iahwc_fb *fb;

	/* If we already have a client buffer promoted to scanout, then we don't
	 * want to render. */
	if (output->fb_pending)
		return;

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
  fb = iahwc_output_render_gl(output, damage);

	if (!fb)
		return;
	output->fb_pending = fb;

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
	pixman_region32_subtract(&c->primary_plane.damage,
				 &c->primary_plane.damage, damage);
}

static void
iahwc_output_set_gamma(struct weston_output *output_base,
		     uint16_t size, uint16_t *r, uint16_t *g, uint16_t *b)
{
	int rc;
	struct iahwc_output *output = to_iahwc_output(output_base);
	struct iahwc_backend *backend =
		to_iahwc_backend(output->base.compositor);

  float rs = *r, gs = *g, bs = *b;
  rc = backend->iahwc_set_display_gamma(backend->iahwc_device, 0,
                                        rs, gs, bs);
	if (rc)
		weston_log("set gamma failed: %m\n");
}

static int
iahwc_output_repaint(struct weston_output *output_base,
		   pixman_region32_t *damage,
		   void *repaint_data)
{
	struct iahwc_output *output = to_iahwc_output(output_base);
	struct iahwc_backend *backend =
		to_iahwc_backend(output->base.compositor);
  int release_fence, ret;

	if (output->disable_pending || output->destroy_pending)
		return -1;

	/* assert(!output->fb_last); */

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
	/* If disable_planes is set then assign_planes() wasn't
	 * called for this render, so we could still have a stale
	 * cursor plane set up.
	 */
	if (output->base.disable_planes) {
		output->cursor_view = NULL;
		output->cursor_plane.x = INT32_MIN;
		output->cursor_plane.y = INT32_MIN;
	}

	iahwc_output_render(output, damage);
	if (!output->fb_pending)
		return -1;

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
  backend->iahwc_present_display(backend->iahwc_device, 0, &release_fence);
  output->frame_commited = 1;

  weston_log("release fence is %d\n", release_fence);
  if (release_fence > 0) {
    ret = sync_wait(release_fence, -1);
    if (ret < 0)
      weston_log("failed to wait on fence %d: %s\n", release_fence, strerror(errno));
  }

  close(release_fence);

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
	output->fb_last = output->fb_current;
	output->fb_current = output->fb_pending;
	output->fb_pending = NULL;

  uint32_t refresh = output_base->current_mode->refresh;
  weston_log("hkps refresh rate is %d\n", refresh);

	return 0;

	/* if (output->fb_pending) { */
	/* 	iahwc_fb_unref(output->fb_pending); */
	/* 	output->fb_pending = NULL; */
	/* } */

	/* return -1; */
}

static void
iahwc_output_start_repaint_loop(struct weston_output *output_base)
{
	struct iahwc_output *output = to_iahwc_output(output_base);
  struct iahwc_backend *b = to_iahwc_backend(output_base->compositor);

	uint32_t fb_id;

  weston_log("hkps here: %s %d\n", __func__, __LINE__);

	if (output->disable_pending || output->destroy_pending)
		return;

	if (!output->fb_current) {
		/* We can't page flip if there's no mode set */
		goto finish_frame;
	}

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
	/* Immediate query didn't provide valid timestamp.
	 * Use pageflip fallback.
	 */
	fb_id = output->fb_current->fb_id;

	output->fb_last = iahwc_fb_ref(output->fb_current);

  weston_log("hkps here: %s %d\n", __func__, __LINE__);
finish_frame:
	/* if we cannot page-flip, immediately finish frame */
	/* weston_compositor_read_presentation_clock(output->base.compositor, &ts); */
	/* weston_output_finish_frame(&output->base, &ts, 0); */
	weston_output_finish_frame(output_base, NULL,
				   WP_PRESENTATION_FEEDBACK_INVALID);

	return;
}

static void
iahwc_output_destroy(struct weston_output *base);

/**
 * Begin a new repaint cycle
 *
 * Called by the core compositor at the beginning of a repaint cycle.
 */
static void *
iahwc_repaint_begin(struct weston_compositor *compositor)
{
	struct iahwc_backend *b = to_iahwc_backend(compositor);
	struct iahwc_pending_state *ret;

	ret = iahwc_pending_state_alloc(b);
	b->repaint_data = ret;

	return ret;
}

/**
 * Flush a repaint set
 *
 * Called by the core compositor when a repaint cycle has been completed
 * and should be flushed.
 */
static void
iahwc_repaint_flush(struct weston_compositor *compositor, void *repaint_data)
{
	struct iahwc_backend *b = to_iahwc_backend(compositor);
	struct iahwc_pending_state *pending_state = repaint_data;

	iahwc_pending_state_free(pending_state);
	b->repaint_data = NULL;
}

/**
 * Cancel a repaint set
 *
 * Called by the core compositor when a repaint has finished, so the data
 * held across the repaint cycle should be discarded.
 */
static void
iahwc_repaint_cancel(struct weston_compositor *compositor, void *repaint_data)
{
	struct iahwc_backend *b = to_iahwc_backend(compositor);
	struct iahwc_pending_state *pending_state = repaint_data;

	iahwc_pending_state_free(pending_state);
	b->repaint_data = NULL;
}

/**
 * Find the closest-matching mode for a given target
 *
 * Given a target mode, find the most suitable mode amongst the output's
 * current mode list to use, preferring the current mode if possible, to
 * avoid an expensive mode switch.
 *
 * @param output IAHWC output
 * @param target_mode Mode to attempt to match
 * @returns Pointer to a mode from the output's mode list
 */
static struct iahwc_mode *
choose_mode (struct iahwc_output *output, struct weston_mode *target_mode)
{
	struct iahwc_mode *tmp_mode = NULL, *mode;

	if (output->base.current_mode->width == target_mode->width &&
	    output->base.current_mode->height == target_mode->height &&
	    (output->base.current_mode->refresh == target_mode->refresh ||
	     target_mode->refresh == 0))
		return (struct iahwc_mode *)output->base.current_mode;

	wl_list_for_each(mode, &output->base.mode_list, base.link) {
		if (mode->base.width == target_mode->width &&
		    mode->base.height == target_mode->height) {
			if (mode->base.refresh == target_mode->refresh ||
			    target_mode->refresh == 0) {
				return mode;
			} else if (!tmp_mode)
				tmp_mode = mode;
		}
	}

	return tmp_mode;
}

static int
iahwc_output_init_egl(struct iahwc_output *output, struct iahwc_backend *b);
static void
iahwc_output_fini_egl(struct iahwc_output *output);
static int
iahwc_output_init_pixman(struct iahwc_output *output, struct iahwc_backend *b);
static void
iahwc_output_fini_pixman(struct iahwc_output *output);

static int
iahwc_output_switch_mode(struct weston_output *output_base, struct weston_mode *mode)
{
	struct iahwc_output *output;
	struct iahwc_mode *iahwc_mode;
	struct iahwc_backend *b;
  weston_log("hkps here %s %d\n", __func__, __LINE__);

	if (output_base == NULL) {
		weston_log("output is NULL.\n");
		return -1;
	}

	if (mode == NULL) {
		weston_log("mode is NULL.\n");
		return -1;
	}

	b = to_iahwc_backend(output_base->compositor);
	output = to_iahwc_output(output_base);
	iahwc_mode  = choose_mode (output, mode);

	if (!iahwc_mode) {
		weston_log("%s, invalid resolution:%dx%d\n", __func__, mode->width, mode->height);
		return -1;
	}

	if (&iahwc_mode->base == output->base.current_mode)
		return 0;

  b->iahwc_set_display_config(b->iahwc_device, 0, iahwc_mode->config_id);

	output->base.current_mode->flags = 0;

	output->base.current_mode = &iahwc_mode->base;
	output->base.current_mode->flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

	/* XXX: This drops our current buffer too early, before we've started
	 *      displaying it. Ideally this should be much more atomic and
	 *      integrated with a full repaint cycle, rather than doing a
	 *      sledgehammer modeswitch first, and only later showing new
	 *      content.
	 */
	iahwc_fb_unref(output->fb_current);
	assert(!output->fb_last);
	assert(!output->fb_pending);
	output->fb_last = output->fb_current = NULL;

	if (b->use_pixman) {
		iahwc_output_fini_pixman(output);
		if (iahwc_output_init_pixman(output, b) < 0) {
			weston_log("failed to init output pixman state with "
				   "new mode\n");
			return -1;
		}
	} else {
		iahwc_output_fini_egl(output);
		if (iahwc_output_init_egl(output, b) < 0) {
			weston_log("failed to init output egl state with "
				   "new mode");
			return -1;
		}
	}

	return 0;
}

static struct gbm_device *
create_gbm_device(int fd)
{
	struct gbm_device *gbm;

	gl_renderer = weston_load_module("gl-renderer.so",
					 "gl_renderer_interface");
	if (!gl_renderer)
		return NULL;

	/* GBM will load a dri driver, but even though they need symbols from
	 * libglapi, in some version of Mesa they are not linked to it. Since
	 * only the gl-renderer module links to it, the call above won't make
	 * these symbols globally available, and loading the DRI driver fails.
	 * Workaround this by dlopen()'ing libglapi with RTLD_GLOBAL. */
	dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);

	gbm = gbm_create_device(fd);

	return gbm;
}

/* When initializing EGL, if the preferred buffer format isn't available
 * we may be able to substitute an ARGB format for an XRGB one.
 *
 * This returns 0 if substitution isn't possible, but 0 might be a
 * legitimate format for other EGL platforms, so the caller is
 * responsible for checking for 0 before calling gl_renderer->create().
 *
 * This works around https://bugs.freedesktop.org/show_bug.cgi?id=89689
 * but it's entirely possible we'll see this again on other implementations.
 */
static int
fallback_format_for(uint32_t format)
{
	switch (format) {
	case GBM_FORMAT_XRGB8888:
		return GBM_FORMAT_ARGB8888;
	case GBM_FORMAT_XRGB2101010:
		return GBM_FORMAT_ARGB2101010;
	default:
		return 0;
	}
}

static int
iahwc_backend_create_gl_renderer(struct iahwc_backend *b)
{
	EGLint format[3] = {
		b->gbm_format,
		fallback_format_for(b->gbm_format),
		0,
	};
	int n_formats = 2;

	if (format[1])
		n_formats = 3;
	if (gl_renderer->display_create(b->compositor,
					EGL_PLATFORM_GBM_KHR,
					(void *)b->gbm,
					NULL,
					gl_renderer->opaque_attribs,
					format,
					n_formats) < 0) {
		return -1;
	}

	return 0;
}

static int
init_egl(struct iahwc_backend *b)
{
	b->gbm = create_gbm_device(b->iahwc.fd);

	if (!b->gbm)
		return -1;

	if (iahwc_backend_create_gl_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		return -1;
	}

	return 0;
}

static struct weston_plane *
iahwc_output_prepare_cursor_view(struct iahwc_output *output,
			       struct weston_view *ev)
{
  weston_log("hkps in %s\n", __FUNCTION__);
	struct iahwc_backend *b = to_iahwc_backend(output->base.compositor);
	struct weston_buffer_viewport *viewport = &ev->surface->buffer_viewport;
	struct wl_shm_buffer *shmbuf;
	float x, y;
	struct weston_buffer *buffer = ev->surface->buffer_ref.buffer;
	uint32_t buf[b->cursor_width * b->cursor_height];
	int32_t stride;
	uint8_t *s;
	int i;
  struct gbm_bo* bo;

	if (output->cursor_view)
		return NULL;

	/* Don't import buffers which span multiple outputs. */
	if (ev->output_mask != (1u << output->base.id))
		return NULL;

	/* We use GBM to import SHM buffers. */
	if (b->gbm == NULL)
		return NULL;

	if (ev->surface->buffer_ref.buffer == NULL)
		return NULL;
	shmbuf = wl_shm_buffer_get(ev->surface->buffer_ref.buffer->resource);
	if (!shmbuf)
		return NULL;
	if (wl_shm_buffer_get_format(shmbuf) != WL_SHM_FORMAT_ARGB8888)
		return NULL;

	if (output->base.transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return NULL;
	if (ev->transform.enabled &&
	    (ev->transform.matrix.type > WESTON_MATRIX_TRANSFORM_TRANSLATE))
		return NULL;
	if (viewport->buffer.scale != output->base.current_scale)
		return NULL;
	if (ev->geometry.scissor_enabled)
		return NULL;

	if (ev->surface->width > b->cursor_width ||
	    ev->surface->height > b->cursor_height)
		return NULL;

	output->cursor_view = ev;
	weston_view_to_global_float(ev, 0, 0, &x, &y);
	output->cursor_plane.x = x;
	output->cursor_plane.y = y;

  if (output->cursor_layer_id == -1) {
    b->iahwc_create_layer(b->iahwc_device, 0, &output->cursor_layer_id);
    b->iahwc_layer_set_usage(b->iahwc_device,
                             0,
                             output->cursor_layer_id,
                             IAHWC_LAYER_USAGE_CURSOR);
  }

  memset(buf, 0, sizeof buf);
	stride = wl_shm_buffer_get_stride(buffer->shm_buffer);
	s = wl_shm_buffer_get_data(buffer->shm_buffer);

	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (i = 0; i < ev->surface->height; i++)
		memcpy(buf + i * b->cursor_width,
		       s + i * stride,
		       ev->surface->width * 4);
	wl_shm_buffer_end_access(buffer->shm_buffer);

  output->current_cursor ^= 1;
  bo = output->gbm_cursor_bo[output->current_cursor];
	if (gbm_bo_write(bo, buf, sizeof buf) < 0)
		weston_log("failed update cursor: %m\n");

  weston_log("hkps setting bo %d for cursor layer\n");
  b->iahwc_layer_set_bo(b->iahwc_device, 0, output->cursor_layer_id, bo);

  iahwc_rect_t source_crop = {0,
                              0,
                              ev->surface->width,
                              ev->surface->height};

  iahwc_rect_t display_frame = {x,
                                y,
                                ev->surface->width,
                                ev->surface->height};

  iahwc_region_t damage_region;
  damage_region.numRects = 1;
  damage_region.rects = &source_crop;

  b->iahwc_layer_set_source_crop(b->iahwc_device, 0, output->cursor_layer_id, source_crop);
  b->iahwc_layer_set_display_frame(b->iahwc_device, 0, output->cursor_layer_id, display_frame);
  b->iahwc_layer_set_surface_damage(b->iahwc_device, 0, output->cursor_layer_id, damage_region);

	return &output->cursor_plane;
}

/**
 * Add a mode to output's mode list
 *
 * Copy the supplied IAHWC mode into a Weston mode structure, and add it to the
 * output's mode list.
 *
 * @param output IAHWC output to add mode to
 * @param info IAHWC mode structure to add
 * @returns Newly-allocated Weston/IAHWC mode structure
 */
static struct iahwc_mode *
iahwc_output_add_mode(struct iahwc_backend *b, struct iahwc_output *output, int config_id)
{
	struct iahwc_mode *mode;

	mode = malloc(sizeof *mode);
	if (mode == NULL)
		return NULL;

	mode->base.flags = 0;
  b->iahwc_get_display_info(b->iahwc_device, 0, config_id, IAHWC_CONFIG_WIDTH, &mode->base.width);
  b->iahwc_get_display_info(b->iahwc_device, 0, config_id, IAHWC_CONFIG_HEIGHT, &mode->base.height);
  b->iahwc_get_display_info(b->iahwc_device, 0, config_id, IAHWC_CONFIG_REFRESHRATE, &mode->base.refresh);

  mode->config_id = config_id;

  // XXX/TODO: Get current mode and set preffered mode flag.
	/* if (info->type & IAHWC_MODE_TYPE_PREFERRED) */
	/* 	mode->base.flags |= WL_OUTPUT_MODE_PREFERRED; */

	wl_list_insert(output->base.mode_list.prev, &mode->base.link);

	return mode;
}

static int
iahwc_subpixel_to_wayland(int iahwc_value)
{
	switch (iahwc_value) {
	default:
	case DRM_MODE_SUBPIXEL_UNKNOWN:
		return WL_OUTPUT_SUBPIXEL_UNKNOWN;
	case DRM_MODE_SUBPIXEL_NONE:
		return WL_OUTPUT_SUBPIXEL_NONE;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;
	case DRM_MODE_SUBPIXEL_VERTICAL_RGB:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;
	case DRM_MODE_SUBPIXEL_VERTICAL_BGR:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;
	}
}

/* returns a value between 0-255 range, where higher is brighter */
static uint32_t
iahwc_get_backlight(struct iahwc_output *output)
{
	long brightness, max_brightness, norm;

	brightness = backlight_get_brightness(output->backlight);
	max_brightness = backlight_get_max_brightness(output->backlight);

	/* convert it on a scale of 0 to 255 */
	norm = (brightness * 255)/(max_brightness);

	return (uint32_t) norm;
}

/* values accepted are between 0-255 range */
static void
iahwc_set_backlight(struct weston_output *output_base, uint32_t value)
{
	struct iahwc_output *output = to_iahwc_output(output_base);
	long max_brightness, new_brightness;

	if (!output->backlight)
		return;

	if (value > 255)
		return;

	max_brightness = backlight_get_max_brightness(output->backlight);

	/* get denormalized value */
	new_brightness = (value * max_brightness) / 255;

	backlight_set_brightness(output->backlight, new_brightness);
}

static void iahwc_output_fini_cursor_egl(struct iahwc_output *output)
{
	unsigned int i;

  for (i = 0; i < ARRAY_LENGTH(output->gbm_cursor_bo); i++) {
    gbm_bo_destroy(output->gbm_cursor_bo[i]);
    output->gbm_cursor_bo[i] = NULL;
  }
}

static int
iahwc_output_init_cursor_egl(struct iahwc_output *output, struct iahwc_backend *b)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(output->gbm_cursor_bo); i++) {
		struct gbm_bo *bo;

		bo = gbm_bo_create(b->gbm, b->cursor_width, b->cursor_height,
                       GBM_FORMAT_ARGB8888,
                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
				   /* GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE); */
		if (!bo) {
      weston_log("hkps unable to create bo for cursor %s\n", strerror(errno));
			goto err;
    }

    weston_log("hkps bo for cursor %d is %p\n", i, bo);
		output->gbm_cursor_bo[i] = bo;
		if (!output->gbm_cursor_bo[i]) {
			gbm_bo_destroy(bo);
			goto err;
		}
	}

	return 0;

err:
	weston_log("cursor buffers unavailable, using gl cursors\n");
	b->cursors_are_broken = 1;
	iahwc_output_fini_cursor_egl(output);
	return -1;
}

/* Init output state that depends on gl or gbm */
static int
iahwc_output_init_egl(struct iahwc_output *output, struct iahwc_backend *b)
{
	EGLint format[2] = {
		output->gbm_format,
		fallback_format_for(output->gbm_format),
	};
	int n_formats = 1;

	output->gbm_surface = gbm_surface_create(b->gbm,
					     output->base.current_mode->width,
					     output->base.current_mode->height,
					     format[0],
					     GBM_BO_USE_SCANOUT |
					     GBM_BO_USE_RENDERING);
	if (!output->gbm_surface) {
		weston_log("failed to create gbm surface\n");
		return -1;
	}

	if (format[1])
		n_formats = 2;
	if (gl_renderer->output_window_create(&output->base,
					      (EGLNativeWindowType)output->gbm_surface,
					      output->gbm_surface,
					      gl_renderer->opaque_attribs,
					      format,
					      n_formats) < 0) {
		weston_log("failed to create gl renderer output state\n");
		gbm_surface_destroy(output->gbm_surface);
		return -1;
	}

	iahwc_output_init_cursor_egl(output, b);

	return 0;
}

static void
iahwc_output_fini_egl(struct iahwc_output *output)
{
	gl_renderer->output_destroy(&output->base);
	gbm_surface_destroy(output->gbm_surface);
}

static int
iahwc_output_init_pixman(struct iahwc_output *output, struct iahwc_backend *b)
{
	int w = output->base.current_mode->width;
	int h = output->base.current_mode->height;
	uint32_t format = output->gbm_format;
	uint32_t pixman_format;
	unsigned int i;

	switch (format) {
		case GBM_FORMAT_XRGB8888:
			pixman_format = PIXMAN_x8r8g8b8;
			break;
		case GBM_FORMAT_RGB565:
			pixman_format = PIXMAN_r5g6b5;
			break;
		default:
			weston_log("Unsupported pixman format 0x%x\n", format);
			return -1;
	}

	/* FIXME error checking */
	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		output->dumb[i] = iahwc_fb_create_dumb(b, w, h, format);
		if (!output->dumb[i])
			goto err;

		output->image[i] =
			pixman_image_create_bits(pixman_format, w, h,
						 output->dumb[i]->map,
						 output->dumb[i]->stride);
		if (!output->image[i])
			goto err;
	}

	if (pixman_renderer_output_create(&output->base) < 0)
		goto err;

	pixman_region32_init_rect(&output->previous_damage,
				  output->base.x, output->base.y, output->base.width, output->base.height);

	return 0;

err:
	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		if (output->dumb[i])
			iahwc_fb_unref(output->dumb[i]);
		if (output->image[i])
			pixman_image_unref(output->image[i]);

		output->dumb[i] = NULL;
		output->image[i] = NULL;
	}

	return -1;
}

static void
iahwc_assign_planes(struct weston_output *output_base, void *repaint_data)
{
  weston_log("hkps in %s\n", __FUNCTION__);

	struct iahwc_backend *b = to_iahwc_backend(output_base->compositor);
	struct iahwc_output *output = to_iahwc_output(output_base);
	struct weston_view *ev, *next;
	pixman_region32_t overlap, surface_overlap;
	struct weston_plane *primary, *next_plane;

	pixman_region32_init(&overlap);
	primary = &output_base->compositor->primary_plane;

	output->cursor_view = NULL;
	output->cursor_plane.x = INT32_MIN;
	output->cursor_plane.y = INT32_MIN;

  b->iahwc_display_clear_all_layers(b->iahwc_device, 0);
  output->primary_layer_id = -1;
  output->cursor_layer_id = -1;

	wl_list_for_each_safe(ev, next, &output_base->compositor->view_list, link) {
		struct weston_surface *es = ev->surface;

		/* Test whether this buffer can ever go into a plane:
		 * non-shm, or small enough to be a cursor.
		 *
		 * Also, keep a reference when using the pixman renderer.
		 * That makes it possible to do a seamless switch to the GL
		 * renderer and since the pixman renderer keeps a reference
		 * to the buffer anyway, there is no side effects.
		 */
		if (b->use_pixman ||
		    (es->buffer_ref.buffer &&
		    (!wl_shm_buffer_get(es->buffer_ref.buffer->resource) ||
		     (ev->surface->width <= b->cursor_width &&
		      ev->surface->height <= b->cursor_height))))
			es->keep_buffer = true;
		else
			es->keep_buffer = false;

		pixman_region32_init(&surface_overlap);
		pixman_region32_intersect(&surface_overlap, &overlap,
					  &ev->transform.boundingbox);

		next_plane = NULL;
		if (pixman_region32_not_empty(&surface_overlap))
			next_plane = primary;
		if (next_plane == NULL)
			next_plane = iahwc_output_prepare_cursor_view(output, ev);
		if (next_plane == NULL)
			next_plane = primary;

		weston_view_move_to_plane(ev, next_plane);

		if (next_plane == primary) {
      if (output->primary_layer_id == -1) {
        b->iahwc_create_layer(b->iahwc_device, 0, &output->primary_layer_id);

        iahwc_rect_t viewport = {0,
                                 0,
                                 output_base->current_mode->width,
                                 output_base->current_mode->height};

        iahwc_region_t damage_region;
        damage_region.numRects = 1;
        damage_region.rects = &viewport;

        b->iahwc_layer_set_source_crop(b->iahwc_device, 0, output->primary_layer_id, viewport);
        b->iahwc_layer_set_display_frame(b->iahwc_device, 0, output->primary_layer_id, viewport);
        b->iahwc_layer_set_surface_damage(b->iahwc_device, 0, output->primary_layer_id, damage_region);

      }
			pixman_region32_union(&overlap, &overlap,
                            &ev->transform.boundingbox);
    }

		if (next_plane == primary ||
		    next_plane == &output->cursor_plane) {
			/* cursor plane involves a copy */
			ev->psf_flags = 0;
		} else {
			/* All other planes are a direct scanout of a
			 * single client buffer.
			 */
			ev->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
		}

		pixman_region32_fini(&surface_overlap);
	}
	pixman_region32_fini(&overlap);

}

static void
iahwc_output_fini_pixman(struct iahwc_output *output)
{
	unsigned int i;

	pixman_renderer_output_destroy(&output->base);
	pixman_region32_fini(&output->previous_damage);

	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		pixman_image_unref(output->image[i]);
		iahwc_fb_unref(output->dumb[i]);
		output->dumb[i] = NULL;
		output->image[i] = NULL;
	}
}

static void
setup_output_seat_constraint(struct iahwc_backend *b,
			     struct weston_output *output,
			     const char *s)
{
	if (strcmp(s, "") != 0) {
		struct weston_pointer *pointer;
		struct udev_seat *seat;

		seat = udev_seat_get_named(&b->input, s);
		if (!seat)
			return;

		seat->base.output = output;

		pointer = weston_seat_get_pointer(&seat->base);
		if (pointer)
			weston_pointer_clamp(pointer,
					     &pointer->x,
					     &pointer->y);
	}
}

static int
parse_gbm_format(const char *s, uint32_t default_value, uint32_t *gbm_format)
{
	int ret = 0;

	if (s == NULL)
		*gbm_format = default_value;
	else if (strcmp(s, "xrgb8888") == 0)
		*gbm_format = GBM_FORMAT_XRGB8888;
	else if (strcmp(s, "rgb565") == 0)
		*gbm_format = GBM_FORMAT_RGB565;
	else if (strcmp(s, "xrgb2101010") == 0)
		*gbm_format = GBM_FORMAT_XRGB2101010;
	else {
		weston_log("fatal: unrecognized pixel format: %s\n", s);
		ret = -1;
	}

	return ret;
}

/**
 * Choose suitable mode for an output
 *
 * Find the most suitable mode to use for initial setup (or reconfiguration on
 * hotplug etc) for a IAHWC output.
 *
 * @param output IAHWC output to choose mode for
 * @param kind Strategy and preference to use when choosing mode
 * @param width Desired width for this output
 * @param height Desired height for this output
 * @param current_mode Mode currently being displayed on this output
 * @param modeline Manually-entered mode (may be NULL)
 * @returns A mode from the output's mode list, or NULL if none available
 */
static struct iahwc_mode *
iahwc_output_choose_initial_mode(struct iahwc_backend *backend,
			       struct iahwc_output *output,
			       enum weston_iahwc_backend_output_mode mode,
			       const char *modeline)
{
	struct iahwc_mode *iahwc_mode;
  uint32_t active_config;

  backend->iahwc_get_display_config(backend->iahwc_device, 0, &active_config);

  weston_log("hkps Active mode is %d\n", active_config);

	wl_list_for_each_reverse(iahwc_mode, &output->base.mode_list, base.link) {
    if (iahwc_mode->config_id == active_config)
      return iahwc_mode;
	}

	weston_log("no available modes for %s\n", output->base.name);
	return NULL;
}

// XXX/TODO: rewrite this to suit iahwc
static int
iahwc_output_set_mode(struct weston_output *base,
		    enum weston_iahwc_backend_output_mode mode,
		    const char *modeline)
{
	struct iahwc_output *output = to_iahwc_output(base);
	struct iahwc_backend *b = to_iahwc_backend(base->compositor);

	struct iahwc_mode *current;

	current = iahwc_output_choose_initial_mode(b, output, mode, modeline);
	if (!current)
		return -1;

	output->base.current_mode = &current->base;
	output->base.current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	/* Set native_ fields, so weston_output_mode_switch_to_native() works */
	output->base.native_mode = output->base.current_mode;
	output->base.native_scale = output->base.current_scale;

	return 0;
}

static void
iahwc_output_set_gbm_format(struct weston_output *base,
			  const char *gbm_format)
{
	struct iahwc_output *output = to_iahwc_output(base);
	struct iahwc_backend *b = to_iahwc_backend(base->compositor);

	if (parse_gbm_format(gbm_format, b->gbm_format, &output->gbm_format) == -1)
		output->gbm_format = b->gbm_format;
}

static void
iahwc_output_set_seat(struct weston_output *base,
		    const char *seat)
{
	struct iahwc_output *output = to_iahwc_output(base);
	struct iahwc_backend *b = to_iahwc_backend(base->compositor);

	setup_output_seat_constraint(b, &output->base,
				     seat ? seat : "");
}

static int
finish_frame_handler(void *data)
{
	struct iahwc_output *output = data;
	struct timespec ts;

  weston_log("hkps here %s %d\n", __func__, __LINE__);

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
}

static int
iahwc_output_enable(struct weston_output *base)
{
	struct iahwc_output *output = to_iahwc_output(base);
	struct iahwc_backend *b = to_iahwc_backend(base->compositor);
	struct weston_mode *m;
	struct wl_event_loop *loop;

	if (b->use_pixman) {
		if (iahwc_output_init_pixman(output, b) < 0) {
			weston_log("Failed to init output pixman state\n");
			goto err;
		}
	} else if (iahwc_output_init_egl(output, b) < 0) {
		weston_log("Failed to init output gl state\n");
		goto err;
	}

	if (output->backlight) {
		weston_log("Initialized backlight, device %s\n",
			   output->backlight->path);
		output->base.set_backlight = iahwc_set_backlight;
		output->base.backlight_current = iahwc_get_backlight(output);
	} else {
		weston_log("Failed to initialize backlight\n");
	}

  // XXX/TODO: fill them  with the correct functions
	output->base.start_repaint_loop = iahwc_output_start_repaint_loop;
	output->base.repaint = iahwc_output_repaint;
  // XXX/TODO: minimal plane assignment for now
	output->base.assign_planes = iahwc_assign_planes;

  // XXX/TODO: No dpms for now.
	output->base.set_dpms = NULL;
	output->base.switch_mode = iahwc_output_switch_mode;

	output->base.set_gamma = iahwc_output_set_gamma;

	weston_plane_init(&output->cursor_plane, b->compositor,
			  INT32_MIN, INT32_MIN);
	weston_plane_init(&output->scanout_plane, b->compositor, 0, 0);

	weston_compositor_stack_plane(b->compositor, &output->cursor_plane, NULL);
	weston_compositor_stack_plane(b->compositor, &output->scanout_plane,
				      &b->compositor->primary_plane);

  weston_log("hkps %s creating a layer %d\n", __FUNCTION__, __LINE__);

	loop = wl_display_get_event_loop(base->compositor->wl_display);
	output->pageflip_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

  output->frame_commited = 0;

	weston_log("Output %s, (connector %d, crtc %d)\n",
		   output->base.name, output->connector_id, output->crtc_id);
	wl_list_for_each(m, &output->base.mode_list, link)
		weston_log_continue(STAMP_SPACE "mode %dx%d@%.1d\n",
				    m->width, m->height, m->refresh);

	output->state_invalid = true;

	return 0;

err:
	return -1;
}

static void
iahwc_output_deinit(struct weston_output *base)
{
	struct iahwc_output *output = to_iahwc_output(base);
	struct iahwc_backend *b = to_iahwc_backend(base->compositor);

	/* output->fb_last and output->fb_pending must not be set here;
	 * destroy_pending/disable_pending exist to guarantee exactly this. */
	assert(!output->fb_last);
	assert(!output->fb_pending);
	iahwc_fb_unref(output->fb_current);
	output->fb_current = NULL;

	if (b->use_pixman)
		iahwc_output_fini_pixman(output);
	else
		iahwc_output_fini_egl(output);

	weston_plane_release(&output->scanout_plane);
	weston_plane_release(&output->cursor_plane);

}

static void
iahwc_output_destroy(struct weston_output *base)
{
	struct iahwc_output *output = to_iahwc_output(base);
	struct iahwc_mode *iahwc_mode, *next;

	wl_list_for_each_safe(iahwc_mode, next, &output->base.mode_list,
			      base.link) {
		wl_list_remove(&iahwc_mode->base.link);
		free(iahwc_mode);
	}

	weston_output_release(&output->base);

	if (output->backlight)
		backlight_destroy(output->backlight);

	free(output);
}

static int
iahwc_output_disable(struct weston_output *base)
{
	struct iahwc_output *output = to_iahwc_output(base);

	if (output->page_flip_pending) {
		output->disable_pending = 1;
		return -1;
	}

	if (output->base.enabled)
		iahwc_output_deinit(&output->base);

	output->disable_pending = 0;

	weston_log("Disabling output %s\n", output->base.name);

	return 0;
}


static int vsync_callback (iahwc_callback_data_t data, iahwc_display_t display, int64_t timestamp) {

  struct iahwc_output* output = data;
  struct timespec ts;
  ts.tv_nsec = timestamp;
  ts.tv_sec = timestamp/(1000 * 1000 * 1000);
  weston_log("hkps timestamp for display %d is %ld\n", display, timestamp);

  if (output->pageflip_timer && output->frame_commited)
    wl_event_source_timer_update(output->pageflip_timer, 1);

  output->frame_commited = 0;

  return 1;

}

/**
 * Create a Weston output structure
 *
 * Given a IAHWC connector, create a matching iahwc_output structure and add it
 * to Weston's output list. It also takes ownership of the connector, which
 * is released when output is destroyed.
 *
 * @param b Weston backend structure
 * @param resources IAHWC resources for this device
 * @param connector IAHWC connector to use for this new output
 * @param iahwc_device udev device pointer
 * @returns 0 on success, or -1 on failure
 */
// XXX?TODO: Rename this function
static int
create_output_for_connector(struct iahwc_backend *b)
{
	struct iahwc_output *output;
	struct iahwc_mode *iahwc_mode;
	char *name;
	const char *make = "unknown";
	const char *model = "unknown";
	const char *serial_number = "unknown";
	int i, num_displays;
  uint32_t size, num_configs;
  uint32_t* configs;
  int ret;

	output = zalloc(sizeof *output);
	if (output == NULL)
		goto err;

  b->iahwc_get_num_displays(b->iahwc_device, &num_displays);

  if (num_displays < 1) {
    weston_log("Unable to find any connected displays");
    goto err;
  }
	/* output->backlight = backlight_init(iahwc_device, */
	/* 				   connector->connector_type); */

  //XXX/TODO: support more than one display

  b->iahwc_get_display_name(b->iahwc_device, 0, &size, NULL);
  weston_log("Size of name is %d\n", size);
  name = (char*)calloc(size + 1, sizeof(char));
  b->iahwc_get_display_name(b->iahwc_device, 0, &size, name);
  name[size]='\0';

  weston_log("Name of the display is %s\n", name);

	weston_output_init(&output->base, b->compositor, name);
	free(name);

  // XXX/TODO: Fill these with appropriate functions.
	output->base.enable = iahwc_output_enable;
	output->base.destroy = iahwc_output_destroy;
	output->base.disable = iahwc_output_disable;

	output->destroy_pending = 0;
	output->disable_pending = 0;

	output->base.make = (char *)make;
	output->base.model = (char *)model;
	output->base.serial_number = (char *)serial_number;
	output->base.subpixel = iahwc_subpixel_to_wayland(DRM_MODE_SUBPIXEL_UNKNOWN);

  output->base.connection_internal = true;

  output->cursor_layer_id = -1;
  output->primary_layer_id = -1;

  b->iahwc_get_display_configs(b->iahwc_device, 0, &num_configs, NULL);
  configs = (uint32_t*) calloc(num_configs, sizeof(uint32_t));
  b->iahwc_get_display_configs(b->iahwc_device, 0, &num_configs, configs);

  b->iahwc_get_display_info(b->iahwc_device, 0, configs[0], IAHWC_CONFIG_DPIX, &output->base.mm_width);
  b->iahwc_get_display_info(b->iahwc_device, 0, configs[0], IAHWC_CONFIG_DPIY, &output->base.mm_height);

  for (i = 0; i < num_configs; i++) {
    iahwc_mode = iahwc_output_add_mode(b, output, configs[i]);
    if (!iahwc_mode) {
      iahwc_output_destroy(&output->base);
      return -1;
    }
  }

  ret = b->iahwc_register_callback(b->iahwc_device,
                                   IAHWC_CALLBACK_VSYNC,
                                   0,
                                   output,
                                   (iahwc_function_ptr_t)vsync_callback);

  if (ret != IAHWC_ERROR_NONE) {
    weston_log("unable to register callback\n");
  }

	weston_compositor_add_pending_output(&output->base, b->compositor);

	return 0;

err:

	return -1;
}

static int
create_outputs(struct iahwc_backend *b)
{

  create_output_for_connector(b);

	if (wl_list_empty(&b->compositor->output_list) &&
	    wl_list_empty(&b->compositor->pending_output_list))
		weston_log("No currently active connector found.\n");


	return 0;
}

static void
iahwc_restore(struct weston_compositor *ec)
{
	weston_launcher_restore(ec->launcher);
}

static void
iahwc_destroy(struct weston_compositor *ec)
{
	struct iahwc_backend *b = to_iahwc_backend(ec);

	udev_input_destroy(&b->input);

	wl_event_source_remove(b->udev_iahwc_source);
	wl_event_source_remove(b->iahwc_source);

	weston_compositor_shutdown(ec);

	if (b->gbm)
		gbm_device_destroy(b->gbm);

	udev_unref(b->udev);

	weston_launcher_destroy(ec->launcher);

  b->iahwc_device->close(b->iahwc_device);

	free(b);
}

static void
session_notify(struct wl_listener *listener, void *data)
{
	struct weston_compositor *compositor = data;
	struct iahwc_backend *b = to_iahwc_backend(compositor);
	struct iahwc_output *output;

	if (compositor->session_active) {
		weston_log("activating session\n");
		weston_compositor_wake(compositor);
		weston_compositor_damage_all(compositor);

		wl_list_for_each(output, &compositor->output_list, base.link)
			output->state_invalid = true;

		udev_input_enable(&b->input);
	} else {
		weston_log("deactivating session\n");
		udev_input_disable(&b->input);

		weston_compositor_offscreen(compositor);

		/* If we have a repaint scheduled (either from a
		 * pending pageflip or the idle handler), make sure we
		 * cancel that so we don't try to pageflip when we're
		 * vt switched away.  The OFFSCREEN state will prevent
		 * further attempts at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

	}
}

static void
planes_binding(struct weston_keyboard *keyboard, uint32_t time, uint32_t key,
	       void *data)
{
	struct iahwc_backend *b = data;

	switch (key) {
	case KEY_C:
		b->cursors_are_broken ^= 1;
		break;
	case KEY_V:
		b->sprites_are_broken ^= 1;
		break;
	case KEY_O:
		b->sprites_hidden ^= 1;
		break;
	default:
		break;
	}
}

static void
switch_to_gl_renderer(struct iahwc_backend *b)
{
	struct iahwc_output *output;
	bool dmabuf_support_inited;

	if (!b->use_pixman)
		return;

	dmabuf_support_inited = !!b->compositor->renderer->import_dmabuf;

	weston_log("Switching to GL renderer\n");

	b->gbm = create_gbm_device(b->iahwc.fd);
	if (!b->gbm) {
		weston_log("Failed to create gbm device. "
			   "Aborting renderer switch\n");
		return;
	}

	wl_list_for_each(output, &b->compositor->output_list, base.link)
		pixman_renderer_output_destroy(&output->base);

	b->compositor->renderer->destroy(b->compositor);

	if (iahwc_backend_create_gl_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		weston_log("Failed to create GL renderer. Quitting.\n");
		/* FIXME: we need a function to shutdown cleanly */
		assert(0);
	}

	wl_list_for_each(output, &b->compositor->output_list, base.link)
		iahwc_output_init_egl(output, b);

	b->use_pixman = 0;

	if (!dmabuf_support_inited && b->compositor->renderer->import_dmabuf) {
		if (linux_dmabuf_setup(b->compositor) < 0)
			weston_log("Error: initializing dmabuf "
				   "support failed.\n");
	}
}

static void
renderer_switch_binding(struct weston_keyboard *keyboard, uint32_t time,
			uint32_t key, void *data)
{
	struct iahwc_backend *b =
		to_iahwc_backend(keyboard->seat->compositor);

	switch_to_gl_renderer(b);
}

static const struct weston_iahwc_output_api api = {
	iahwc_output_set_mode,
	iahwc_output_set_gbm_format,
	iahwc_output_set_seat,
};

static struct iahwc_backend *
iahwc_backend_create(struct weston_compositor *compositor,
		   struct weston_iahwc_backend_config *config)
{

	struct iahwc_backend *b;
  void *iahwc_dl_handle, *gl_handle;
  iahwc_module_t* iahwc_module;
  iahwc_device_t* iahwc_device;

  const char* device = "/dev/dri/renderD128";
	const char *seat_id = default_seat;

	weston_log("initializing iahwc backend\n");

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor = compositor;
	compositor->backend = &b->base;

  //XXX/TODO: use pixman
  b->use_pixman = 0;

  // Work around when libhwcomposer is not linked to lib{EGL,GLESv2}
  gl_handle = dlopen("libEGL.so", RTLD_NOW|RTLD_GLOBAL);
  if (!gl_handle)
    weston_log("Unable to open libEGL.so: %s\n", dlerror());

  gl_handle = dlopen("libGLESv2.so", RTLD_NOW|RTLD_GLOBAL);
  if (!gl_handle) {
    weston_log("Unable to open libGLESv2.so: %s\n", dlerror());
    weston_log("Unable to open libhwcomposer prerequisites aborting...\n");
    abort();
  }

  // XXX/TODO: Initialize hwc
  iahwc_dl_handle = dlopen("libhwcomposer.so", RTLD_NOW);
  if (!iahwc_dl_handle) {
    weston_log("Unable to open libhwcomposer.so: %s\n", dlerror());
    weston_log("aborting...\n");
    abort();
  }

  iahwc_module = (iahwc_module_t*) dlsym(iahwc_dl_handle, IAHWC_MODULE_STR);
  iahwc_module->open(iahwc_module, &iahwc_device);

  b->iahwc_module = iahwc_module;
  b->iahwc_device = iahwc_device;

  // XXX/TODO: Get all required function poinsters. add them to iahwc_backend
  b->iahwc_get_num_displays = (IAHWC_PFN_GET_NUM_DISPLAYS)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_GET_NUM_DISPLAYS);
  b->iahwc_create_layer = (IAHWC_PFN_CREATE_LAYER)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_CREATE_LAYER);
  b->iahwc_get_display_info = (IAHWC_PFN_DISPLAY_GET_INFO)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_DISPLAY_GET_INFO);
  b->iahwc_get_display_configs = (IAHWC_PFN_DISPLAY_GET_CONFIGS)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_DISPLAY_GET_CONFIGS);
  b->iahwc_get_display_name = (IAHWC_PFN_DISPLAY_GET_NAME)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_DISPLAY_GET_NAME);
  b->iahwc_set_display_gamma = (IAHWC_PFN_DISPLAY_SET_GAMMA)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_DISPLAY_SET_GAMMA);
  b->iahwc_set_display_config = (IAHWC_PFN_DISPLAY_SET_CONFIG)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_DISPLAY_SET_CONFIG);
  b->iahwc_get_display_config = (IAHWC_PFN_DISPLAY_GET_CONFIG)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_DISPLAY_GET_CONFIG);
  b->iahwc_display_clear_all_layers = (IAHWC_PFN_DISPLAY_CLEAR_ALL_LAYERS)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_DISPLAY_CLEAR_ALL_LAYERS);
  b->iahwc_present_display = (IAHWC_PFN_PRESENT_DISPLAY)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_PRESENT_DISPLAY);
  b->iahwc_layer_set_bo = (IAHWC_PFN_LAYER_SET_BO)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_LAYER_SET_BO);
  b->iahwc_layer_set_acquire_fence = (IAHWC_PFN_LAYER_SET_ACQUIRE_FENCE)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_LAYER_SET_ACQUIRE_FENCE);
  b->iahwc_layer_set_source_crop = (IAHWC_PFN_LAYER_SET_SOURCE_CROP)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_LAYER_SET_SOURCE_CROP);
  b->iahwc_layer_set_display_frame = (IAHWC_PFN_LAYER_SET_DISPLAY_FRAME)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_LAYER_SET_DISPLAY_FRAME);
  b->iahwc_layer_set_surface_damage = (IAHWC_PFN_LAYER_SET_SURFACE_DAMAGE)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_LAYER_SET_SURFACE_DAMAGE);
  b->iahwc_layer_set_usage = (IAHWC_PFN_LAYER_SET_USAGE)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_LAYER_SET_USAGE);
  b->iahwc_register_callback = (IAHWC_PFN_REGISTER_CALLBACK)
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_REGISTER_CALLBACK);

	if (parse_gbm_format(config->gbm_format, GBM_FORMAT_XRGB8888, &b->gbm_format) < 0)
      goto err_compositor;

  b->iahwc.fd = open(device, O_RDWR);

	b->udev = udev_new();
	if (b->udev == NULL) {
		weston_log("failed to initialize udev context\n");
		goto err_compositor;
	}

  if (b->iahwc.fd < 0) {
    printf("unable to open gpu file\n");
    exit(EXIT_FAILURE);
  }
	if (config->seat_id)
      seat_id = config->seat_id;

	// Check if we run drm-backend using weston-launch
	compositor->launcher = weston_launcher_connect(compositor, config->tty,
                                                 seat_id, true);
	if (compositor->launcher == NULL) {
      weston_log("fatal: drm backend should be run "
                 "using weston-launch binary or as root\n");
      goto err_compositor;
	}

	if (create_outputs(b) < 0) {
		weston_log("failed to create output");
    goto err_compositor;
	}

  // session_notification XXX?TODO: make necessary changes
	b->session_listener.notify = session_notify;
	wl_signal_add(&compositor->session_signal, &b->session_listener);

  // XXX/TODO: currently using egl, add pixman support?
  if (init_egl(b) < 0) {
			weston_log("failed to initialize egl\n");
      goto err_compositor;
  }

  b->cursor_width = 256;
  b->cursor_height = 256;

	b->base.destroy = iahwc_destroy;
	b->base.restore = iahwc_restore;
	b->base.repaint_begin = iahwc_repaint_begin;
	b->base.repaint_flush = iahwc_repaint_flush;
	b->base.repaint_cancel = iahwc_repaint_cancel;

	wl_list_init(&b->plane_list);

  // XXX/TODO: No sprites for now
  // XXX/TODO: Add api in hwc to get plane info.
	//create_sprites(b);

	if (udev_input_init(&b->input,
                      compositor, b->udev, seat_id,
                      config->configure_device) < 0) {
		weston_log("failed to create input devices\n");
		goto err_compositor;
	}

  // XXX/TODO: setup hotplugging support from IAHWC

	weston_setup_vt_switch_bindings(compositor);

  // XXX/TODO: just import not sure we need it
	weston_compositor_add_debug_binding(compositor, KEY_O,
                                      planes_binding, b);
	weston_compositor_add_debug_binding(compositor, KEY_C,
                                      planes_binding, b);
	weston_compositor_add_debug_binding(compositor, KEY_V,
                                      planes_binding, b);
	/* weston_compositor_add_debug_binding(compositor, KEY_Q, */
  /*                                     recorder_binding, b); */
	weston_compositor_add_debug_binding(compositor, KEY_W,
                                      renderer_switch_binding, b);

  // XXX/TODO: This should be there.
	if (compositor->renderer->import_dmabuf) {
      if (linux_dmabuf_setup(compositor) < 0)
          weston_log("Error: initializing dmabuf "
                     "support failed.\n");
	}

	int ret = weston_plugin_api_register(compositor, WESTON_IAHWC_OUTPUT_API_NAME,
                                       &api, sizeof(api));

  if (ret)
    goto err_compositor;

  return b;

err_compositor:
	weston_compositor_shutdown(compositor);
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_iahwc_backend_config *config)
{
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
                    struct weston_backend_config *config_base)
{
	struct iahwc_backend *b;
	struct weston_iahwc_backend_config config = {{ 0, }};

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_IAHWC_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_iahwc_backend_config)) {
		weston_log("iahwc backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = iahwc_backend_create(compositor, &config);
	if (b == NULL)
		return -1;

	return 0;
}
