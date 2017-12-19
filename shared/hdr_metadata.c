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

#include <stdio.h>
#include <string.h>
#include <assert.h>

#ifdef IN_WESTON
#include <wayland-server.h>
#else
#define WL_EXPORT
#endif

#include "hdr_metadata.h"

enum metadata_id {
	METADATA_TYPE1,
};

WL_EXPORT void
weston_hdr_metadata(void *data,
                    uint16_t display_primary_r_x,
                    uint16_t display_primary_r_y,
                    uint16_t display_primary_g_x,
                    uint16_t display_primary_g_y,
                    uint16_t display_primary_b_x,
                    uint16_t display_primary_b_y,
                    uint16_t white_point_x,
                    uint16_t white_point_y,
                    uint16_t min_luminance,
                    uint16_t max_luminance,
                    uint16_t max_cll,
                    uint16_t max_fall,
                    enum hdr_metadata_eotf eotf)
{
	uint8_t *data8;
	uint16_t *data16;

	data8 = data;

	*data8++ = eotf;
	*data8++ = METADATA_TYPE1;

	data16 = (void*)data8;

	*data16++ = display_primary_r_x;
	*data16++ = display_primary_r_y;
	*data16++ = display_primary_g_x;
	*data16++ = display_primary_g_y;
	*data16++ = display_primary_b_x;
	*data16++ = display_primary_b_y;
	*data16++ = white_point_x;
	*data16++ = white_point_y;

	*data16++ = max_luminance;
	*data16++ = min_luminance;
	*data16++ = max_cll;
	*data16++ = max_fall;
}
