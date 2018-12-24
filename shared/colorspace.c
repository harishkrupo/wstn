/*
 * Copyright © 2017 Intel Corporation
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

#include <stdlib.h>
#include <string.h>

#ifdef IN_WESTON
#include <wayland-server.h>
#else
#define WL_EXPORT
#endif

#include "helpers.h"
#include "colorspace.h"

static const struct weston_colorspace bt470m = {
	.r = {{ 0.670f, 0.330f, }},
	.g = {{ 0.210f, 0.710f, }},
	.b = {{ 0.140f, 0.080f, }},
	.whitepoint = {{ 0.3101f, 0.3162f, }},
	.name = "BT.470 M",
	.whitepoint_name = "C",
};

static const struct weston_colorspace bt470bg = {
	.r = {{ 0.640f, 0.330f, }},
	.g = {{ 0.290f, 0.600f, }},
	.b = {{ 0.150f, 0.060f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.470 B/G",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace smpte170m = {
	.r = {{ 0.630f, 0.340f, }},
	.g = {{ 0.310f, 0.595f, }},
	.b = {{ 0.155f, 0.070f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "SMPTE 170M",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace smpte240m = {
	.r = {{ 0.630f, 0.340f, }},
	.g = {{ 0.310f, 0.595f, }},
	.b = {{ 0.155f, 0.070f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "SMPTE 240M",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace bt709 = {
	.r = {{ 0.640f, 0.330f, }},
	.g = {{ 0.300f, 0.600f, }},
	.b = {{ 0.150f, 0.060f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.709",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace bt2020 = {
	.r = {{ 0.708f, 0.292f, }},
	.g = {{ 0.170f, 0.797f, }},
	.b = {{ 0.131f, 0.046f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.2020",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace srgb = {
	.r = {{ 0.640f, 0.330f, }},
	.g = {{ 0.300f, 0.600f, }},
	.b = {{ 0.150f, 0.060f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "sRGB",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace adobergb = {
	.r = {{ 0.640f, 0.330f, }},
	.g = {{ 0.210f, 0.710f, }},
	.b = {{ 0.150f, 0.060f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "AdobeRGB",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace dci_p3 = {
	.r = {{ 0.680f, 0.320f, }},
	.g = {{ 0.265f, 0.690f, }},
	.b = {{ 0.150f, 0.060f, }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "DCI-P3 D65",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace prophotorgb = {
	.r = {{ 0.7347f, 0.2653f, }},
	.g = {{ 0.1596f, 0.8404f, }},
	.b = {{ 0.0366f, 0.0001f, }},
	.whitepoint = {{ .3457, .3585 }},
	.name = "ProPhoto RGB",
	.whitepoint_name = "D50",
};

static const struct weston_colorspace ciergb = {
	.r = {{ 0.7347f, 0.2653f, }},
	.g = {{ 0.2738f, 0.7174f, }},
	.b = {{ 0.1666f, 0.0089f, }},
	.whitepoint = {{ 1.0f / 3.0f, 1.0f / 3.0f, }},
	.name = "CIE RGB",
	.whitepoint_name = "E",
};

static const struct weston_colorspace ciexyz = {
	.r = {{ 1.0f, 0.0f, }},
	.g = {{ 0.0f, 1.0f, }},
	.b = {{ 0.0f, 0.0f, }},
	.whitepoint = {{ 1.0f / 3.0f, 1.0f / 3.0f, }},
	.name = "CIE XYZ",
	.whitepoint_name = "E",
};

const struct weston_colorspace ap0 = {
	.r = {{ 0.7347f,  0.2653f, }},
	.g = {{ 0.0000f,  1.0000f, }},
	.b = {{ 0.0001f, -0.0770f, }},
	.whitepoint = {{ .32168f, .33767f, }},
	.name = "ACES primaries #0",
	.whitepoint_name = "D60",
};

const struct weston_colorspace ap1 = {
	.r = {{ 0.713f, 0.393f, }},
	.g = {{ 0.165f, 0.830f, }},
	.b = {{ 0.128f, 0.044f, }},
	.whitepoint = {{ 0.32168f, 0.33767f, }},
	.name = "ACES primaries #1",
	.whitepoint_name = "D60",
};

static const struct weston_colorspace * const colorspaces[] = {
	[WESTON_CS_BT470M] = &bt470m,
	[WESTON_CS_BT470BG] = &bt470bg,
	[WESTON_CS_SMPTE170M] = &smpte170m,
	[WESTON_CS_SMPTE240M] = &smpte240m,
	[WESTON_CS_BT709] = &bt709,
	[WESTON_CS_BT2020] = &bt2020,
	[WESTON_CS_SRGB] = &srgb,
	[WESTON_CS_ADOBERGB] = &adobergb,
	[WESTON_CS_DCI_P3] = &dci_p3,
	[WESTON_CS_PROPHOTORGB] = &prophotorgb,
	[WESTON_CS_CIERGB] = &ciergb,
	[WESTON_CS_CIEXYZ] = &ciexyz,
	[WESTON_CS_AP0] = &ap0,
	[WESTON_CS_AP1] = &ap1,
};

WL_EXPORT const struct weston_colorspace *
weston_colorspace_lookup(const char *name)
{
	unsigned i;

	if (!name)
		return NULL;

	for (i = 0; i < ARRAY_LENGTH(colorspaces); i++) {
		const struct weston_colorspace *c = colorspaces[i];

		if (!strcmp(c->name, name))
			return c;
	}

	return NULL;
}
