/*
 * Copyright © 2018 Harish Krupo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <signal.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>

#include "hdr-metadata-unstable-v1-client-protocol.h"
#include "shared/helpers.h"

struct display {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct zwp_hdr_metadata_v1 *hdr_metadata;
};

static int running = 1;

static void
signal_int(int signum)
{
  running = 0;
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
                       uint32_t name, const char *interface, uint32_t version)
{
  struct display *d = data;

  if (strcmp(interface, "wl_compositor") == 0) {
    d->compositor =
      wl_registry_bind(registry, name,
                       &wl_compositor_interface,
                       MIN(version, 4));
  } else if (strcmp(interface, "zwp_hdr_metadata_v1") == 0) {
    fprintf(stderr, "got hdr metadata interface\n");
    d->hdr_metadata =
      wl_registry_bind(registry, name,
                       &zwp_hdr_metadata_v1_interface,
                       MIN(version, 1));
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
static void
usage(int error_code)
{
  fprintf(stderr, "Usage: weston-hdr-test [OPTIONS]\n\n"
          "  -t\tETOF type\n\n"
          "  -h\tThis help text\n\n");

  exit(error_code);
}

int
main(int argc, char **argv)
{
  struct sigaction sigint;
  struct display display = { 0 };
  int i;
  int eotf_type = 0;
  enum zwp_hdr_metadata_v1_EOTF eotf_enum;
  struct wl_surface *surface;

  for (i = 1; i < argc; i++) {
    if (strcmp("-t", argv[i]) == 0 && i+1 < argc)
      eotf_type = atoi(argv[++i]);
    else if (strcmp("-h", argv[i]) == 0)
      usage(EXIT_SUCCESS);
    else
      usage(EXIT_FAILURE);
  }

  display.display = wl_display_connect(NULL);
  assert(display.display);

  display.registry = wl_display_get_registry(display.display);
  wl_registry_add_listener(display.registry,
                           &registry_listener, &display);

  wl_display_roundtrip(display.display);

  surface = wl_compositor_create_surface(display.compositor);

  if (!eotf_type)
    eotf_enum = ZWP_HDR_METADATA_V1_EOTF_ST_2084_PQ;
  else
    eotf_enum = ZWP_HDR_METADATA_V1_EOTF_HLG;

  fprintf(stderr, "setting metadata eotf %d\n", eotf_type);
  zwp_hdr_metadata_v1_set(display.hdr_metadata, surface, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, eotf_enum);

  sigint.sa_handler = signal_int;
  sigemptyset(&sigint.sa_mask);
  sigint.sa_flags = SA_RESETHAND;
  sigaction(SIGINT, &sigint, NULL);

  while (running) {
    wl_display_dispatch(display.display);
  }

  fprintf(stderr, "hdr test exiting\n");

  wl_surface_destroy(surface);

  if (display.compositor)
    wl_compositor_destroy(display.compositor);

  wl_registry_destroy(display.registry);
	wl_display_flush(display.display);
	wl_display_disconnect(display.display);

	return 0;
}
