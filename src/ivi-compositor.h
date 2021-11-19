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

#ifndef IVI_COMPOSITOR_H
#define IVI_COMPOSITOR_H

#include <stdbool.h>
#include "config.h"

#include <libweston/backend-drm.h>
#include <libweston/libweston.h>
#include <libweston/windowed-output-api.h>
#include <libweston-desktop/libweston-desktop.h>

#include "remote.h"

#include "agl-shell-server-protocol.h"

struct ivi_compositor;

struct desktop_client {
	struct wl_resource *resource;
	struct ivi_compositor *ivi;
	struct wl_list link;	/* ivi_compositor::desktop_clients */
};

struct ivi_compositor {
	struct weston_compositor *compositor;
	struct weston_config *config;

	struct wl_listener heads_changed;

	bool init_failed;
	bool hide_cursor;
	bool activate_by_default;

	/*
	 * Options parsed from command line arugments.
	 * Overrides what is found in the config file.
	 */
	struct {
		/* drm */
		bool use_current_mode;
		/* wayland/x11 */
		int width;
		int height;
		int scale;
	} cmdline;
	const struct weston_windowed_output_api *window_api;
	const struct weston_drm_output_api *drm_api;
	const struct weston_remoting_api *remoting_api;
	const struct weston_transmitter_api *waltham_transmitter_api;

	struct wl_global *agl_shell;
	struct wl_global *agl_shell_desktop;

	struct {
		struct wl_client *client;
		struct wl_resource *resource;
		bool ready;
	} shell_client;

	struct wl_list desktop_clients;	/* desktop_client::link */

	struct wl_list outputs; /* ivi_output.link */
	struct wl_list surfaces; /* ivi_surface.link */

	struct weston_desktop *desktop;
	struct wl_listener seat_created_listener;
	struct ivi_policy *policy;

	struct wl_list pending_surfaces;
	struct wl_list popup_pending_apps;
	struct wl_list fullscreen_pending_apps;
	struct wl_list split_pending_apps;
	struct wl_list remote_pending_apps;

	struct weston_layer hidden;
	struct weston_layer background;
	struct weston_layer normal;
	struct weston_layer panel;
	struct weston_layer popup;
	struct weston_layer fullscreen;
};

struct ivi_surface;

enum ivi_output_type {
	OUTPUT_LOCAL,
	OUTPUT_REMOTE,
	/* same as remote but we need to signal the transmitter plug-in
	 * for the surfaces to have to be forwarded to those remoted outputs */
	OUTPUT_WALTHAM,
};

struct ivi_output {
	struct wl_list link; /* ivi_compositor.outputs */
	struct ivi_compositor *ivi;

	char *name;
	struct weston_config_section *config;
	struct weston_output *output;

	struct ivi_surface *background;
	/* Panels */
	struct ivi_surface *top;
	struct ivi_surface *bottom;
	struct ivi_surface *left;
	struct ivi_surface *right;

	/* for the black surface */
	struct fullscreen_view {
		struct ivi_surface *fs;
		struct wl_listener fs_destroy;
	} fullscreen_view;

	struct wl_listener output_destroy;

	/*
	 * Usable area for normal clients, i.e. with panels removed.
	 * In output-coorrdinate space.
	 */
	struct weston_geometry area;
	struct weston_geometry area_saved;

	struct ivi_surface *active;
	struct ivi_surface *previous_active;

	/* Temporary: only used during configuration */
	size_t add_len;
	struct weston_head *add[8];

	char *app_id;
	enum ivi_output_type type;
};

enum ivi_surface_role {
	IVI_SURFACE_ROLE_NONE,
	IVI_SURFACE_ROLE_DESKTOP,
	IVI_SURFACE_ROLE_BACKGROUND,
	IVI_SURFACE_ROLE_PANEL,
	IVI_SURFACE_ROLE_POPUP,
	IVI_SURFACE_ROLE_FULLSCREEN,
	IVI_SURFACE_ROLE_SPLIT_V,
	IVI_SURFACE_ROLE_SPLIT_H,
	IVI_SURFACE_ROLE_REMOTE,
};

struct ivi_bounding_box {
	int x; int y;
	int width; int height;
};

struct pending_popup {
	struct ivi_output *ioutput;
	char *app_id;
	int x; int y;
	struct ivi_bounding_box bb;

	struct wl_list link;	/** ivi_compositor::popup_pending_surfaces */
};

struct pending_fullscreen {
	struct ivi_output *ioutput;
	char *app_id;
	struct wl_list link;	/** ivi_compositor::fullscreen_pending_apps */
};

struct pending_split {
	struct ivi_output *ioutput;
	char *app_id;
	uint32_t orientation;
	struct wl_list link;	/** ivi_compositor::split_pending_apps */
};

struct pending_remote {
	struct ivi_output *ioutput;
	char *app_id;
	struct wl_list link;    /** ivi_compositor::remote_pending_apps */
};

struct ivi_desktop_surface {
	struct ivi_output *pending_output;
	struct ivi_output *last_output;
};

struct ivi_background_surface {
	struct ivi_output *output;
};

struct ivi_popup_surface {
	struct ivi_output *output;
	int x; int y; /* initial position */
	struct ivi_bounding_box bb;	/* bounding box */
};

struct ivi_fullscreen_surface {
	struct ivi_output *output;
};

struct ivi_split_surface {
	struct ivi_output *output;
	uint32_t orientation;
};

struct ivi_remote_surface {
	struct ivi_output *output;
};

struct ivi_panel_surface {
	struct ivi_output *output;
	enum agl_shell_edge edge;
};

enum ivi_surface_flags {
	IVI_SURFACE_PROP_MAP = (1 << 0),
	/* x, y, width, height */
	IVI_SURFACE_PROP_POSITION = (1 << 1),
};

/* the waltham surface is a pointer type as well and
 * in order to avoid adding ifdef for waltham use a
 * generic pointer, which will be only be valid when the
 * surface is a remote out on a waltham type of output */
struct ivi_surface_waltham {
	void *transmitter_surface;
};

struct ivi_surface {
	struct ivi_compositor *ivi;
	struct weston_desktop_surface *dsurface;
	struct weston_view *view;

	struct wl_list link;
	int focus_count;

	struct {
		enum ivi_surface_flags flags;
		int32_t x, y;
		int32_t width, height;
	} pending;
	bool activated_by_default;
	bool advertised_on_launch;
	bool checked_pending;

	enum ivi_surface_role role;
	union {
		struct ivi_desktop_surface desktop;
		struct ivi_background_surface bg;
		struct ivi_panel_surface panel;
		struct ivi_popup_surface popup;
		struct ivi_fullscreen_surface fullscreen;
		struct ivi_split_surface split;
		struct ivi_remote_surface remote;
	};

	struct ivi_surface_waltham waltham_surface;
	struct wl_listener listener_advertise_app;
	struct wl_signal signal_advertise_app;
};

struct ivi_shell_client {
	struct wl_list link;
	char *command;
	bool require_ready;

	pid_t pid;
	struct wl_client *client;

	struct wl_listener client_destroy;
};

struct ivi_compositor *
to_ivi_compositor(struct weston_compositor *ec);

#ifdef HAVE_SYSTEMD
int
ivi_agl_systemd_notify(struct ivi_compositor *ivi);
#else
static int
ivi_agl_systemd_notify(struct ivi_compositor *ivi)
{
}
#endif

int
ivi_shell_init(struct ivi_compositor *ivi);

void
ivi_shell_init_black_fs(struct ivi_compositor *ivi);

int
ivi_shell_create_global(struct ivi_compositor *ivi);

int
ivi_launch_shell_client(struct ivi_compositor *ivi);

int
ivi_desktop_init(struct ivi_compositor *ivi);

struct ivi_shell_client *
ivi_shell_client_from_wl(struct wl_client *client);

struct ivi_output *
to_ivi_output(struct weston_output *o);

void
ivi_set_desktop_surface(struct ivi_surface *surface);

/*
 * removes the pending popup one
 */
void
ivi_check_pending_desktop_surface(struct ivi_surface *surface);

void
ivi_reflow_outputs(struct ivi_compositor *ivi);

struct ivi_surface *
to_ivi_surface(struct weston_surface *surface);

void
ivi_layout_set_mapped(struct ivi_surface *surface);

void
ivi_layout_set_position(struct ivi_surface *surface,
			int32_t x, int32_t y,
			int32_t width, int32_t height);

struct ivi_surface *
ivi_find_app(struct ivi_compositor *ivi, const char *app_id);

void
ivi_layout_commit(struct ivi_compositor *ivi);

void
ivi_layout_init(struct ivi_compositor *ivi, struct ivi_output *output);

void
ivi_layout_activate(struct ivi_output *output, const char *app_id);

void
ivi_layout_activate_by_surf(struct ivi_output *output, struct ivi_surface *surf);

void
ivi_layout_desktop_committed(struct ivi_surface *surf);

void
ivi_layout_popup_committed(struct ivi_surface *surface);

void
ivi_layout_fullscreen_committed(struct ivi_surface *surface);

void
ivi_layout_split_committed(struct ivi_surface *surface);

void
ivi_layout_deactivate(struct ivi_compositor *ivi, const char *app_id);

void
ivi_layout_desktop_resize(struct ivi_surface *surface,
			  struct weston_geometry area);

struct ivi_output *
ivi_layout_get_output_from_surface(struct ivi_surface *surf);

void
insert_black_surface(struct ivi_output *output);

void
remove_black_surface(struct ivi_output *output);

const char *
ivi_layout_get_surface_role_name(struct ivi_surface *surf);

void
ivi_set_pending_desktop_surface_remote(struct ivi_output *ioutput,
		const char *app_id);

struct ivi_output *
ivi_layout_find_with_app_id(const char *app_id, struct ivi_compositor *ivi);

void
shell_advertise_app_state(struct ivi_compositor *ivi, const char *app_id,
			  const char *data, uint32_t app_state);
void
ivi_screenshooter_create(struct ivi_compositor *ivi);

void
ivi_seat_init(struct ivi_compositor *ivi);

void
ivi_seat_reset_caps_sent(struct ivi_compositor *ivi);

void
agl_shell_desktop_advertise_application_id(struct ivi_compositor *ivi,
					   struct ivi_surface *surface);
void
ivi_destroy_waltham_destroy(struct ivi_surface *surface);

bool
ivi_check_pending_surface(struct ivi_surface *surface);

#endif
