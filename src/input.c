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

#include <assert.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ivi-compositor.h"
#include "shared/helpers.h"

struct ivi_shell_seat {
	struct weston_seat *seat;
	struct weston_surface *focused_surface;

	bool hide_cursor;
	bool new_caps_sent;

	struct wl_listener seat_destroy_listener;
	struct wl_listener caps_changed_listener;
	struct wl_listener keyboard_focus_listener;
	struct wl_listener pointer_focus_listener;
};

static struct ivi_surface *
get_ivi_shell_surface(struct weston_surface *surface)
{
	struct weston_desktop_surface *desktop_surface =
		weston_surface_get_desktop_surface(surface);

	if (desktop_surface)
		return weston_desktop_surface_get_user_data(desktop_surface);

	return NULL;
}

static void
ivi_shell_seat_handle_destroy(struct wl_listener *listener, void *data)
{
	struct ivi_shell_seat *shseat =
		container_of(listener,
			     struct ivi_shell_seat, seat_destroy_listener);

	wl_list_remove(&shseat->keyboard_focus_listener.link);
	wl_list_remove(&shseat->caps_changed_listener.link);
	wl_list_remove(&shseat->pointer_focus_listener.link);
	wl_list_remove(&shseat->seat_destroy_listener.link);

	free(shseat);
}

static struct ivi_shell_seat *
get_ivi_shell_seat(struct weston_seat *seat)
{
	struct wl_listener *listener;

	listener = wl_signal_get(&seat->destroy_signal,
				 ivi_shell_seat_handle_destroy);
	assert(listener != NULL);

	return container_of(listener,
			    struct ivi_shell_seat, seat_destroy_listener);
}


static void
ivi_shell_seat_handle_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard = data;
	struct ivi_shell_seat *shseat = get_ivi_shell_seat(keyboard->seat);

	if (shseat->focused_surface) {
		struct ivi_surface *surf =
			get_ivi_shell_surface(shseat->focused_surface);
		if (surf && --surf->focus_count == 0)
			weston_desktop_surface_set_activated(surf->dsurface, false);
	}

	shseat->focused_surface = weston_surface_get_main_surface(keyboard->focus);

	if (shseat->focused_surface) {
		struct ivi_surface *surf =
			get_ivi_shell_surface(shseat->focused_surface);
		if (surf && surf->focus_count++ == 0)
			weston_desktop_surface_set_activated(surf->dsurface, true);
	}
}

static void
ivi_shell_seat_handle_pointer_focus(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer = data;
	struct ivi_shell_seat *shseat = get_ivi_shell_seat(pointer->seat);
	struct wl_resource *resource;
	int resources = 0;

	/* FIXME: should probably query it and not assume all caps */
	uint32_t caps = (WL_SEAT_CAPABILITY_POINTER |
			 WL_SEAT_CAPABILITY_TOUCH |
			 WL_SEAT_CAPABILITY_KEYBOARD);

	wl_resource_for_each(resource, &pointer->seat->base_resource_list)
		resources++;

	/* remove the POINTER capability such that the client will not install
	 * a cursor surface */
	if (shseat->hide_cursor && !shseat->new_caps_sent && resources) {
		caps &= ~WL_SEAT_CAPABILITY_POINTER;
		wl_resource_for_each(resource, &pointer->seat->base_resource_list) {
			wl_seat_send_capabilities(resource, caps);
		}
		shseat->new_caps_sent = true;
	}
}

static void
ivi_shell_seat_handle_caps_changed(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard;
	struct weston_pointer *pointer;
	struct ivi_shell_seat *shseat;

	shseat = container_of(listener, struct ivi_shell_seat,
			      caps_changed_listener);
	keyboard = weston_seat_get_keyboard(shseat->seat);
	pointer = weston_seat_get_pointer(shseat->seat);

	if (pointer && wl_list_empty(&shseat->pointer_focus_listener.link)) {
		wl_signal_add(&pointer->focus_signal,
			      &shseat->pointer_focus_listener);
	} else {
		wl_list_remove(&shseat->pointer_focus_listener.link);
		wl_list_init(&shseat->pointer_focus_listener.link);
	}

	if (keyboard &&
	    wl_list_empty(&shseat->keyboard_focus_listener.link)) {
		wl_signal_add(&keyboard->focus_signal,
			      &shseat->keyboard_focus_listener);
	} else if (!keyboard) {
		wl_list_remove(&shseat->keyboard_focus_listener.link);
		wl_list_init(&shseat->keyboard_focus_listener.link);
	}
}

static struct ivi_shell_seat *
ivi_shell_seat_create(struct weston_seat *seat, bool hide_cursor)
{
	struct ivi_shell_seat *shseat;

	shseat = zalloc(sizeof(*shseat));
	if (!shseat) {
		weston_log("no memory to allocate shell seat\n");
		return NULL;
	}

	shseat->seat = seat;
	shseat->hide_cursor = hide_cursor;
	shseat->new_caps_sent = false;

	shseat->seat_destroy_listener.notify = ivi_shell_seat_handle_destroy;
	wl_signal_add(&seat->destroy_signal, &shseat->seat_destroy_listener);

	shseat->keyboard_focus_listener.notify =
		ivi_shell_seat_handle_keyboard_focus;
	wl_list_init(&shseat->keyboard_focus_listener.link);

	shseat->pointer_focus_listener.notify =
		ivi_shell_seat_handle_pointer_focus;
	wl_list_init(&shseat->pointer_focus_listener.link);

	shseat->caps_changed_listener.notify =
		ivi_shell_seat_handle_caps_changed;
	wl_signal_add(&seat->updated_caps_signal, &shseat->caps_changed_listener);

	ivi_shell_seat_handle_caps_changed(&shseat->caps_changed_listener, NULL);

	return shseat;
}


static void
ivi_shell_handle_seat_created(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	struct ivi_compositor *ivi =
		container_of(listener, struct ivi_compositor, seat_created_listener);

	weston_log("Cursor is %s\n", ivi->hide_cursor ? "set" : "not set");
	ivi_shell_seat_create(seat, ivi->hide_cursor);
}

/*
 * useful to reset the fact that 'new' capabilities have seent
 */
void
ivi_seat_reset_caps_sent(struct ivi_compositor *ivi)
{
	struct weston_compositor *ec = ivi->compositor;
	struct weston_seat *seat;

	wl_list_for_each(seat, &ec->seat_list, link) {
		struct ivi_shell_seat *ivi_seat = get_ivi_shell_seat(seat);
		ivi_seat->new_caps_sent = false;
	}
}

void
ivi_seat_init(struct ivi_compositor *ivi)
{
	struct weston_compositor *ec = ivi->compositor;
	struct weston_seat *seat;

	wl_list_for_each(seat, &ec->seat_list, link) {
		weston_log("Seat %p, cursor is %s\n", seat, ivi->hide_cursor ?
				"set" : "not set");
		ivi_shell_seat_create(seat, ivi->hide_cursor);
	}

	ivi->seat_created_listener.notify = ivi_shell_handle_seat_created;
	wl_signal_add(&ec->seat_created_signal, &ivi->seat_created_listener);
}
