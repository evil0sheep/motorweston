/*
 * Copyright © 2012 Openismus GmbH
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "compositor.h"
#include "text-server-protocol.h"
#include "input-method-server-protocol.h"

struct input_method;
struct input_method_context;
struct text_backend;

struct text_input {
	struct wl_resource *resource;

	struct weston_compositor *ec;

	struct wl_list input_methods;

	struct weston_surface *surface;

	pixman_box32_t cursor_rectangle;

	uint32_t input_panel_visible;
};

struct text_input_manager {
	struct wl_global *text_input_manager_global;
	struct wl_listener destroy_listener;

	struct weston_compositor *ec;
};

struct input_method {
	struct wl_resource *input_method_binding;
	struct wl_global *input_method_global;
	struct wl_listener destroy_listener;

	struct weston_seat *seat;
	struct text_input *model;

	struct wl_list link;

	struct wl_listener keyboard_focus_listener;

	int focus_listener_initialized;

	struct input_method_context *context;

	struct text_backend *text_backend;
};

struct input_method_context {
	struct wl_resource *resource;

	struct text_input *model;
	struct input_method *input_method;

	struct wl_list link;

	struct wl_resource *keyboard;
};

struct text_backend {
	struct weston_compositor *compositor;

	struct {
		char *path;
		struct wl_resource *binding;
		struct weston_process process;
		struct wl_client *client;

		unsigned deathcount;
		uint32_t deathstamp;
	} input_method;

	struct wl_listener seat_created_listener;
	struct wl_listener destroy_listener;
};

static void input_method_context_create(struct text_input *model,
					struct input_method *input_method);
static void input_method_context_end_keyboard_grab(struct input_method_context *context);

static void input_method_init_seat(struct weston_seat *seat);

static void
deactivate_text_input(struct text_input *text_input,
		      struct input_method *input_method)
{
	struct weston_compositor *ec = text_input->ec;

	if (input_method->model == text_input) {
		if (input_method->context && input_method->input_method_binding) {
			input_method_context_end_keyboard_grab(input_method->context);
			wl_input_method_send_deactivate(input_method->input_method_binding,
							input_method->context->resource);
		}

		wl_list_remove(&input_method->link);
		input_method->model = NULL;
		input_method->context = NULL;
		wl_signal_emit(&ec->hide_input_panel_signal, ec);
		wl_text_input_send_leave(text_input->resource);
	}
}

static void
destroy_text_input(struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next, &text_input->input_methods, link)
		deactivate_text_input(text_input, input_method);

	free(text_input);
}

static void
text_input_set_surrounding_text(struct wl_client *client,
				struct wl_resource *resource,
				const char *text,
				uint32_t cursor,
				uint32_t anchor)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next, &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		wl_input_method_context_send_surrounding_text(input_method->context->resource,
							      text,
							      cursor,
							      anchor);
	}
}

static void
text_input_activate(struct wl_client *client,
		    struct wl_resource *resource,
		    struct wl_resource *seat,
		    struct wl_resource *surface)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_seat *weston_seat = wl_resource_get_user_data(seat);
	struct input_method *input_method = weston_seat->input_method;
	struct text_input *old = weston_seat->input_method->model;
	struct weston_compositor *ec = text_input->ec;

	if (old == text_input)
		return;

	if (old) {
		deactivate_text_input(old,
				      weston_seat->input_method);
	}

	input_method->model = text_input;
	wl_list_insert(&text_input->input_methods, &input_method->link);
	input_method_init_seat(weston_seat);

	text_input->surface = wl_resource_get_user_data(surface);

	input_method_context_create(text_input, input_method);

	if (text_input->input_panel_visible) {
		wl_signal_emit(&ec->show_input_panel_signal, text_input->surface);
		wl_signal_emit(&ec->update_input_panel_signal, &text_input->cursor_rectangle);
	}

	wl_text_input_send_enter(text_input->resource, text_input->surface->resource);
}

static void
text_input_deactivate(struct wl_client *client,
		      struct wl_resource *resource,
		      struct wl_resource *seat)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_seat *weston_seat = wl_resource_get_user_data(seat);

	deactivate_text_input(text_input,
			      weston_seat->input_method);
}

static void
text_input_reset(struct wl_client *client,
		 struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next, &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		wl_input_method_context_send_reset(input_method->context->resource);
	}
}

static void
text_input_set_cursor_rectangle(struct wl_client *client,
				struct wl_resource *resource,
				int32_t x,
				int32_t y,
				int32_t width,
				int32_t height)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = text_input->ec;

	text_input->cursor_rectangle.x1 = x;
	text_input->cursor_rectangle.y1 = y;
	text_input->cursor_rectangle.x2 = x + width;
	text_input->cursor_rectangle.y2 = y + height;

	wl_signal_emit(&ec->update_input_panel_signal, &text_input->cursor_rectangle);
}

static void
text_input_set_content_type(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t hint,
			    uint32_t purpose)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next, &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		wl_input_method_context_send_content_type(input_method->context->resource, hint, purpose);
	}
}

static void
text_input_invoke_action(struct wl_client *client,
			 struct wl_resource *resource,
			 uint32_t button,
			 uint32_t index)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next, &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		wl_input_method_context_send_invoke_action(input_method->context->resource, button, index);
	}
}

static void
text_input_commit_state(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t serial)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next, &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		wl_input_method_context_send_commit_state(input_method->context->resource, serial);
	}
}

static void
text_input_show_input_panel(struct wl_client *client,
			    struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = text_input->ec;

	text_input->input_panel_visible = 1;

	if (!wl_list_empty(&text_input->input_methods)) {
		wl_signal_emit(&ec->show_input_panel_signal, text_input->surface);
		wl_signal_emit(&ec->update_input_panel_signal, &text_input->cursor_rectangle);
	}
}

static void
text_input_hide_input_panel(struct wl_client *client,
			    struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = text_input->ec;

	text_input->input_panel_visible = 0;

	if (!wl_list_empty(&text_input->input_methods))
		wl_signal_emit(&ec->hide_input_panel_signal, ec);
}

static void
text_input_set_preferred_language(struct wl_client *client,
				  struct wl_resource *resource,
				  const char *language)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next, &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		wl_input_method_context_send_preferred_language(input_method->context->resource,
								language);
	}
}

static const struct wl_text_input_interface text_input_implementation = {
	text_input_activate,
	text_input_deactivate,
	text_input_show_input_panel,
	text_input_hide_input_panel,
	text_input_reset,
	text_input_set_surrounding_text,
	text_input_set_content_type,
	text_input_set_cursor_rectangle,
	text_input_set_preferred_language,
	text_input_commit_state,
	text_input_invoke_action
};

static void text_input_manager_create_text_input(struct wl_client *client,
						 struct wl_resource *resource,
						 uint32_t id)
{
	struct text_input_manager *text_input_manager = wl_resource_get_user_data(resource);
	struct text_input *text_input;

	text_input = calloc(1, sizeof *text_input);

	text_input->resource =
		wl_resource_create(client, &wl_text_input_interface, 1, id);
	wl_resource_set_implementation(text_input->resource,
				       &text_input_implementation,
				       text_input, destroy_text_input);

	text_input->ec = text_input_manager->ec;

	wl_list_init(&text_input->input_methods);
};

static const struct wl_text_input_manager_interface text_input_manager_implementation = {
	text_input_manager_create_text_input
};

static void
bind_text_input_manager(struct wl_client *client,
			void *data,
			uint32_t version,
			uint32_t id)
{
	struct text_input_manager *text_input_manager = data;
	struct wl_resource *resource;

	/* No checking for duplicate binding necessary.  */
	resource =
		wl_resource_create(client,
				   &wl_text_input_manager_interface, 1, id);
	if (resource)
		wl_resource_set_implementation(resource,
					       &text_input_manager_implementation,
					       text_input_manager, NULL);
}

static void
text_input_manager_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct text_input_manager *text_input_manager =
		container_of(listener, struct text_input_manager, destroy_listener);

	wl_global_destroy(text_input_manager->text_input_manager_global);

	free(text_input_manager);
}

static void
text_input_manager_create(struct weston_compositor *ec)
{
	struct text_input_manager *text_input_manager;

	text_input_manager = calloc(1, sizeof *text_input_manager);

	text_input_manager->ec = ec;

	text_input_manager->text_input_manager_global =
		wl_global_create(ec->wl_display,
				 &wl_text_input_manager_interface, 1,
				 text_input_manager, bind_text_input_manager);

	text_input_manager->destroy_listener.notify = text_input_manager_notifier_destroy;
	wl_signal_add(&ec->destroy_signal, &text_input_manager->destroy_listener);
}

static void
input_method_context_destroy(struct wl_client *client,
			     struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
input_method_context_commit_string(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t serial,
				   const char *text)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_commit_string(context->model->resource, serial, text);
}

static void
input_method_context_preedit_string(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t serial,
				    const char *text,
				    const char *commit)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_preedit_string(context->model->resource, serial, text, commit);
}

static void
input_method_context_preedit_styling(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t index,
				     uint32_t length,
				     uint32_t style)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_preedit_styling(context->model->resource, index, length, style);
}

static void
input_method_context_preedit_cursor(struct wl_client *client,
				    struct wl_resource *resource,
				    int32_t cursor)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_preedit_cursor(context->model->resource, cursor);
}

static void
input_method_context_delete_surrounding_text(struct wl_client *client,
					     struct wl_resource *resource,
					     int32_t index,
					     uint32_t length)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_delete_surrounding_text(context->model->resource, index, length);
}

static void
input_method_context_cursor_position(struct wl_client *client,
				     struct wl_resource *resource,
				     int32_t index,
				     int32_t anchor)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_cursor_position(context->model->resource, index, anchor);
}

static void
input_method_context_modifiers_map(struct wl_client *client,
				   struct wl_resource *resource,
				   struct wl_array *map)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_modifiers_map(context->model->resource, map);
}

static void
input_method_context_keysym(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t serial,
			    uint32_t time,
			    uint32_t sym,
			    uint32_t state,
			    uint32_t modifiers)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	wl_text_input_send_keysym(context->model->resource, serial, time,
				  sym, state, modifiers);
}

static void
unbind_keyboard(struct wl_resource *resource)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	input_method_context_end_keyboard_grab(context);
	context->keyboard = NULL;
}

static void
input_method_context_grab_key(struct weston_keyboard_grab *grab,
			      uint32_t time, uint32_t key, uint32_t state_w)
{
	struct weston_keyboard *keyboard = grab->keyboard;
	struct wl_display *display;
	uint32_t serial;

	if (!keyboard->input_method_resource)
		return;

	display = wl_client_get_display(wl_resource_get_client(keyboard->input_method_resource));
	serial = wl_display_next_serial(display);
	wl_keyboard_send_key(keyboard->input_method_resource,
			     serial, time, key, state_w);
}

static void
input_method_context_grab_modifier(struct weston_keyboard_grab *grab, uint32_t serial,
				   uint32_t mods_depressed, uint32_t mods_latched,
				   uint32_t mods_locked, uint32_t group)
{
	struct weston_keyboard *keyboard = grab->keyboard;

	if (!keyboard->input_method_resource)
		return;

	wl_keyboard_send_modifiers(keyboard->input_method_resource,
				   serial, mods_depressed, mods_latched,
				   mods_locked, group);
}

static void
input_method_context_grab_cancel(struct weston_keyboard_grab *grab)
{
	weston_keyboard_end_grab(grab->keyboard);
}

static const struct weston_keyboard_grab_interface input_method_context_grab = {
	input_method_context_grab_key,
	input_method_context_grab_modifier,
	input_method_context_grab_cancel,
};

static void
input_method_context_grab_keyboard(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t id)
{
	struct input_method_context *context = wl_resource_get_user_data(resource);
	struct wl_resource *cr;
	struct weston_seat *seat = context->input_method->seat;
	struct weston_keyboard *keyboard = seat->keyboard;

	cr = wl_resource_create(client, &wl_keyboard_interface, 1, id);
	wl_resource_set_implementation(cr, NULL, context, unbind_keyboard);

	context->keyboard = cr;

	wl_keyboard_send_keymap(cr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
				keyboard->xkb_info->keymap_fd,
				keyboard->xkb_info->keymap_size);

	if (keyboard->grab != &keyboard->default_grab) {
		weston_keyboard_end_grab(keyboard);
	}
	weston_keyboard_start_grab(keyboard, &keyboard->input_method_grab);
	keyboard->input_method_resource = cr;
}

static void
input_method_context_key(struct wl_client *client,
			 struct wl_resource *resource,
			 uint32_t serial,
			 uint32_t time,
			 uint32_t key,
			 uint32_t state_w)
{
	struct input_method_context *context = wl_resource_get_user_data(resource);
	struct weston_seat *seat = context->input_method->seat;
	struct weston_keyboard *keyboard = seat->keyboard;
	struct weston_keyboard_grab *default_grab = &keyboard->default_grab;

	default_grab->interface->key(default_grab, time, key, state_w);
}

static void
input_method_context_modifiers(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t serial,
			       uint32_t mods_depressed,
			       uint32_t mods_latched,
			       uint32_t mods_locked,
			       uint32_t group)
{
	struct input_method_context *context = wl_resource_get_user_data(resource);

	struct weston_seat *seat = context->input_method->seat;
	struct weston_keyboard *keyboard = seat->keyboard;
	struct weston_keyboard_grab *default_grab = &keyboard->default_grab;

	default_grab->interface->modifiers(default_grab,
					   serial, mods_depressed,
					   mods_latched, mods_locked,
					   group);
}

static void
input_method_context_language(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t serial,
			      const char *language)
{
	struct input_method_context *context = wl_resource_get_user_data(resource);

	wl_text_input_send_language(context->model->resource, serial, language);
}

static void
input_method_context_text_direction(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t serial,
				    uint32_t direction)
{
	struct input_method_context *context = wl_resource_get_user_data(resource);

	wl_text_input_send_text_direction(context->model->resource, serial, direction);
}


static const struct wl_input_method_context_interface input_method_context_implementation = {
	input_method_context_destroy,
	input_method_context_commit_string,
	input_method_context_preedit_string,
	input_method_context_preedit_styling,
	input_method_context_preedit_cursor,
	input_method_context_delete_surrounding_text,
	input_method_context_cursor_position,
	input_method_context_modifiers_map,
	input_method_context_keysym,
	input_method_context_grab_keyboard,
	input_method_context_key,
	input_method_context_modifiers,
	input_method_context_language,
	input_method_context_text_direction
};

static void
destroy_input_method_context(struct wl_resource *resource)
{
	struct input_method_context *context = wl_resource_get_user_data(resource);

	if (context->keyboard) {
		wl_resource_destroy(context->keyboard);
	}

	free(context);
}

static void
input_method_context_create(struct text_input *model,
			    struct input_method *input_method)
{
	struct input_method_context *context;
	struct wl_resource *binding;

	if (!input_method->input_method_binding)
		return;

	context = calloc(1, sizeof *context);
	if (context == NULL)
		return;

	binding = input_method->input_method_binding;
	context->resource =
		wl_resource_create(wl_resource_get_client(binding),
				   &wl_input_method_context_interface, 1, 0);
	wl_resource_set_implementation(context->resource,
				       &input_method_context_implementation,
				       context, destroy_input_method_context);

	context->model = model;
	context->input_method = input_method;
	input_method->context = context;


	wl_input_method_send_activate(binding, context->resource);
}

static void
input_method_context_end_keyboard_grab(struct input_method_context *context)
{
	struct weston_keyboard_grab *grab =
		&context->input_method->seat->keyboard->input_method_grab;
	struct weston_keyboard *keyboard = grab->keyboard;

	if (!grab->keyboard)
		return;

	if (grab->keyboard->grab == grab)
		weston_keyboard_end_grab(grab->keyboard);

	keyboard->input_method_resource = NULL;
}

static void
unbind_input_method(struct wl_resource *resource)
{
	struct input_method *input_method = wl_resource_get_user_data(resource);
	struct text_backend *text_backend = input_method->text_backend;

	input_method->input_method_binding = NULL;
	input_method->context = NULL;

	text_backend->input_method.binding = NULL;
}

static void
bind_input_method(struct wl_client *client,
		  void *data,
		  uint32_t version,
		  uint32_t id)
{
	struct input_method *input_method = data;
	struct text_backend *text_backend = input_method->text_backend;
	struct wl_resource *resource;

	resource =
		wl_resource_create(client, &wl_input_method_interface, 1, id);

	if (input_method->input_method_binding != NULL) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "interface object already bound");
		wl_resource_destroy(resource);
		return;
	}

	if (text_backend->input_method.client != client) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "permission to bind input_method denied");
		wl_resource_destroy(resource);
		return;
	}

	wl_resource_set_implementation(resource, NULL, input_method,
				       unbind_input_method);
	input_method->input_method_binding = resource;

	text_backend->input_method.binding = resource;
}

static void
input_method_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct input_method *input_method =
		container_of(listener, struct input_method, destroy_listener);

	if (input_method->model)
		deactivate_text_input(input_method->model, input_method);

	wl_global_destroy(input_method->input_method_global);
	wl_list_remove(&input_method->destroy_listener.link);

	free(input_method);
}

static void
handle_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard = data;
	struct input_method *input_method =
		container_of(listener, struct input_method, keyboard_focus_listener);
	struct weston_surface *surface = keyboard->focus;

	if (!input_method->model)
		return;

	if (!surface || input_method->model->surface != surface)
		deactivate_text_input(input_method->model,
				      input_method);
}

static void
input_method_init_seat(struct weston_seat *seat)
{
	if (seat->input_method->focus_listener_initialized)
		return;

	if (seat->keyboard) {
		seat->input_method->keyboard_focus_listener.notify = handle_keyboard_focus;
		wl_signal_add(&seat->keyboard->focus_signal, &seat->input_method->keyboard_focus_listener);
		seat->keyboard->input_method_grab.interface = &input_method_context_grab;
	}

	seat->input_method->focus_listener_initialized = 1;
}

static void launch_input_method(struct text_backend *text_backend);

static void
handle_input_method_sigchld(struct weston_process *process, int status)
{
	uint32_t time;
	struct text_backend *text_backend =
		container_of(process, struct text_backend, input_method.process);

	text_backend->input_method.process.pid = 0;
	text_backend->input_method.client = NULL;

	/* if input_method dies more than 5 times in 10 seconds, give up */
	time = weston_compositor_get_time();
	if (time - text_backend->input_method.deathstamp > 10000) {
		text_backend->input_method.deathstamp = time;
		text_backend->input_method.deathcount = 0;
	}

	text_backend->input_method.deathcount++;
	if (text_backend->input_method.deathcount > 5) {
		weston_log("input_method died, giving up.\n");
		return;
	}

	weston_log("input_method died, respawning...\n");
	launch_input_method(text_backend);
}

static void
launch_input_method(struct text_backend *text_backend)
{
	if (text_backend->input_method.binding)
		return;

	if (!text_backend->input_method.path)
		return;

	if (text_backend->input_method.process.pid != 0)
		return;

	text_backend->input_method.client = weston_client_launch(text_backend->compositor,
								 &text_backend->input_method.process,
								 text_backend->input_method.path,
								 handle_input_method_sigchld);

	if (!text_backend->input_method.client)
		weston_log("not able to start %s\n", text_backend->input_method.path);
}

static void
handle_seat_created(struct wl_listener *listener,
		    void *data)
{
	struct weston_seat *seat = data;
	struct text_backend *text_backend =
		container_of(listener, struct text_backend,
			     seat_created_listener);
	struct input_method *input_method;
	struct weston_compositor *ec = seat->compositor;

	input_method = calloc(1, sizeof *input_method);

	input_method->seat = seat;
	input_method->model = NULL;
	input_method->focus_listener_initialized = 0;
	input_method->context = NULL;
	input_method->text_backend = text_backend;

	input_method->input_method_global =
		wl_global_create(ec->wl_display, &wl_input_method_interface, 1,
				 input_method, bind_input_method);

	input_method->destroy_listener.notify = input_method_notifier_destroy;
	wl_signal_add(&seat->destroy_signal, &input_method->destroy_listener);

	seat->input_method = input_method;

	launch_input_method(text_backend);
}

static void
text_backend_configuration(struct text_backend *text_backend)
{
	struct weston_config_section *section;

	section = weston_config_get_section(text_backend->compositor->config,
					    "input-method", NULL, NULL);
	weston_config_section_get_string(section, "path",
					 &text_backend->input_method.path,
					 LIBEXECDIR "/weston-keyboard");
}

static void
text_backend_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct text_backend *text_backend =
		container_of(listener, struct text_backend, destroy_listener);

	if (text_backend->input_method.client)
		wl_client_destroy(text_backend->input_method.client);

	free(text_backend->input_method.path);

	free(text_backend);
}


WL_EXPORT int
text_backend_init(struct weston_compositor *ec)
{
	struct text_backend *text_backend;

	text_backend = calloc(1, sizeof(*text_backend));

	text_backend->compositor = ec;

	text_backend->seat_created_listener.notify = handle_seat_created;
	wl_signal_add(&ec->seat_created_signal,
		      &text_backend->seat_created_listener);

	text_backend->destroy_listener.notify = text_backend_notifier_destroy;
	wl_signal_add(&ec->destroy_signal, &text_backend->destroy_listener);

	text_backend_configuration(text_backend);

	text_input_manager_create(ec);

	return 0;
}
