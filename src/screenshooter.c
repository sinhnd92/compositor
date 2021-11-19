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

#include "ivi-compositor.h"
#include "shared/helpers.h"

#include <libweston/libweston.h>
#include "agl-screenshooter-server-protocol.h"
#include <libweston/weston-log.h>

struct screenshooter {
	struct ivi_compositor *ivi;
	struct wl_global *global;
	struct wl_client *client;
	struct wl_listener destroy_listener;
};

static void
screenshooter_done(void *data, enum weston_screenshooter_outcome outcome)
{
	struct wl_resource *resource = data;

	if (outcome == WESTON_SCREENSHOOTER_NO_MEMORY) {
		wl_resource_post_no_memory(resource);
		return;
	}

	agl_screenshooter_send_done(resource, outcome);
}

static void
screenshooter_shoot(struct wl_client *client,
		    struct wl_resource *resource,
		    struct wl_resource *output_resource,
		    struct wl_resource *buffer_resource)
{
	struct weston_output *output =
		weston_head_from_resource(output_resource)->output;
	struct weston_buffer *buffer =
		weston_buffer_from_resource(buffer_resource);

	if (buffer == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	weston_screenshooter_shoot(output, buffer, screenshooter_done, resource);
}

static void
screenshooter_destructor_destroy(struct wl_client *client,
		                 struct wl_resource *global_resource)
{
	wl_resource_destroy(global_resource);
}

struct agl_screenshooter_interface screenshooter_implementation = {
	screenshooter_shoot,
	screenshooter_destructor_destroy
};

static void
bind_shooter(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	struct screenshooter *shooter = data;
	struct wl_resource *resource;
	bool debug_enabled = true;

	resource = wl_resource_create(client,
				      &agl_screenshooter_interface, 1, id);

	if (!debug_enabled && !shooter->client) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "screenshooter failed: permission denied. "\
				       "Debug must be enabled");
		return;
	}

	wl_resource_set_implementation(resource, &screenshooter_implementation,
				       data, NULL);
}

static void
screenshooter_destroy(struct wl_listener *listener, void *data)
{
	struct screenshooter *shooter =
		container_of(listener, struct screenshooter, destroy_listener);

	wl_list_remove(&shooter->destroy_listener.link);

	wl_global_destroy(shooter->global);
	free(shooter);
}

void
ivi_screenshooter_create(struct ivi_compositor *ivi)
{
	struct weston_compositor *ec = ivi->compositor;
	struct screenshooter *shooter;

	shooter = zalloc(sizeof(*shooter));
	if (shooter == NULL)
		return;

	shooter->ivi = ivi;
	shooter->global = wl_global_create(ec->wl_display,
					   &agl_screenshooter_interface, 1,
					   shooter, bind_shooter);

	shooter->destroy_listener.notify = screenshooter_destroy;
	wl_signal_add(&ec->destroy_signal, &shooter->destroy_listener);

	weston_log("Screenshooter interface created\n");
}
