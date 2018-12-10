/*
 * Copyright © 2018 Intel Corporation
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

#include <string.h>

#include "compositor.h"
#include "hdr_metadata.h"

#include "hdr-metadata-unstable-v1-server-protocol.h"

static void
hdr_metadata_destroy_request(struct wl_client *client,
                             struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
hdr_metadata_set_request(struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *surface_resource,
                         uint32_t display_primary_r_x,
                         uint32_t display_primary_r_y,
                         uint32_t display_primary_g_x,
                         uint32_t display_primary_g_y,
                         uint32_t display_primary_b_x,
                         uint32_t display_primary_b_y,
                         uint32_t white_point_x,
                         uint32_t white_point_y,
                         uint32_t max_luminance,
                         uint32_t min_luminance,
                         uint32_t max_cll,
                         uint32_t max_fall,
                         uint32_t eotf)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
  weston_log("Received hdr set request for surface: %p r (%d %d) br (%d %d)  r (%d %d)  r (%d %d) maxl %d minl %d maxc %d maxf %d eotf %d\n", surface, display_primary_r_x, display_primary_r_y, display_primary_b_x, display_primary_b_y, display_primary_g_x, display_primary_g_y, white_point_x, white_point_y, max_luminance, min_luminance, max_cll, max_fall, eotf);

	/* if (surface->hdr_metadata[1]) { */
	/* 	return; */
	/* } */
	/* weston_hdr_metadata(surface->hdr_metadata, */
	/* 		    display_primary_r_x, */
	/* 		    display_primary_r_y, */
	/* 		    display_primary_g_x, */
	/* 		    display_primary_g_y, */
	/* 		    display_primary_b_x, */
	/* 		    display_primary_b_y, */
	/* 		    white_point_x, */
	/* 		    white_point_y, */
	/* 		    max_luminance, */
	/* 		    min_luminance, */
	/* 		    max_cll, */
	/* 		    max_fall, */
	/* 		    eotf); */
}

static const struct zwp_hdr_metadata_v1_interface
zwp_hdr_metadata_implementation = {
	.destroy = hdr_metadata_destroy_request,
	.set = hdr_metadata_set_request,
};

static void
bind_hdr_metadata(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct weston_compositor *compositor = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &zwp_hdr_metadata_v1_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &zwp_hdr_metadata_implementation,
				       compositor, NULL);
}

WL_EXPORT int
weston_hdr_metadata_setup(struct weston_compositor *compositor)
{
	if (!wl_global_create(compositor->wl_display,
			      &zwp_hdr_metadata_v1_interface, 1,
			      compositor, bind_hdr_metadata))
		return -1;

	return 0;
}

