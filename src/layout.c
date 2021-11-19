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
#include "shared/helpers.h"

#include <assert.h>
#include <string.h>

#include <libweston/libweston.h>
#include <libweston-desktop/libweston-desktop.h>

#include "agl-shell-desktop-server-protocol.h"

#define AGL_COMP_DEBUG

static const char *ivi_roles_as_string[] = {
	[IVI_SURFACE_ROLE_NONE]		= "NONE",
	[IVI_SURFACE_ROLE_BACKGROUND]	= "BACKGROUND",
	[IVI_SURFACE_ROLE_PANEL]	= "PANEL",
	[IVI_SURFACE_ROLE_DESKTOP]	= "DESKTOP",
	[IVI_SURFACE_ROLE_POPUP]	= "POPUP",
	[IVI_SURFACE_ROLE_SPLIT_H]	= "SPLIT_H",
	[IVI_SURFACE_ROLE_SPLIT_V]	= "SPLIT_V",
	[IVI_SURFACE_ROLE_FULLSCREEN]	= "FULLSCREEN",
	[IVI_SURFACE_ROLE_REMOTE]	= "REMOTE",
};

const char *
ivi_layout_get_surface_role_name(struct ivi_surface *surf)
{
	if (surf->role < 0 || surf->role >= ARRAY_LENGTH(ivi_roles_as_string))
		return " unknown surface role";

	return ivi_roles_as_string[surf->role];
}

static void
ivi_background_init(struct ivi_compositor *ivi, struct ivi_output *output)
{
	struct weston_output *woutput = output->output;
	struct ivi_surface *bg = output->background;
	struct weston_view *view;

	if (!bg) {
		weston_log("WARNING: Output does not have a background\n");
		return;
	}

	assert(bg->role == IVI_SURFACE_ROLE_BACKGROUND);

	view = bg->view;

	weston_view_set_output(view, woutput);
	weston_view_set_position(view, woutput->x, woutput->y);

	weston_log("(background) position view %p, x %d, y %d, on output %s\n", view,
			woutput->x, woutput->y, output->name);

	view->is_mapped = true;
	view->surface->is_mapped = true;

	weston_layer_entry_insert(&ivi->background.view_list, &view->layer_link);
}

static void
ivi_panel_init(struct ivi_compositor *ivi, struct ivi_output *output,
	       struct ivi_surface *panel)
{
	struct weston_output *woutput = output->output;
	struct weston_desktop_surface *dsurface;
	struct weston_view *view;
	struct weston_geometry geom;
	int x = woutput->x;
	int y = woutput->y;

	if (!panel)
		return;

	assert(panel->role == IVI_SURFACE_ROLE_PANEL);
	dsurface = panel->dsurface;
	view = panel->view;
	geom = weston_desktop_surface_get_geometry(dsurface);

	weston_log("(panel) geom.width %d, geom.height %d, geom.x %d, geom.y %d\n",
			geom.width, geom.height, geom.x, geom.y);

	switch (panel->panel.edge) {
	case AGL_SHELL_EDGE_TOP:
		output->area.y += geom.height;
		output->area.height -= geom.height;
		break;
	case AGL_SHELL_EDGE_BOTTOM:
		y += woutput->height - geom.height;
		output->area.height -= geom.height;
		break;
	case AGL_SHELL_EDGE_LEFT:
		output->area.x += geom.width;
		output->area.width -= geom.width;
		break;
	case AGL_SHELL_EDGE_RIGHT:
		x += woutput->width - geom.width;
		output->area.width -= geom.width;
		break;
	}

	x -= geom.x;
	y -= geom.y;

	weston_view_set_output(view, woutput);
	weston_view_set_position(view, x, y);

	weston_log("(panel) edge %d position view %p, x %d, y %d\n",
			panel->panel.edge, view, x, y);

	view->is_mapped = true;
	view->surface->is_mapped = true;

	weston_log("panel type %d inited on output %s\n", panel->panel.edge,
			output->name);

	weston_layer_entry_insert(&ivi->panel.view_list, &view->layer_link);
}

/*
 * Initializes all static parts of the layout, i.e. the background and panels.
 */
void
ivi_layout_init(struct ivi_compositor *ivi, struct ivi_output *output)
{
	ivi_background_init(ivi, output);

	output->area.x = 0;
	output->area.y = 0;
	output->area.width = output->output->width;
	output->area.height = output->output->height;

	ivi_panel_init(ivi, output, output->top);
	ivi_panel_init(ivi, output, output->bottom);
	ivi_panel_init(ivi, output, output->left);
	ivi_panel_init(ivi, output, output->right);

	weston_compositor_schedule_repaint(ivi->compositor);

	weston_log("Usable area: %dx%d+%d,%d\n",
		   output->area.width, output->area.height,
		   output->area.x, output->area.y);
}

struct ivi_surface *
ivi_find_app(struct ivi_compositor *ivi, const char *app_id)
{
	struct ivi_surface *surf;
	const char *id;

	wl_list_for_each(surf, &ivi->surfaces, link) {
		id = weston_desktop_surface_get_app_id(surf->dsurface);
		if (id && strcmp(app_id, id) == 0)
			return surf;
	}

	return NULL;
}

static void
ivi_layout_activate_complete(struct ivi_output *output,
			     struct ivi_surface *surf)
{
	struct ivi_compositor *ivi = output->ivi;
	struct weston_output *woutput = output->output;
	struct weston_view *view = surf->view;

	if (weston_view_is_mapped(view)) {
		weston_layer_entry_remove(&view->layer_link);
	}

	weston_view_set_output(view, woutput);
	weston_view_set_position(view,
				 woutput->x + output->area.x,
				 woutput->y + output->area.y);

	view->is_mapped = true;
	view->surface->is_mapped = true;

	if (output->active) {
		output->active->view->is_mapped = false;
		output->active->view->surface->is_mapped = false;

		weston_layer_entry_remove(&output->active->view->layer_link);
	}
	output->previous_active = output->active;
	output->active = surf;

	weston_layer_entry_insert(&ivi->normal.view_list, &view->layer_link);
	weston_view_update_transform(view);

	weston_view_damage_below(view);

	/*
	 * the 'remote' role now makes use of this part so make sure we don't
	 * trip the enum such that we might end up with a modified output for
	 * 'remote' role
	 */
	if (surf->role == IVI_SURFACE_ROLE_DESKTOP) {
		if (surf->desktop.pending_output)
			surf->desktop.last_output = surf->desktop.pending_output;
		surf->desktop.pending_output = NULL;
	}

	weston_log("Activation completed for app_id %s, role %s, output %s\n",
			weston_desktop_surface_get_app_id(surf->dsurface),
			ivi_layout_get_surface_role_name(surf), output->name);
}

struct ivi_output *
ivi_layout_find_with_app_id(const char *app_id, struct ivi_compositor *ivi)
{
	struct ivi_output *out;

	if (!app_id)
		return NULL;

	wl_list_for_each(out, &ivi->outputs, link) {
		if (!out->app_id)
			continue;

		if (!strcmp(app_id, out->app_id))
			return out;
	}

	return NULL;
}


static struct ivi_output *
ivi_layout_find_bg_output(struct ivi_compositor *ivi)
{
	struct ivi_output *out;

	wl_list_for_each(out, &ivi->outputs, link) {
		if (out->background &&
		    out->background->role == IVI_SURFACE_ROLE_BACKGROUND)
			return out;
	}

	return NULL;
}

void
ivi_layout_desktop_committed(struct ivi_surface *surf)
{
	struct weston_desktop_surface *dsurf = surf->dsurface;
	struct weston_geometry geom = weston_desktop_surface_get_geometry(dsurf);
	struct ivi_policy *policy = surf->ivi->policy;
	struct ivi_output *output;
	const char *app_id = weston_desktop_surface_get_app_id(dsurf);

	assert(surf->role == IVI_SURFACE_ROLE_DESKTOP ||
	       surf->role == IVI_SURFACE_ROLE_REMOTE);

	/*
	 * we can't make use here of the ivi_layout_get_output_from_surface()
	 * due to the fact that we'll always land here when a surface performs
	 * a commit and pending_output will not bet set. This works in tandem
	 * with 'activated_by_default' at this point to avoid tripping over
	 * to a surface that continuously updates its content
	 */
	if (surf->role == IVI_SURFACE_ROLE_DESKTOP)
		output = surf->desktop.pending_output;
	else
		output = surf->remote.output;

	if (surf->role == IVI_SURFACE_ROLE_DESKTOP && !output) {
		struct ivi_output *r_output;

		if (policy && policy->api.surface_activate_by_default &&
		    !policy->api.surface_activate_by_default(surf, surf->ivi))
			return;

		/* we can only activate it again by using the protocol */
		if (surf->activated_by_default)
			return;

		/* check first if there aren't any outputs being set */
		r_output = ivi_layout_find_with_app_id(app_id, surf->ivi);

		if (r_output) {
			struct weston_view *view = r_output->fullscreen_view.fs->view;
			if (view->is_mapped || view->surface->is_mapped)
				remove_black_surface(r_output);
		}


		/* try finding an output with a background and use that */
		if (!r_output)
			r_output = ivi_layout_find_bg_output(surf->ivi);

		/* if we couldn't still find an output by this point, there's
		 * something wrong so we abort with a protocol error */
		if (!r_output) {
			wl_resource_post_error(surf->ivi->shell_client.resource,
					       AGL_SHELL_ERROR_INVALID_ARGUMENT,
					       "No valid output found to activate surface by default");
			return;
		}

		if (!surf->ivi->activate_by_default) {
			weston_log("Refusing to activate surface role %d, app_id %s\n",
					surf->role, app_id);
			return;
		}

		/* use the output of the bg to activate the app on start-up by
		 * default */
		if (surf->view && r_output) {
			if (app_id && r_output) {
				weston_log("Surface with app_id %s, role %s activating by default\n",
					weston_desktop_surface_get_app_id(surf->dsurface),
					ivi_layout_get_surface_role_name(surf));
				ivi_layout_activate(r_output, app_id);
				surf->activated_by_default = true;
			} else if (!app_id) {
				/*
				 * applications not setting an app_id, or
				 * setting an app_id but at a later point in
				 * time, might fall-back here so give them a
				 * chance to receive the configure event and
				 * act upon it
				 */
				weston_log("Surface no app_id, role %s activating by default\n",
					ivi_layout_get_surface_role_name(surf));
				ivi_layout_activate_by_surf(r_output, surf);
				surf->activated_by_default = true;
			}
		}

		return;
	}

	if (surf->role == IVI_SURFACE_ROLE_REMOTE && output) {
		if (policy && policy->api.surface_activate_by_default &&
		    !policy->api.surface_activate_by_default(surf, surf->ivi))
			return;

		/* we can only activate it again by using the protocol, but
		 * additionally the output is not reset when
		 * ivi_layout_activate_complete() terminates so we use the
		 * current active surface to avoid hitting this again and again
		 * */
		if (surf->activated_by_default && output->active == surf)
			return;

		if (app_id) {
			weston_log("Surface with app_id %s, role %s activating by default\n",
					weston_desktop_surface_get_app_id(surf->dsurface),
					ivi_layout_get_surface_role_name(surf));
			ivi_layout_activate(output, app_id);
			surf->activated_by_default = true;
		}
		return;
	}

	if (!weston_desktop_surface_get_maximized(dsurf) ||
	    geom.width != output->area.width ||
	    geom.height != output->area.height)
		return;

	ivi_layout_activate_complete(output, surf);
}

void
ivi_layout_fullscreen_committed(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	struct ivi_policy *policy = ivi->policy;

	struct weston_desktop_surface *dsurface = surface->dsurface;
	struct weston_surface *wsurface =
		weston_desktop_surface_get_surface(dsurface);
	const char *app_id = weston_desktop_surface_get_app_id(dsurface);

	struct ivi_output *output = surface->split.output;
	struct weston_output *woutput = output->output;

	struct weston_view *view = surface->view;
	struct weston_geometry geom;

	if (policy && policy->api.surface_activate_by_default &&
	    !policy->api.surface_activate_by_default(surface, surface->ivi) &&
	    !surface->activated_by_default)
		return;

	if (surface->view->is_mapped)
		return;

	geom = weston_desktop_surface_get_geometry(dsurface);
	weston_log("(fs) geom x %d, y %d, width %d, height %d\n", geom.x, geom.y,
			geom.width, geom.height);

	assert(surface->role == IVI_SURFACE_ROLE_FULLSCREEN);

	weston_desktop_surface_set_fullscreen(dsurface, true);

	weston_view_set_output(view, woutput);
	weston_view_set_position(view, woutput->x, woutput->y);
	weston_layer_entry_insert(&ivi->fullscreen.view_list, &view->layer_link);

	weston_view_update_transform(view);
	weston_view_damage_below(view);

	wsurface->is_mapped = true;
	surface->view->is_mapped = true;

	shell_advertise_app_state(ivi, app_id,
				  NULL, AGL_SHELL_DESKTOP_APP_STATE_ACTIVATED);

	weston_log("Activation completed for app_id %s, role %s, output %s\n",
			app_id, ivi_layout_get_surface_role_name(surface), output->name);
}

void
ivi_layout_desktop_resize(struct ivi_surface *surface,
			  struct weston_geometry area)
{
	struct weston_desktop_surface *dsurf = surface->dsurface;
	struct weston_view *view = surface->view;

	int x = area.x;
	int y = area.y;
	int width = area.width;
	int height = area.height;

	weston_desktop_surface_set_size(dsurf,
					width, height);

	weston_view_set_position(view, x, y);
	weston_view_update_transform(view);
	weston_view_damage_below(view);
}

void
ivi_layout_split_committed(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	struct ivi_policy *policy = ivi->policy;

	struct weston_desktop_surface *dsurface = surface->dsurface;
	struct weston_surface *wsurface =
		weston_desktop_surface_get_surface(dsurface);
	const char *app_id = weston_desktop_surface_get_app_id(dsurface);

	struct ivi_output *output = surface->split.output;
	struct weston_output *woutput = output->output;

	struct weston_view *view = surface->view;
	struct weston_geometry geom;

	int x, y;
	int width, height;

	x = woutput->x;
	y = woutput->y;

	if (policy && policy->api.surface_activate_by_default &&
	    !policy->api.surface_activate_by_default(surface, surface->ivi) &&
	    !surface->activated_by_default)
		return;

	if (surface->view->is_mapped)
		return;

	geom = weston_desktop_surface_get_geometry(dsurface);

	assert(surface->role == IVI_SURFACE_ROLE_SPLIT_H ||
	       surface->role == IVI_SURFACE_ROLE_SPLIT_V);

	/* save the previous area in order to recover it back when if this kind
	 * of surface is being destroyed/removed */
	output->area_saved = output->area;

	switch (surface->role) {
	case IVI_SURFACE_ROLE_SPLIT_V:
		geom.width = (output->area.width / 2);

		x += woutput->width - geom.width;
		output->area.width -= geom.width;

		width = woutput->width - x;
		height = output->area.height;
		y = output->area.y;

		break;
	case IVI_SURFACE_ROLE_SPLIT_H:
		geom.height = (output->area.height / 2);

		y = output->area.y;
		output->area.y += geom.height;
		output->area.height -= geom.height;

		width = output->area.width;
		height = output->area.height;

		x = output->area.x;

		break;
	default:
		assert(!"Invalid split orientation\n");
	}

	weston_desktop_surface_set_size(dsurface,
					width, height);

	/* resize the active surface first, output->area already contains
	 * correct area to resize to */
	if (output->active)
		ivi_layout_desktop_resize(output->active, output->area);

	weston_view_set_output(view, woutput);
	weston_view_set_position(view, x, y);
	weston_layer_entry_insert(&ivi->normal.view_list, &view->layer_link);

	weston_view_update_transform(view);
	weston_view_damage_below(view);

	wsurface->is_mapped = true;
	surface->view->is_mapped = true;

	shell_advertise_app_state(ivi, app_id,
				  NULL, AGL_SHELL_DESKTOP_APP_STATE_ACTIVATED);

	weston_log("Activation completed for app_id %s, role %s, output %s\n",
			app_id, ivi_layout_get_surface_role_name(surface), output->name);
}

static void
ivi_compute_popup_position(const struct weston_output *output, struct weston_view *view,
			   int initial_x, int initial_y, int *new_x, int *new_y)
{
	*new_x = output->x + initial_x;
	*new_y = output->y + initial_y;
}


void
ivi_layout_popup_committed(struct ivi_surface *surface)
{
	struct ivi_compositor *ivi = surface->ivi;
	struct ivi_policy *policy = ivi->policy;

	struct weston_desktop_surface *dsurface = surface->dsurface;
	struct weston_surface *wsurface =
		weston_desktop_surface_get_surface(dsurface);
	const char *app_id = weston_desktop_surface_get_app_id(dsurface);

	int new_x, new_y;

	struct ivi_output *output = surface->popup.output;
	struct weston_output *woutput = output->output;

	struct weston_view *view = surface->view;

	if (policy && policy->api.surface_activate_by_default &&
	    !policy->api.surface_activate_by_default(surface, surface->ivi) &&
	    !surface->activated_by_default)
		return;

	if (surface->view->is_mapped)
		return;

	assert(surface->role == IVI_SURFACE_ROLE_POPUP);

	weston_view_set_output(view, woutput);

	ivi_compute_popup_position(woutput, view,
				   surface->popup.x, surface->popup.y, &new_x, &new_y);
	weston_view_set_position(view, new_x, new_y);

	/* only clip the pop-up dialog window if we have a valid
	 * width and height being passed on. Users might not want to have one
	 * set-up so only enfore it is really passed on. */
	if (surface->popup.bb.width > 0 && surface->popup.bb.height > 0)
		weston_view_set_mask(view, surface->popup.bb.x, surface->popup.bb.y,
				     surface->popup.bb.width, surface->popup.bb.height);

	weston_layer_entry_insert(&ivi->popup.view_list, &view->layer_link);

	weston_view_update_transform(view);
	weston_view_damage_below(view);

	wsurface->is_mapped = true;
	surface->view->is_mapped = true;

	shell_advertise_app_state(ivi, app_id,
				  NULL, AGL_SHELL_DESKTOP_APP_STATE_ACTIVATED);

	weston_log("Activation completed for app_id %s, role %s, output %s\n",
			app_id, ivi_layout_get_surface_role_name(surface), output->name);
}

static void
ivi_layout_popup_re_add(struct ivi_surface *surface)
{
	assert(surface->role == IVI_SURFACE_ROLE_POPUP);
	struct weston_view *view = surface->view;

	if (weston_view_is_mapped(view)) {
		struct weston_desktop_surface *dsurface = surface->dsurface;
		struct weston_surface *wsurface =
			weston_desktop_surface_get_surface(dsurface);

		weston_layer_entry_remove(&view->layer_link);

		wsurface->is_mapped = false;
		view->is_mapped = false;
	}

	/* reset the activate by default in order to (still) allow the surface
	 * to be activaved using the request */
	if (!surface->activated_by_default)
		surface->activated_by_default = true;

	ivi_layout_popup_committed(surface);
}

static bool
ivi_layout_surface_is_split_or_fullscreen(struct ivi_surface *surf)
{
	struct ivi_compositor *ivi = surf->ivi;
	struct ivi_surface *is;

	if (surf->role != IVI_SURFACE_ROLE_SPLIT_H &&
	    surf->role != IVI_SURFACE_ROLE_SPLIT_V &&
	    surf->role != IVI_SURFACE_ROLE_FULLSCREEN)
		return false;

	/* reset the activate by default in order to (still) allow the surface
	 * to be activaved using the request */
	if (!surf->activated_by_default)
		surf->activated_by_default = true;

	wl_list_for_each(is, &ivi->surfaces, link)
		if (is == surf)
			return true;

	return false;
}

void
ivi_layout_activate_by_surf(struct ivi_output *output, struct ivi_surface *surf)
{
	struct ivi_compositor *ivi = output->ivi;
	struct weston_desktop_surface *dsurf;
	struct weston_view *view;
	struct weston_geometry geom;
	struct ivi_policy *policy = output->ivi->policy;

	dsurf = surf->dsurface;
	view = surf->view;

	const char *app_id = weston_desktop_surface_get_app_id(dsurf);

	if (!surf)
		return;

	if (policy && policy->api.surface_activate &&
	    !policy->api.surface_activate(surf, surf->ivi)) {
		return;
	}

#ifdef AGL_COMP_DEBUG
	weston_log("Activating app_id %s, type %s\n", app_id,
			ivi_layout_get_surface_role_name(surf));
#endif

	if (surf->role == IVI_SURFACE_ROLE_POPUP) {
		ivi_layout_popup_re_add(surf);
		return;
	}

	/* do not 're'-activate surfaces that are split or active */
	if (surf == output->active ||
	    ivi_layout_surface_is_split_or_fullscreen(surf))
		return;

	if (surf->role == IVI_SURFACE_ROLE_REMOTE) {
		struct ivi_output *remote_output =
			ivi_layout_find_with_app_id(app_id, ivi);

		/* if already active on a remote output do not
		 * attempt to activate it again */
		if (remote_output && remote_output->active == surf)
			return;
	}


	geom = weston_desktop_surface_get_geometry(dsurf);

	if (surf->role == IVI_SURFACE_ROLE_DESKTOP)
		surf->desktop.pending_output = output;
	if (weston_desktop_surface_get_maximized(dsurf) &&
	    geom.width == output->area.width &&
	    geom.height == output->area.height) {
		ivi_layout_activate_complete(output, surf);
		return;
	}

	weston_desktop_surface_set_maximized(dsurf, true);
	weston_desktop_surface_set_size(dsurf,
					output->area.width,
					output->area.height);

	weston_log("Setting app_id %s, role %s, set to maximized (%dx%d)\n",
			app_id, ivi_layout_get_surface_role_name(surf),
			output->area.width, output->area.height);
	/*
	 * If the view isn't mapped, we put it onto the hidden layer so it will
	 * start receiving frame events, and will be able to act on our
	 * configure event.
	 */
	if (!weston_view_is_mapped(view)) {
		view->is_mapped = true;
		view->surface->is_mapped = true;

		weston_view_set_output(view, output->output);
		weston_layer_entry_insert(&ivi->hidden.view_list, &view->layer_link);
		/* force repaint of the entire output */

		weston_log("Placed app_id %s, type %s in hidden layer\n",
				app_id, ivi_layout_get_surface_role_name(surf));
	}
}

void
ivi_layout_activate(struct ivi_output *output, const char *app_id)
{
	struct ivi_surface *surf;
	struct ivi_compositor *ivi = output->ivi;

	if (!app_id)
		return;

	surf = ivi_find_app(ivi, app_id);
	if (!surf)
		return;

	ivi_layout_activate_by_surf(output, surf);
}

struct ivi_output *
ivi_layout_get_output_from_surface(struct ivi_surface *surf)
{
	struct ivi_output *ivi_output = NULL;

	switch (surf->role) {
	case IVI_SURFACE_ROLE_DESKTOP:
		if (surf->desktop.pending_output)
			ivi_output = surf->desktop.pending_output;
		else
			ivi_output = surf->desktop.last_output;
		break;
	case IVI_SURFACE_ROLE_POPUP:
		ivi_output = surf->popup.output;
		break;
	case IVI_SURFACE_ROLE_BACKGROUND:
		ivi_output = surf->bg.output;
		break;
	case IVI_SURFACE_ROLE_PANEL:
		ivi_output = surf->panel.output;
		break;
	case IVI_SURFACE_ROLE_FULLSCREEN:
		ivi_output = surf->fullscreen.output;
		break;
	case IVI_SURFACE_ROLE_SPLIT_H:
	case IVI_SURFACE_ROLE_SPLIT_V:
		ivi_output = surf->split.output;
		break;
	case IVI_SURFACE_ROLE_REMOTE:
		ivi_output = surf->remote.output;
		break;
	case IVI_SURFACE_ROLE_NONE:
	default:
		break;
	}

	return ivi_output;
}

void
ivi_layout_deactivate(struct ivi_compositor *ivi, const char *app_id)
{
	struct ivi_surface *surf;
	struct ivi_output *ivi_output;
	struct ivi_policy *policy = ivi->policy;

	if (!app_id)
		return;

	surf = ivi_find_app(ivi, app_id);
	if (!surf)
		return;

	if (policy && policy->api.surface_deactivate &&
	    !policy->api.surface_deactivate(surf, surf->ivi)) {
		return;
	}

	ivi_output = ivi_layout_get_output_from_surface(surf);
	weston_log("Deactiving %s, role %s\n", app_id,
			ivi_layout_get_surface_role_name(surf));

	if (surf->role == IVI_SURFACE_ROLE_DESKTOP) {
		struct ivi_surface *previous_active;

		previous_active = ivi_output->previous_active;
		if (!previous_active) {
			/* we don't have a previous active it means we should
			 * display the bg */
			if (ivi_output->active) {
				struct weston_view *view;

				view = ivi_output->active->view;
				view->is_mapped = false;
				view->surface->is_mapped = false;

				weston_layer_entry_remove(&view->layer_link);
				weston_view_damage_below(view);
				ivi_output->active = NULL;
			}
		} else {
			struct weston_desktop_surface *dsurface;
			const char *previous_active_app_id;

			dsurface = previous_active->dsurface;
			previous_active_app_id =
				weston_desktop_surface_get_app_id(dsurface);
			ivi_layout_activate(ivi_output, previous_active_app_id);
		}
	} else if (surf->role == IVI_SURFACE_ROLE_POPUP) {
		struct weston_view *view  = surf->view;

		weston_layer_entry_remove(&view->layer_link);
		weston_view_damage_below(view);
	}
}
