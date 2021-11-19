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
#include "policy.h"

#ifdef HAVE_SMACK
#include <sys/smack.h>
#endif

#include <string.h>
#include "shared/helpers.h"

#ifdef HAVE_SMACK
static const char *const bind_agl_shell[] = {
	"User::App::homescreen",
	"User::App::cluster-gauges"	/* cluster-dashboard */
};

static const char *const bind_agl_shell_desktop[] = {
	"User::App::launcher",
	"User::App::alexa-viewer",
	"User::App::tbtnavi",
	"User::App::hvac",
	"User::App::xdg-cluster-receiver",		/* cluster-receiver, native XDG app*/
	"User::App::cluster-receiver"			/* cluster-receiver, Qt app  */
};
#endif

static const char *const applications_permitted[] = {
	"homescreen", "alexa-viewer", "launcher", "hvac",
	"navigation", "mediaplayer"
};

/* helper start searches the applications_permitted for the
 * app_id
 */
static bool
ivi_policy_verify_permitted_app(const char *app_id)
{
	for (size_t i = 0; i < ARRAY_LENGTH(applications_permitted); i++)
		if (strcmp(app_id, applications_permitted[i]) == 0)
			return true;

	return false;
}

#ifdef HAVE_SMACK
/* helper to determine which applications are allowed to bind to the
 * private extensions
 */
static bool
ivi_policy_check_bind_agl_shell(const char *app_id)
{
	for (size_t i = 0; i < ARRAY_LENGTH(bind_agl_shell); i++)
		if (strcmp(app_id, bind_agl_shell[i]) == 0)
			return true;

	return false;
}

static bool
ivi_policy_check_bind_agl_shell_desktop(const char *app_id)
{
	for (size_t i = 0; i < ARRAY_LENGTH(bind_agl_shell_desktop); i++)
		if (strcmp(app_id, bind_agl_shell_desktop[i]) == 0)
			return true;

	return false;
}
#endif

static bool
ivi_policy_verify_ivi_surface(struct ivi_surface *surf)
{
	const char *app_id = weston_desktop_surface_get_app_id(surf->dsurface);
	return ivi_policy_verify_permitted_app(app_id);
}

/*
 * deny-all policy implementation allows every action to be denied
 *
 * This is an example, that implements the API
 *
 * For injecting rules back in the compositor one should use ivi_policy_add()
 * - policy_rule_allow_to_add is required in order to add further policy rules
 * - policy_rule_try_event will be callback executed when handling the state
 *   change
 */
static bool
ivi_policy_default_surface_create(struct ivi_surface *surf, void *user_data)
{
	return ivi_policy_verify_ivi_surface(surf);
}

static bool
ivi_policy_default_surface_commmited(struct ivi_surface *surf, void *user_data)
{
	return ivi_policy_verify_ivi_surface(surf);
}

static bool
ivi_policy_default_surface_activate(struct ivi_surface *surf, void *user_data)
{
	return ivi_policy_verify_ivi_surface(surf);
}

static bool
ivi_policy_default_surface_deactivate(struct ivi_surface *surf, void *user_data)
{
	return ivi_policy_verify_ivi_surface(surf);
}

static bool
ivi_policy_default_surface_activate_default(struct ivi_surface *surf, void *user_data)
{
	return ivi_policy_verify_ivi_surface(surf);
}

static bool
ivi_policy_default_surface_advertise_state_change(struct ivi_surface *surf, void *user_data)
{
	return ivi_policy_verify_ivi_surface(surf);
}

#ifdef HAVE_SMACK
static bool
ivi_policy_default_shell_bind_interface(void *client, void *interface)
{
	struct wl_interface *shell_interface = interface;
	struct wl_client *conn_client = client;

	pid_t pid, uid, gid;
	int client_fd;
	char *label;
	bool ret = false;

	wl_client_get_credentials(conn_client, &pid, &uid, &gid);

	client_fd = wl_client_get_fd(conn_client);
	if (smack_new_label_from_socket(client_fd, &label) < 0) {
		return ret;
	}

	if (strcmp(shell_interface->name, "agl_shell") == 0)
		ret = ivi_policy_check_bind_agl_shell(label);

	if (strcmp(shell_interface->name, "agl_shell_desktop") == 0)
		ret = ivi_policy_check_bind_agl_shell_desktop(label);

	if (ret)
		weston_log("Client with pid %d, uid %d, gid %d, allowed "
				"to bind to %s for label %s\n", pid, uid, gid,
				shell_interface->name, label);

	/* client responsible for free'ing */
	free(label);
	return ret;
}
#else
static bool
ivi_policy_default_shell_bind_interface(void *client, void *interface)
{
	return false;
}
#endif

static bool
ivi_policy_default_allow_to_add(void *user_data)
{
	/* verify that policy rules can be added with ivi_policy_add() */
	return true;
}

/*
 * Policy rules added by ivi_policy_add() will be handled by this callback, and
 * should be treated depending on the event. Note this is just an example.
 */
static void
ivi_policy_default_try_event(struct ivi_a_policy *a_policy)
{
	uint32_t event = a_policy->event;

	switch (event) {
	case AGL_SHELL_POLICY_EVENT_SHOW:
		ivi_layout_activate(a_policy->output, a_policy->app_id);
		break;
	case AGL_SHELL_POLICY_EVENT_HIDE:
		ivi_layout_deactivate(a_policy->policy->ivi, a_policy->app_id);
	default:
		break;
	}
}

static const struct ivi_policy_api policy_api = {
	.struct_size = sizeof(policy_api),
	.surface_create = ivi_policy_default_surface_create,
	.surface_commited = ivi_policy_default_surface_commmited,
	.surface_activate = ivi_policy_default_surface_activate,
	.surface_deactivate = ivi_policy_default_surface_deactivate,
	.surface_activate_by_default = ivi_policy_default_surface_activate_default,
	.surface_advertise_state_change = ivi_policy_default_surface_advertise_state_change,
	.shell_bind_interface = ivi_policy_default_shell_bind_interface,
	.policy_rule_allow_to_add = ivi_policy_default_allow_to_add,
	.policy_rule_try_event = ivi_policy_default_try_event,
};

int
ivi_policy_init(struct ivi_compositor *ivi)
{
	ivi->policy = ivi_policy_create(ivi, &policy_api, ivi);
	if (!ivi->policy)
		return -1;

	weston_log("Installing 'deny-all' policy engine\n");
	return 0;
}
