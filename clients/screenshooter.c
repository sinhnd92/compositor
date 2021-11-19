/*
 * Copyright © 2020 Collabora, Ltd.
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

#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <getopt.h>
#include <cairo.h>

#include <wayland-client.h>
#include "shared/helpers.h"
#include "shared/xalloc.h"
#include "shared/file-util.h"
#include "shared/os-compatibility.h"
#include "agl-screenshooter-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct screenshooter_data;

struct screenshooter_output {
	struct wl_output *output;
	struct wl_buffer *buffer;

	int width, height, offset_x, offset_y, scale;
	void *data;
	struct screenshooter_data *sh_data;

	struct wl_list link;	/** screenshooter_data::output_list */
};

struct xdg_output_v1_info {
	struct zxdg_output_v1 *xdg_output;
	struct screenshooter_output *output;

	char *name, *description;
	struct wl_list link;	/** screenshooter_data::xdg_output_list */
};


struct buffer_size {
	int width, height;

	int min_x, min_y;
	int max_x, max_y;
};

struct screenshooter_data {
	struct wl_display *display;
	struct wl_shm *shm;
	struct wl_list output_list;	/** screenshooter_output::link */
	struct wl_list xdg_output_list;	/** xdg_output_v1_info::link */

	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct agl_screenshooter *screenshooter;
	int buffer_copy_done;
};

static int opts = 0x0;

#define OPT_SCREENSHOT_OUTPUT		1
#define OPT_SHOW_ALL_OUTPUTS		2
#define OPT_SCREENSHOT_ALL_OUTPUTS	3

static void
display_handle_geometry(void *data,
			struct wl_output *wl_output,
			int x,
			int y,
			int physical_width,
			int physical_height,
			int subpixel,
			const char *make,
			const char *model,
			int transform)
{
	struct screenshooter_output *output;

	output = wl_output_get_user_data(wl_output);

	if (wl_output == output->output) {
		output->offset_x = x;
		output->offset_y = y;
	}
}

static void
display_handle_mode(void *data,
		    struct wl_output *wl_output,
		    uint32_t flags,
		    int width,
		    int height,
		    int refresh)
{
	struct screenshooter_output *output;

	output = wl_output_get_user_data(wl_output);

	if (wl_output == output->output && (flags & WL_OUTPUT_MODE_CURRENT)) {
		output->width = width;
		output->height = height;
	}
}

static void
display_handle_done(void *data, struct wl_output *wl_output)
{

}

static void
display_handle_scale(void *data, struct wl_output *wl_output,
		int32_t scale)
{
	struct screenshooter_output *output = data;
	output->scale = scale;
}


static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode,
	display_handle_done,
	display_handle_scale,
};

static void
handle_xdg_output_v1_logical_position(void *data, struct zxdg_output_v1 *output,
                                      int32_t x, int32_t y)
{
}

static void
handle_xdg_output_v1_logical_size(void *data, struct zxdg_output_v1 *output,
                                      int32_t width, int32_t height)
{
}

static void
handle_xdg_output_v1_done(void *data, struct zxdg_output_v1 *output)
{
	/* Don't bother waiting for this; there's no good reason a
	 * compositor will wait more than one roundtrip before sending
	 * these initial events. */
}

static void
handle_xdg_output_v1_name(void *data, struct zxdg_output_v1 *output,
                          const char *name)
{
	struct xdg_output_v1_info *xdg_output = data;
	xdg_output->name = strdup(name);
}

static void
handle_xdg_output_v1_description(void *data, struct zxdg_output_v1 *output,
                          const char *description)
{
	struct xdg_output_v1_info *xdg_output = data;
	xdg_output->description = strdup(description);
}

static const struct zxdg_output_v1_listener xdg_output_v1_listener = {
	.logical_position = handle_xdg_output_v1_logical_position,
	.logical_size = handle_xdg_output_v1_logical_size,
	.done = handle_xdg_output_v1_done,
	.name = handle_xdg_output_v1_name,
	.description = handle_xdg_output_v1_description,
};

static void
add_xdg_output_v1_info(struct screenshooter_data *shooter_data,
                       struct screenshooter_output *output)
{
	struct xdg_output_v1_info *xdg_output = zalloc(sizeof(*xdg_output));

	wl_list_insert(&shooter_data->xdg_output_list, &xdg_output->link);

	xdg_output->xdg_output =
		zxdg_output_manager_v1_get_xdg_output(shooter_data->xdg_output_manager,
						      output->output);

	zxdg_output_v1_add_listener(xdg_output->xdg_output,
				    &xdg_output_v1_listener, xdg_output);
	xdg_output->output = output;
}

static void
screenshot_done(void *data, struct agl_screenshooter *screenshooter, uint32_t status)
{
	struct screenshooter_data *sh_data = data;
	sh_data->buffer_copy_done = 1;
}

static const struct agl_screenshooter_listener screenshooter_listener = {
	screenshot_done
};

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	struct screenshooter_output *output;
	struct screenshooter_data *sh_data = data;

	if (strcmp(interface, "wl_output") == 0) {
		output = zalloc(sizeof(*output));
		if (!output)
			return;

		output->output = wl_registry_bind(registry, name,
						  &wl_output_interface, 1);
		output->sh_data = sh_data;
		wl_list_insert(&sh_data->output_list, &output->link);
		wl_output_add_listener(output->output, &output_listener, output);
	} else if (strcmp(interface, "wl_shm") == 0) {
		sh_data->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, "agl_screenshooter") == 0) {
		sh_data->screenshooter = wl_registry_bind(registry, name,
							  &agl_screenshooter_interface, 1);

		agl_screenshooter_add_listener(sh_data->screenshooter,
					       &screenshooter_listener, sh_data);
	} else if (strcmp(interface, "zxdg_output_manager_v1") == 0) {
		sh_data->xdg_output_manager = wl_registry_bind(registry, name,
					&zxdg_output_manager_v1_interface, version);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	/* XXX: unimplemented */
}

static const struct wl_registry_listener registry_listener = {
	handle_global,
	handle_global_remove
};

static struct wl_buffer *
screenshot_create_shm_buffer(int width, int height, void **data_out,
			     struct wl_shm *shm)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	int fd, size, stride;
	void *data;

	stride = width * 4;
	size = stride * height;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %s\n",
			size, strerror(errno));
		return NULL;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return NULL;
	}

	pool = wl_shm_create_pool(shm, fd, size);
	close(fd);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					   WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	*data_out = data;

	return buffer;
}

static void
screenshot_write_png_per_output(const struct buffer_size *buff_size,
				struct screenshooter_output *sh_output)
{
	int output_stride, buffer_stride, i;
	cairo_surface_t *surface;
	void *data, *d, *s;
	FILE *fp;
	char filepath[PATH_MAX];

	buffer_stride = buff_size->width * 4;
	data = xmalloc(buffer_stride * buff_size->height);
	if (!data)
		return;

	output_stride = sh_output->width * 4;
	s = sh_output->data;
	d = data + (sh_output->offset_y - buff_size->min_y) * buffer_stride +
		   (sh_output->offset_x - buff_size->min_x) * 4;

	for (i = 0; i < sh_output->height; i++) {
		memcpy(d, s, output_stride);
		d += buffer_stride;
		s += output_stride;
	}

	surface = cairo_image_surface_create_for_data(data,
						      CAIRO_FORMAT_ARGB32,
						      buff_size->width,
						      buff_size->height,
						      buffer_stride);

	fp = file_create_dated(getenv("XDG_PICTURES_DIR"), "agl-screenshot-",
			       ".png", filepath, sizeof(filepath));
	if (fp) {
		fclose(fp);
		cairo_surface_write_to_png(surface, filepath);
	}

	cairo_surface_destroy(surface);
	free(data);
}

static void
screenshot_write_png(const struct buffer_size *buff_size,
		     struct wl_list *output_list)
{
	int output_stride, buffer_stride, i;
	cairo_surface_t *surface;
	void *data, *d, *s;
	struct screenshooter_output *output, *next;
	FILE *fp;
	char filepath[PATH_MAX];

	buffer_stride = buff_size->width * 4;

	data = xmalloc(buffer_stride * buff_size->height);
	if (!data)
		return;

	wl_list_for_each_safe(output, next, output_list, link) {
		output_stride = output->width * 4;
		s = output->data;
		d = data + (output->offset_y - buff_size->min_y) * buffer_stride +
			   (output->offset_x - buff_size->min_x) * 4;

		for (i = 0; i < output->height; i++) {
			memcpy(d, s, output_stride);
			d += buffer_stride;
			s += output_stride;
		}

		free(output);
	}

	surface = cairo_image_surface_create_for_data(data,
						      CAIRO_FORMAT_ARGB32,
						      buff_size->width,
						      buff_size->height,
						      buffer_stride);

	fp = file_create_dated(getenv("XDG_PICTURES_DIR"), "agl-screenshot-",
			       ".png", filepath, sizeof(filepath));
	if (fp) {
		fclose(fp);
		cairo_surface_write_to_png(surface, filepath);
	}

	cairo_surface_destroy(surface);
	free(data);
}

static void
screenshot_set_buffer_size_per_output(struct buffer_size *buff_size,
				      struct screenshooter_output *output)
{
	buff_size->min_x = MIN(buff_size->min_x, output->offset_x);
	buff_size->min_y = MIN(buff_size->min_y, output->offset_y);
	buff_size->max_x = MAX(buff_size->max_x, output->offset_x + output->width);
	buff_size->max_y = MAX(buff_size->max_y, output->offset_y + output->height);

}

static void
screenshot_compute_output_offset(int *pos, struct screenshooter_output *sh_output)
{
	sh_output->offset_x = *pos;
	*pos += sh_output->width;
}

static int
screenshot_set_buffer_size(struct buffer_size *buff_size, struct wl_list *output_list)
{
	struct screenshooter_output *output;
	int pos = 0;

	buff_size->min_x = buff_size->min_y = INT_MAX;
	buff_size->max_x = buff_size->max_y = INT_MIN;

	wl_list_for_each_reverse(output, output_list, link)
		screenshot_compute_output_offset(&pos, output);

	wl_list_for_each(output, output_list, link)
		screenshot_set_buffer_size_per_output(buff_size, output);

	if (buff_size->max_x <= buff_size->min_x ||
	    buff_size->max_y <= buff_size->min_y)
		return -1;

	buff_size->width = buff_size->max_x - buff_size->min_x;
	buff_size->height = buff_size->max_y - buff_size->min_y;

	return 0;
}

static struct screenshooter_output *
agl_shooter_search_for_output(const char *output_name,
			      struct screenshooter_data *sh_data)
{
	struct screenshooter_output *found_output = NULL;
	struct xdg_output_v1_info *output;

	if (!output_name)
		return found_output;

	wl_list_for_each(output, &sh_data->xdg_output_list, link) {
		if (output->name && strcmp(output->name, output_name) == 0) {
			found_output = output->output;
			break;
		}
	}

	return found_output;
}

static void
agl_shooter_display_all_outputs(struct screenshooter_data *sh_data)
{
	struct xdg_output_v1_info *xdg_output;
	wl_list_for_each(xdg_output, &sh_data->xdg_output_list, link) {
		fprintf(stdout, "Output '%s', desc: '%s'\n", xdg_output->name,
				xdg_output->description);
	}
}


static void
agl_shooter_screenshot_all_outputs(struct screenshooter_data *sh_data)
{
	struct screenshooter_output *output;
	struct buffer_size buff_size = {};

	if (screenshot_set_buffer_size(&buff_size, &sh_data->output_list))
		return;

	wl_list_for_each(output, &sh_data->output_list, link) {
		output->buffer =
			screenshot_create_shm_buffer(output->width,
						     output->height,
						     &output->data,
						     sh_data->shm);

		agl_screenshooter_take_shot(sh_data->screenshooter,
					    output->output,
					    output->buffer);

		sh_data->buffer_copy_done = 0;
		while (!sh_data->buffer_copy_done)
			wl_display_roundtrip(sh_data->display);
	}

	screenshot_write_png(&buff_size, &sh_data->output_list);
}

static void
agl_shooter_screenshot_output(struct screenshooter_output *sh_output)
{
	int pos = 0;
	struct buffer_size buff_size = {};
	struct screenshooter_data *sh_data = sh_output->sh_data;

	screenshot_compute_output_offset(&pos, sh_output);
	screenshot_set_buffer_size_per_output(&buff_size, sh_output);

	sh_output->buffer =
		screenshot_create_shm_buffer(sh_output->width,
					     sh_output->height,
					     &sh_output->data, sh_data->shm);

	agl_screenshooter_take_shot(sh_data->screenshooter,
				    sh_output->output,
				    sh_output->buffer);

	sh_data->buffer_copy_done = 0;
	while (!sh_data->buffer_copy_done)
		wl_display_roundtrip(sh_data->display);

	screenshot_write_png_per_output(&buff_size, sh_output);
}

static void
agl_shooter_destroy_xdg_output_manager(struct screenshooter_data *sh_data)
{
	struct xdg_output_v1_info *xdg_output;

	wl_list_for_each(xdg_output, &sh_data->xdg_output_list, link) {
		free(xdg_output->name);
		free(xdg_output->description);
		zxdg_output_v1_destroy(xdg_output->xdg_output);
	}

	zxdg_output_manager_v1_destroy(sh_data->xdg_output_manager);
}

static void
print_usage_and_exit(void)
{
	fprintf(stderr, "./agl-screenshooter [-o OUTPUT_NAME] [-l] [-a]\n");

	fprintf(stderr, "\t-o OUTPUT_NAME -- take a screenshot of the output "
				"specified by OUTPUT_NAME\n");
	fprintf(stderr, "\t-a  -- take a screenshot of all the outputs found\n");
	fprintf(stderr, "\t-l  -- list all the outputs found\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wl_registry *registry;

	struct screenshooter_data sh_data = {};
	struct screenshooter_output *sh_output = NULL;
	int c, option_index;

	char *output_name = NULL;

	static struct option long_options[] = {
		{"output", 	required_argument, 0,  'o' },
		{"list", 	required_argument, 0,  'l' },
		{"all", 	required_argument, 0,  'a' },
		{"help",	no_argument      , 0,  'h' },
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "o:lah",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'o':
			output_name = optarg;
			opts |= (1 << OPT_SCREENSHOT_OUTPUT);
			break;
		case 'l':
			opts |= (1 << OPT_SHOW_ALL_OUTPUTS);
			break;
		case 'a':
			opts |= (1 << OPT_SCREENSHOT_ALL_OUTPUTS);
			break;
		default:
			print_usage_and_exit();
		}
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return EXIT_FAILURE;
	}

	wl_list_init(&sh_data.output_list);
	wl_list_init(&sh_data.xdg_output_list);
	sh_data.display = display;

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &sh_data);

	wl_display_dispatch(display);
	wl_display_roundtrip(display);


	if (sh_data.screenshooter == NULL) {
		fprintf(stderr, "Compositor doesn't support screenshooter\n");
		return EXIT_FAILURE;
	}

	wl_list_for_each(sh_output, &sh_data.output_list, link)
		add_xdg_output_v1_info(&sh_data, sh_output);

	/* do another round-trip for xdg_output */
	wl_display_roundtrip(sh_data.display);

	if (opts & (1 << OPT_SHOW_ALL_OUTPUTS)) {
		agl_shooter_display_all_outputs(&sh_data);
		agl_shooter_destroy_xdg_output_manager(&sh_data);
		return EXIT_SUCCESS;
	}

	if (opts & (1 << OPT_SCREENSHOT_ALL_OUTPUTS)) {
		agl_shooter_screenshot_all_outputs(&sh_data);
		agl_shooter_destroy_xdg_output_manager(&sh_data);
		return EXIT_SUCCESS;
	}

	sh_output = NULL;
	if (output_name)
		sh_output = agl_shooter_search_for_output(output_name, &sh_data);

	if (!sh_output && (opts & (1 << OPT_SCREENSHOT_OUTPUT))) {
		fprintf(stderr, "Could not find an output matching '%s'\n",
				output_name);
		agl_shooter_destroy_xdg_output_manager(&sh_data);
		return EXIT_FAILURE;
	}

	/* if we're still here just pick the first one available
	 * and use that. Still useful in case we are run without
	 * any args whatsoever */
	if (!sh_output)
		sh_output = container_of(sh_data.output_list.next,
					 struct screenshooter_output, link);

	/* take a screenshot only of that specific output */
	agl_shooter_screenshot_output(sh_output);
	agl_shooter_destroy_xdg_output_manager(&sh_data);

	return 0;
}
