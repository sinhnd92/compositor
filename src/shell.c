/*
 * Copyright © 2019 Collabora, Ltd.
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
#include "policy.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <libweston/libweston.h>
#include <libweston/config-parser.h>

#include "shared/os-compatibility.h"

#include "agl-shell-server-protocol.h"
#include "agl-shell-desktop-server-protocol.h"

#ifdef HAVE_WALTHAM
#include <waltham-transmitter/transmitter_api.h>
#endif

static void
create_black_surface_view(struct ivi_output *output);

void
agl_shell_desktop_advertise_application_id(struct ivi_compositor *ivi,
					   struct ivi_surface *surface)
{
	struct desktop_client *dclient;
	static bool display_adv = false;

	if (surface->advertised_on_launch)
		return;

	/* advertise to all desktop clients the new surface */
	wl_list_for_each(dclient, &ivi->desktop_clients, link) {
		const char *app_id =
			weston_desktop_surface_get_app_id(surface->dsurface);
		if (app_id == NULL) {
			if (!display_adv) {
				weston_log("WARNING app_is is null, unable to advertise\n");
				display_adv = true;
			}
			return;
		}
		agl_shell_desktop_send_application(dclient->resource, app_id);
		surface->advertised_on_launch = true;
	}
}

void
ivi_set_desktop_surface(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	assert(surface->role == IVI_SURFACE_ROLE_NONE);

	surface->role = IVI_SURFACE_ROLE_DESKTOP;
	wl_list_insert(&surface->ivi->surfaces, &surface->link);

	agl_shell_desktop_advertise_application_id(ivi, surface);
}

static void
ivi_set_desktop_surface_popup(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	assert(surface->role == IVI_SURFACE_ROLE_NONE);

	surface->role = IVI_SURFACE_ROLE_POPUP;
	wl_list_insert(&ivi->surfaces, &surface->link);

	agl_shell_desktop_advertise_application_id(ivi, surface);
}

static void
ivi_set_desktop_surface_fullscreen(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	assert(surface->role == IVI_SURFACE_ROLE_NONE);

	surface->role = IVI_SURFACE_ROLE_FULLSCREEN;
	wl_list_insert(&ivi->surfaces, &surface->link);

	agl_shell_desktop_advertise_application_id(ivi, surface);
}

#ifdef HAVE_WALTHAM
void
ivi_destroy_waltham_destroy(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	const struct weston_transmitter_api *api =
		ivi->waltham_transmitter_api;

	if (!api)
		return;

	if (surface->waltham_surface.transmitter_surface)
		api->surface_destroy(surface->waltham_surface.transmitter_surface);
}

static void
ivi_output_notify_waltham_plugin(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	const struct weston_transmitter_api *api = ivi->waltham_transmitter_api;
	struct weston_transmitter *transmitter;
	struct weston_transmitter_remote *trans_remote;
	struct weston_surface *weston_surface;
	struct weston_output *woutput = surface->remote.output->output;
	const char *app_id;

	if (!api)
		return;

	transmitter = api->transmitter_get(ivi->compositor);
	if (!transmitter)
		return;

	trans_remote = api->get_transmitter_remote(woutput->name, transmitter);
	if (!trans_remote) {
		weston_log("Could not find a valie weston_transmitter_remote "
				"that matches the output %s\n", woutput->name);
		return;
	}

	app_id = weston_desktop_surface_get_app_id(surface->dsurface);
	weston_surface =
		weston_desktop_surface_get_surface(surface->dsurface);

	weston_log("Forwarding app_id %s to remote %s\n", app_id, woutput->name);

	/* this will have the effect of informing the remote side to create a
	 * surface with the name app_id. W/ xdg-shell the following happens:
	 *
	 * compositor (server):
	 * surface_push_to_remote():
	 * 	waltham-transmitter plug-in
	 * 		-> wthp_ivi_app_id_surface_create()
	 *
	 * client -- on the receiver side:
	 * 	-> wthp_ivi_app_id_surface_create()
	 * 		-> wth_receiver_weston_main()
	 * 			-> wl_compositor_create_surface()
	 * 			-> xdg_wm_base_get_xdg_surface
	 * 			-> xdg_toplevel_set_app_id()
	 * 			-> gst_init()
	 * 			-> gst_parse_launch()
	 *
	 * wth_receiver_weston_main() will be invoked from the handler of
	 * wthp_ivi_app_id_surface_create() and is responsible for setting-up
	 * the gstreamer pipeline as well.
	 */
	surface->waltham_surface.transmitter_surface =
	    api->surface_push_to_remote(weston_surface, app_id, trans_remote, NULL);
}

#else
void
ivi_destroy_waltham_destroy(struct ivi_surface *surface)
{
}
static void
ivi_output_notify_waltham_plugin(struct ivi_surface *surface)
{
}
#endif

static void
ivi_set_desktop_surface_remote(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	struct weston_view *view;
	struct ivi_output *output = surface->remote.output;

	assert(surface->role == IVI_SURFACE_ROLE_NONE);

	/* remote type are the same as desktop just that client can tell
	 * the compositor to start on another output */
	surface->role = IVI_SURFACE_ROLE_REMOTE;

	/* if thew black surface view is mapped on the mean we need
	 * to remove it in order to start showing the 'remote' surface
	 * just being added */
	view = output->fullscreen_view.fs->view;
	if (view->is_mapped || view->surface->is_mapped)
		remove_black_surface(output);

	if (output->type == OUTPUT_WALTHAM)
		ivi_output_notify_waltham_plugin(surface);

	wl_list_insert(&ivi->surfaces, &surface->link);
}


static void
ivi_set_desktop_surface_split(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	assert(surface->role == IVI_SURFACE_ROLE_NONE);

	if (surface->split.orientation == AGL_SHELL_DESKTOP_APP_ROLE_SPLIT_VERTICAL)
		surface->role = IVI_SURFACE_ROLE_SPLIT_V;
	else
		surface->role = IVI_SURFACE_ROLE_SPLIT_H;

	wl_list_insert(&ivi->surfaces, &surface->link);

	agl_shell_desktop_advertise_application_id(ivi, surface);
}

static void
ivi_set_pending_desktop_surface_popup(struct ivi_output *ioutput,
				      int x, int y, int bx, int by, int width, int height,
				      const char *app_id)
{
	struct ivi_compositor *ivi = ioutput->ivi;
	size_t len_app_id = strlen(app_id);

	struct pending_popup *p_popup = zalloc(sizeof(*p_popup));

	p_popup->app_id = zalloc(sizeof(char) * (len_app_id + 1));
	memcpy(p_popup->app_id, app_id, len_app_id);
	p_popup->ioutput = ioutput;
	p_popup->x = x;
	p_popup->y = y;

	p_popup->bb.x = bx;
	p_popup->bb.y = by;
	p_popup->bb.width = width;
	p_popup->bb.height = height;

	wl_list_insert(&ivi->popup_pending_apps, &p_popup->link);
}

static void
ivi_set_pending_desktop_surface_fullscreen(struct ivi_output *ioutput,
					   const char *app_id)
{
	struct ivi_compositor *ivi = ioutput->ivi;
	size_t len_app_id = strlen(app_id);

	struct pending_fullscreen *fs = zalloc(sizeof(*fs));

	fs->app_id = zalloc(sizeof(char) * (len_app_id + 1));
	memcpy(fs->app_id, app_id, len_app_id);

	fs->ioutput = ioutput;

	wl_list_insert(&ivi->fullscreen_pending_apps, &fs->link);
}

static void
ivi_set_pending_desktop_surface_split(struct ivi_output *ioutput,
				      const char *app_id, uint32_t orientation)
{
	struct ivi_compositor *ivi = ioutput->ivi;
	struct ivi_surface *surf;
	size_t len_app_id = strlen(app_id);
	struct pending_split *split;

	if (orientation != AGL_SHELL_DESKTOP_APP_ROLE_SPLIT_VERTICAL &&
	    orientation != AGL_SHELL_DESKTOP_APP_ROLE_SPLIT_HORIZONTAL)
		return;

	/* more than one is un-supported, do note we need to do
	 * conversion for surface roles instead of using the protocol ones */
	wl_list_for_each(surf, &ivi->surfaces, link)
		if (surf->role == IVI_SURFACE_ROLE_SPLIT_V ||
		    surf->role == IVI_SURFACE_ROLE_SPLIT_H)
			return;

	split = zalloc(sizeof(*split));
	split->app_id = zalloc(sizeof(char) * (len_app_id + 1));
	memcpy(split->app_id, app_id, len_app_id);

	split->ioutput = ioutput;
	split->orientation = orientation;

	wl_list_insert(&ivi->split_pending_apps, &split->link);
}

void
ivi_set_pending_desktop_surface_remote(struct ivi_output *ioutput,
		const char *app_id)
{
	struct ivi_compositor *ivi = ioutput->ivi;
	size_t len_app_id = strlen(app_id);

	struct pending_remote *remote = zalloc(sizeof(*remote));

	remote->app_id = zalloc(sizeof(char) * (len_app_id + 1));
	memcpy(remote->app_id, app_id, len_app_id);

	remote->ioutput = ioutput;

	wl_list_insert(&ivi->remote_pending_apps, &remote->link);
}


static void
ivi_remove_pending_desktop_surface_split(struct pending_split *split)
{
	free(split->app_id);
	wl_list_remove(&split->link);
	free(split);
}

static void
ivi_remove_pending_desktop_surface_fullscreen(struct pending_fullscreen *fs)
{
	free(fs->app_id);
	wl_list_remove(&fs->link);
	free(fs);
}

static void
ivi_remove_pending_desktop_surface_popup(struct pending_popup *p_popup)
{
	free(p_popup->app_id);
	wl_list_remove(&p_popup->link);
	free(p_popup);
}

static void
ivi_remove_pending_desktop_surface_remote(struct pending_remote *remote)
{
	free(remote->app_id);
	wl_list_remove(&remote->link);
	free(remote);
}

static bool
ivi_check_pending_desktop_surface_popup(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	struct pending_popup *p_popup, *next_p_popup;
	const char *_app_id =
			weston_desktop_surface_get_app_id(surface->dsurface);

	if (wl_list_empty(&ivi->popup_pending_apps) || !_app_id)
		return false;

	wl_list_for_each_safe(p_popup, next_p_popup,
			      &ivi->popup_pending_apps, link) {
		if (!strcmp(_app_id, p_popup->app_id)) {
			surface->popup.output = p_popup->ioutput;
			surface->popup.x = p_popup->x;
			surface->popup.y = p_popup->y;

			surface->popup.bb.x = p_popup->bb.x;
			surface->popup.bb.y = p_popup->bb.y;
			surface->popup.bb.width = p_popup->bb.width;
			surface->popup.bb.height = p_popup->bb.height;

			ivi_remove_pending_desktop_surface_popup(p_popup);
			return true;
		}
	}

	return false;
}

static bool
ivi_check_pending_desktop_surface_split(struct ivi_surface *surface)
{
	struct pending_split *split_surf, *next_split_surf;
	struct ivi_compositor *ivi = surface->ivi;
	const char *_app_id =
			weston_desktop_surface_get_app_id(surface->dsurface);

	if (wl_list_empty(&ivi->split_pending_apps) || !_app_id)
		return false;

	wl_list_for_each_safe(split_surf, next_split_surf,
			      &ivi->split_pending_apps, link) {
		if (!strcmp(_app_id, split_surf->app_id)) {
			surface->split.output = split_surf->ioutput;
			surface->split.orientation = split_surf->orientation;
			ivi_remove_pending_desktop_surface_split(split_surf);
			return true;
		}
	}

	return false;
}

static bool
ivi_check_pending_desktop_surface_fullscreen(struct ivi_surface *surface)
{
	struct pending_fullscreen *fs_surf, *next_fs_surf;
	struct ivi_compositor *ivi = surface->ivi;
	const char *_app_id =
			weston_desktop_surface_get_app_id(surface->dsurface);

	if (wl_list_empty(&ivi->fullscreen_pending_apps) || !_app_id)
		return false;

	wl_list_for_each_safe(fs_surf, next_fs_surf,
			      &ivi->fullscreen_pending_apps, link) {
		if (!strcmp(_app_id, fs_surf->app_id)) {
			surface->fullscreen.output = fs_surf->ioutput;
			ivi_remove_pending_desktop_surface_fullscreen(fs_surf);
			return true;
		}
	}

	return false;
}

static bool
ivi_check_pending_desktop_surface_remote(struct ivi_surface *surface)
{
	struct pending_remote *remote_surf, *next_remote_surf;
	struct ivi_compositor *ivi = surface->ivi;
	const char *_app_id =
		weston_desktop_surface_get_app_id(surface->dsurface);

	if (wl_list_empty(&ivi->remote_pending_apps) || !_app_id)
		return false;

	wl_list_for_each_safe(remote_surf, next_remote_surf,
			      &ivi->remote_pending_apps, link) {
		if (!strcmp(_app_id, remote_surf->app_id)) {
			surface->remote.output = remote_surf->ioutput;
			ivi_remove_pending_desktop_surface_remote(remote_surf);
			return true;
		}
	}

	return false;
}

bool
ivi_check_pending_surface(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	struct wl_list *role_pending_list;
	struct pending_popup *p_popup;
	struct pending_split *p_split;
	struct pending_fullscreen *p_fullscreen;
	struct pending_remote *p_remote;
	const char *app_id =
		weston_desktop_surface_get_app_id(surface->dsurface);

	if (!app_id)
		return false;

	role_pending_list = &ivi->popup_pending_apps;
	wl_list_for_each(p_popup, role_pending_list, link) {
		if (!strcmp(app_id, p_popup->app_id)) {
			return true;
		}
	}

	role_pending_list = &ivi->split_pending_apps;
	wl_list_for_each(p_split, role_pending_list, link) {
		if (!strcmp(app_id, p_split->app_id)) {
			return true;
		}
	}

	role_pending_list = &ivi->fullscreen_pending_apps;
	wl_list_for_each(p_fullscreen, role_pending_list, link) {
		if (!strcmp(app_id, p_fullscreen->app_id)) {
			return true;
		}
	}

	role_pending_list = &ivi->remote_pending_apps;
	wl_list_for_each(p_remote, role_pending_list, link) {
		if (!strcmp(app_id, p_remote->app_id)) {
			return true;
		}
	}

	/* else, we are a regular desktop surface */
	return false;
}


void
ivi_check_pending_desktop_surface(struct ivi_surface *surface)
{
	bool ret = false;

	ret = ivi_check_pending_desktop_surface_popup(surface);
	if (ret) {
		ivi_set_desktop_surface_popup(surface);
		ivi_layout_popup_committed(surface);
		return;
	}

	ret = ivi_check_pending_desktop_surface_split(surface);
	if (ret) {
		ivi_set_desktop_surface_split(surface);
		ivi_layout_split_committed(surface);
		return;
	}

	ret = ivi_check_pending_desktop_surface_fullscreen(surface);
	if (ret) {
		ivi_set_desktop_surface_fullscreen(surface);
		ivi_layout_fullscreen_committed(surface);
		return;
	}

	ret = ivi_check_pending_desktop_surface_remote(surface);
	if (ret) {
		ivi_set_desktop_surface_remote(surface);
		ivi_layout_desktop_committed(surface);
		return;
	}

	/* if we end up here means we have a regular desktop app and
	 * try to activate it */
	ivi_set_desktop_surface(surface);
	ivi_layout_desktop_committed(surface);
}

void
ivi_shell_init_black_fs(struct ivi_compositor *ivi)
{
	struct ivi_output *out;

	wl_list_for_each(out, &ivi->outputs, link) {
		create_black_surface_view(out);
		insert_black_surface(out);
	}
}

int
ivi_shell_init(struct ivi_compositor *ivi)
{
	weston_layer_init(&ivi->hidden, ivi->compositor);
	weston_layer_init(&ivi->background, ivi->compositor);
	weston_layer_init(&ivi->normal, ivi->compositor);
	weston_layer_init(&ivi->panel, ivi->compositor);
	weston_layer_init(&ivi->popup, ivi->compositor);
	weston_layer_init(&ivi->fullscreen, ivi->compositor);

	weston_layer_set_position(&ivi->hidden,
				  WESTON_LAYER_POSITION_HIDDEN);
	weston_layer_set_position(&ivi->background,
				  WESTON_LAYER_POSITION_BACKGROUND);
	weston_layer_set_position(&ivi->normal,
				  WESTON_LAYER_POSITION_NORMAL);
	weston_layer_set_position(&ivi->panel,
				  WESTON_LAYER_POSITION_UI);
	weston_layer_set_position(&ivi->popup,
				  WESTON_LAYER_POSITION_TOP_UI);
	weston_layer_set_position(&ivi->fullscreen,
				  WESTON_LAYER_POSITION_FULLSCREEN);

	return 0;
}

static void
ivi_shell_advertise_xdg_surfaces(struct ivi_compositor *ivi, struct wl_resource *resource)
{
	struct ivi_surface *surface;

	wl_list_for_each(surface, &ivi->surfaces, link) {
		const char *app_id =
			weston_desktop_surface_get_app_id(surface->dsurface);
		if (app_id == NULL) {
			weston_log("WARNING app_is is null, unable to advertise\n");
			return;
		}
		agl_shell_desktop_send_application(resource, app_id);
	}
}

static void
client_exec(const char *command, int fd)
{
	sigset_t sig;
	char s[32];

	/* Don't give the child our signal mask */
	sigfillset(&sig);
	sigprocmask(SIG_UNBLOCK, &sig, NULL);

	/* Launch clients as the user; don't give them the wrong euid */
	if (seteuid(getuid()) == -1) {
		weston_log("seteuid failed: %s\n", strerror(errno));
		return;
	}

	/* Duplicate fd to unset the CLOEXEC flag. We don't need to worry about
	 * clobbering fd, as we'll exit/exec either way.
	 */
	fd = dup(fd);
	if (fd == -1) {
		weston_log("dup failed: %s\n", strerror(errno));
		return;
	}

	snprintf(s, sizeof s, "%d", fd);
	setenv("WAYLAND_SOCKET", s, 1);

	execl("/bin/sh", "/bin/sh", "-c", command, NULL);
	weston_log("executing '%s' failed: %s", command, strerror(errno));
}

static struct wl_client *
launch_shell_client(struct ivi_compositor *ivi, const char *command)
{
	struct wl_client *client;
	int sock[2];
	pid_t pid;

	weston_log("launching' %s'\n", command);

	if (os_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sock) < 0) {
		weston_log("socketpair failed while launching '%s': %s\n",
			   command, strerror(errno));
		return NULL;
	}

	pid = fork();
	if (pid == -1) {
		close(sock[0]);
		close(sock[1]);
		weston_log("fork failed while launching '%s': %s\n",
			   command, strerror(errno));
		return NULL;
	}

	if (pid == 0) {
		client_exec(command, sock[1]);
		_Exit(EXIT_FAILURE);
	}
	close(sock[1]);

	client = wl_client_create(ivi->compositor->wl_display, sock[0]);
	if (!client) {
		close(sock[0]);
		weston_log("Failed to create wayland client for '%s'",
			   command);
		return NULL;
	}

	return client;
}

int
ivi_launch_shell_client(struct ivi_compositor *ivi)
{
	struct weston_config_section *section;
	char *command = NULL;

	section = weston_config_get_section(ivi->config, "shell-client",
					    NULL, NULL);
	if (section)
		weston_config_section_get_string(section, "command",
						 &command, NULL);

	if (!command)
		return -1;

	ivi->shell_client.client = launch_shell_client(ivi, command);
	if (!ivi->shell_client.client)
		return -1;

	return 0;
}

static void
destroy_black_view(struct wl_listener *listener, void *data)
{
	struct fullscreen_view *fs =
		wl_container_of(listener, fs, fs_destroy);


	if (fs && fs->fs) {
		wl_list_remove(&fs->fs_destroy.link);
		free(fs->fs);
	}
}


static void
create_black_surface_view(struct ivi_output *output)
{
	struct weston_surface *surface = NULL;
	struct weston_view *view;
	struct ivi_compositor *ivi = output->ivi;
	struct weston_compositor *wc= ivi->compositor;
	struct weston_output *woutput = output->output;

	if (!woutput)
		return;

	surface = weston_surface_create(wc);
	view = weston_view_create(surface);

	assert(view || surface);

	weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1);
	weston_surface_set_size(surface, woutput->width, woutput->height);
	weston_view_set_position(view, woutput->x, woutput->y);

	output->fullscreen_view.fs = zalloc(sizeof(struct ivi_surface));
	output->fullscreen_view.fs->view = view;

	output->fullscreen_view.fs_destroy.notify = destroy_black_view;
	wl_signal_add(&woutput->destroy_signal,
		      &output->fullscreen_view.fs_destroy);
}

void
remove_black_surface(struct ivi_output *output)
{
	struct weston_view *view;

	if (!output &&
	    !output->fullscreen_view.fs &&
	    !output->fullscreen_view.fs->view) {
		weston_log("Output %s doesn't have a surface installed!\n", output->name);
		return;
	}

	view = output->fullscreen_view.fs->view;
	assert(view->is_mapped == true ||
	       view->surface->is_mapped == true);

	view->is_mapped = false;
	view->surface->is_mapped = false;

	weston_layer_entry_remove(&view->layer_link);
	weston_view_update_transform(view);

	weston_view_damage_below(view);
}

void
insert_black_surface(struct ivi_output *output)
{
	struct weston_view *view;

	if ((!output &&
	    !output->fullscreen_view.fs &&
	    !output->fullscreen_view.fs->view) || !output->output) {
		weston_log("Output %s doesn't have a surface installed!\n", output->name);
		return;
	}

	view = output->fullscreen_view.fs->view;
	if (view->is_mapped || view->surface->is_mapped)
		return;

	weston_layer_entry_remove(&view->layer_link);
	weston_layer_entry_insert(&output->ivi->fullscreen.view_list,
				  &view->layer_link);

	view->is_mapped = true;
	view->surface->is_mapped = true;

	weston_view_update_transform(view);
	weston_view_damage_below(view);
}

static void
shell_ready(struct wl_client *client, struct wl_resource *shell_res)
{
	struct ivi_compositor *ivi = wl_resource_get_user_data(shell_res);
	struct ivi_output *output;
	struct ivi_surface *surface, *tmp;

	/* Init already finished. Do nothing */
	if (ivi->shell_client.ready)
		return;

	ivi->shell_client.ready = true;

	wl_list_for_each(output, &ivi->outputs, link) {
		if (output->background)
			remove_black_surface(output);
		ivi_layout_init(ivi, output);
	}

	wl_list_for_each_safe(surface, tmp, &ivi->pending_surfaces, link) {
		wl_list_remove(&surface->link);
		wl_list_init(&surface->link);
		ivi_check_pending_desktop_surface(surface);
		surface->checked_pending = true;
	}
}

static void
shell_set_background(struct wl_client *client,
		     struct wl_resource *shell_res,
		     struct wl_resource *surface_res,
		     struct wl_resource *output_res)
{
	struct weston_head *head = weston_head_from_resource(output_res);
	struct weston_output *woutput = weston_head_get_output(head);
	struct ivi_output *output = to_ivi_output(woutput);
	struct weston_surface *wsurface = wl_resource_get_user_data(surface_res);
	struct weston_desktop_surface *dsurface;
	struct ivi_surface *surface;

	dsurface = weston_surface_get_desktop_surface(wsurface);
	if (!dsurface) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface must be a desktop surface");
		return;
	}

	surface = weston_desktop_surface_get_user_data(dsurface);
	if (surface->role != IVI_SURFACE_ROLE_NONE) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface already has another ivi role");
		return;
	}

	if (output->background) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_BACKGROUND_EXISTS,
				       "output already has background");
		return;
	}

	surface->checked_pending = true;
	surface->role = IVI_SURFACE_ROLE_BACKGROUND;
	surface->bg.output = output;
	wl_list_remove(&surface->link);
	wl_list_init(&surface->link);

	output->background = surface;

	weston_desktop_surface_set_maximized(dsurface, true);
	weston_desktop_surface_set_size(dsurface,
					output->output->width,
					output->output->height);
}

static void
shell_set_panel(struct wl_client *client,
		struct wl_resource *shell_res,
		struct wl_resource *surface_res,
		struct wl_resource *output_res,
		uint32_t edge)
{
	struct weston_head *head = weston_head_from_resource(output_res);
	struct weston_output *woutput = weston_head_get_output(head);
	struct ivi_output *output = to_ivi_output(woutput);
	struct weston_surface *wsurface = wl_resource_get_user_data(surface_res);
	struct weston_desktop_surface *dsurface;
	struct ivi_surface *surface;
	struct ivi_surface **member;
	int32_t width = 0, height = 0;

	dsurface = weston_surface_get_desktop_surface(wsurface);
	if (!dsurface) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface must be a desktop surface");
		return;
	}

	surface = weston_desktop_surface_get_user_data(dsurface);
	if (surface->role != IVI_SURFACE_ROLE_NONE) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "surface already has another ivi role");
		return;
	}

	switch (edge) {
	case AGL_SHELL_EDGE_TOP:
		member = &output->top;
		break;
	case AGL_SHELL_EDGE_BOTTOM:
		member = &output->bottom;
		break;
	case AGL_SHELL_EDGE_LEFT:
		member = &output->left;
		break;
	case AGL_SHELL_EDGE_RIGHT:
		member = &output->right;
		break;
	default:
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_INVALID_ARGUMENT,
				       "invalid edge for panel");
		return;
	}

	if (*member) {
		wl_resource_post_error(shell_res,
				       AGL_SHELL_ERROR_BACKGROUND_EXISTS,
				       "output already has panel on this edge");
		return;
	}

	surface->checked_pending = true;
	surface->role = IVI_SURFACE_ROLE_PANEL;
	surface->panel.output = output;
	surface->panel.edge = edge;
	wl_list_remove(&surface->link);
	wl_list_init(&surface->link);

	*member = surface;

	switch (surface->panel.edge) {
	case AGL_SHELL_EDGE_TOP:
	case AGL_SHELL_EDGE_BOTTOM:
		width = woutput->width;
		break;
	case AGL_SHELL_EDGE_LEFT:
	case AGL_SHELL_EDGE_RIGHT:
		height = woutput->height;
		break;
	}

	weston_desktop_surface_set_size(dsurface, width, height);
}

void
shell_advertise_app_state(struct ivi_compositor *ivi, const char *app_id,
			  const char *data, uint32_t app_state)
{
	struct desktop_client *dclient;
	uint32_t app_role;
	struct ivi_surface *surf = ivi_find_app(ivi, app_id);
	struct ivi_policy *policy = ivi->policy;

	/* FIXME: should queue it here and see when binding agl-shell-desktop
	 * if there are any to be sent */
	if (!surf)
		return;

	if (!app_id)
		return;

	if (policy && policy->api.surface_advertise_state_change &&
	    !policy->api.surface_advertise_state_change(surf, surf->ivi)) {
		return;
	}

	app_role = surf->role;
	if (app_role == IVI_SURFACE_ROLE_POPUP)
		app_role = AGL_SHELL_DESKTOP_APP_ROLE_POPUP;

	wl_list_for_each(dclient, &ivi->desktop_clients, link)
		agl_shell_desktop_send_state_app(dclient->resource, app_id,
						 data, app_state, app_role);
}

static void
shell_activate_app(struct wl_client *client,
		   struct wl_resource *shell_res,
		   const char *app_id,
		   struct wl_resource *output_res)
{
	struct weston_head *head = weston_head_from_resource(output_res);
	struct weston_output *woutput = weston_head_get_output(head);
	struct ivi_output *output = to_ivi_output(woutput);

	ivi_layout_activate(output, app_id);
}

static void
shell_desktop_activate_app(struct wl_client *client,
			   struct wl_resource *shell_res,
			   const char *app_id, const char *data,
			   struct wl_resource *output_res)
{
	struct weston_head *head = weston_head_from_resource(output_res);
	struct weston_output *woutput = weston_head_get_output(head);
	struct ivi_output *output = to_ivi_output(woutput);

	ivi_layout_activate(output, app_id);
	shell_advertise_app_state(output->ivi, app_id,
				  data, AGL_SHELL_DESKTOP_APP_STATE_ACTIVATED);
}

static void
shell_deactivate_app(struct wl_client *client,
		   struct wl_resource *shell_res,
		   const char *app_id)
{
	struct desktop_client *dclient = wl_resource_get_user_data(shell_res);
	struct ivi_compositor *ivi = dclient->ivi;

	ivi_layout_deactivate(ivi, app_id);
	shell_advertise_app_state(ivi, app_id,
				  NULL, AGL_SHELL_DESKTOP_APP_STATE_DEACTIVATED);
}

static const struct agl_shell_interface agl_shell_implementation = {
	.ready = shell_ready,
	.set_background = shell_set_background,
	.set_panel = shell_set_panel,
	.activate_app = shell_activate_app,
};

static void
shell_desktop_set_app_property(struct wl_client *client,
			       struct wl_resource *shell_res,
			       const char *app_id, uint32_t role,
			       int x, int y, int bx, int by,
			       int width, int height,
			       struct wl_resource *output_res)
{
	struct weston_head *head = weston_head_from_resource(output_res);
	struct weston_output *woutput = weston_head_get_output(head);
	struct ivi_output *output = to_ivi_output(woutput);

	switch (role) {
	case AGL_SHELL_DESKTOP_APP_ROLE_POPUP:
		ivi_set_pending_desktop_surface_popup(output, x, y, bx, by,
						      width, height, app_id);
		break;
	case AGL_SHELL_DESKTOP_APP_ROLE_FULLSCREEN:
		ivi_set_pending_desktop_surface_fullscreen(output, app_id);
		break;
	case AGL_SHELL_DESKTOP_APP_ROLE_SPLIT_VERTICAL:
	case AGL_SHELL_DESKTOP_APP_ROLE_SPLIT_HORIZONTAL:
		ivi_set_pending_desktop_surface_split(output, app_id, role);
		break;
	case AGL_SHELL_DESKTOP_APP_ROLE_REMOTE:
		ivi_set_pending_desktop_surface_remote(output, app_id);
		break;
	default:
		break;
	}
}

static const struct agl_shell_desktop_interface agl_shell_desktop_implementation = {
	.activate_app = shell_desktop_activate_app,
	.set_app_property = shell_desktop_set_app_property,
	.deactivate_app = shell_deactivate_app,
};

static void
unbind_agl_shell(struct wl_resource *resource)
{
	struct ivi_compositor *ivi;
	struct ivi_output *output;
	struct ivi_surface *surf, *surf_tmp;

	ivi = wl_resource_get_user_data(resource);
	wl_list_for_each(output, &ivi->outputs, link) {
		/* reset the active surf if there's one present */
		if (output->active) {
			output->active->view->is_mapped = false;
			output->active->view->surface->is_mapped = false;

			weston_layer_entry_remove(&output->active->view->layer_link);
			output->active = NULL;
		}

		insert_black_surface(output);
	}

	wl_list_for_each_safe(surf, surf_tmp, &ivi->surfaces, link) {
		wl_list_remove(&surf->link);
		wl_list_init(&surf->link);
	}

	wl_list_for_each_safe(surf, surf_tmp, &ivi->pending_surfaces, link) {
		wl_list_remove(&surf->link);
		wl_list_init(&surf->link);
	}

	wl_list_init(&ivi->surfaces);
	wl_list_init(&ivi->pending_surfaces);

	ivi->shell_client.ready = false;
	ivi->shell_client.resource = NULL;
	ivi->shell_client.client = NULL;
}

static void
bind_agl_shell(struct wl_client *client,
	       void *data, uint32_t version, uint32_t id)
{
	struct ivi_compositor *ivi = data;
	struct wl_resource *resource;
	struct ivi_policy *policy;
	void *interface;

	policy = ivi->policy;
	interface = (void *) &agl_shell_interface;
	if (policy && policy->api.shell_bind_interface &&
	    !policy->api.shell_bind_interface(client, interface)) {
		wl_client_post_implementation_error(client,
				       "client not authorized to use agl_shell");
		return;
	}

	resource = wl_resource_create(client, &agl_shell_interface,
				      1, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

#if 0
	if (ivi->shell_client.client != client) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "client not authorized to use agl_shell");
		return;
	}
#endif

	if (ivi->shell_client.resource) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "agl_shell has already been bound");
		return;
	}

	wl_resource_set_implementation(resource, &agl_shell_implementation,
				       ivi, unbind_agl_shell);
	ivi->shell_client.resource = resource;
}

static void
unbind_agl_shell_desktop(struct wl_resource *resource)
{
	struct desktop_client *dclient = wl_resource_get_user_data(resource);

	wl_list_remove(&dclient->link);
	free(dclient);
}

static void
bind_agl_shell_desktop(struct wl_client *client,
		       void *data, uint32_t version, uint32_t id)
{
	struct ivi_compositor *ivi = data;
	struct wl_resource *resource;
	struct ivi_policy *policy;
	struct desktop_client *dclient;
	void *interface;

	policy = ivi->policy;
	interface  = (void *) &agl_shell_desktop_interface;
	if (policy && policy->api.shell_bind_interface &&
	    !policy->api.shell_bind_interface(client, interface)) {
		wl_client_post_implementation_error(client,
				"client not authorized to use agl_shell_desktop");
		return;
	}

	dclient = zalloc(sizeof(*dclient));
	if (!dclient) {
		wl_client_post_no_memory(client);
		return;
	}

	resource = wl_resource_create(client, &agl_shell_desktop_interface,
				      version, id);
	dclient->ivi = ivi;
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &agl_shell_desktop_implementation,
				       dclient, unbind_agl_shell_desktop);

	dclient->resource = resource;
	wl_list_insert(&ivi->desktop_clients, &dclient->link);

	/* advertise xdg surfaces */
	ivi_shell_advertise_xdg_surfaces(ivi, resource);
}

int
ivi_shell_create_global(struct ivi_compositor *ivi)
{
	ivi->agl_shell = wl_global_create(ivi->compositor->wl_display,
					  &agl_shell_interface, 1,
					  ivi, bind_agl_shell);
	if (!ivi->agl_shell) {
		weston_log("Failed to create wayland global.\n");
		return -1;
	}

	ivi->agl_shell_desktop = wl_global_create(ivi->compositor->wl_display,
						  &agl_shell_desktop_interface, 1,
						  ivi, bind_agl_shell_desktop);
	if (!ivi->agl_shell_desktop) {
		weston_log("Failed to create wayland global (agl_shell_desktop).\n");
		return -1;
	}

	return 0;
}
