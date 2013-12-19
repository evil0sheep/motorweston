/*
 * Copyright © 2011 Intel Corporation
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

#include "compositor.h"

struct weston_binding {
	uint32_t key;
	uint32_t button;
	uint32_t axis;
	uint32_t modifier;
	void *handler;
	void *data;
	struct wl_list link;
};

static struct weston_binding *
weston_compositor_add_binding(struct weston_compositor *compositor,
			      uint32_t key, uint32_t button, uint32_t axis,
			      uint32_t modifier, void *handler, void *data)
{
	struct weston_binding *binding;

	binding = malloc(sizeof *binding);
	if (binding == NULL)
		return NULL;

	binding->key = key;
	binding->button = button;
	binding->axis = axis;
	binding->modifier = modifier;
	binding->handler = handler;
	binding->data = data;

	return binding;
}

WL_EXPORT struct weston_binding *
weston_compositor_add_key_binding(struct weston_compositor *compositor,
				  uint32_t key, uint32_t modifier,
				  weston_key_binding_handler_t handler,
				  void *data)
{
	struct weston_binding *binding;

	binding = weston_compositor_add_binding(compositor, key, 0, 0,
						modifier, handler, data);
	if (binding == NULL)
		return NULL;

	wl_list_insert(compositor->key_binding_list.prev, &binding->link);

	return binding;
}

WL_EXPORT struct weston_binding *
weston_compositor_add_modifier_binding(struct weston_compositor *compositor,
				       uint32_t modifier,
				       weston_modifier_binding_handler_t handler,
				       void *data)
{
	struct weston_binding *binding;

	binding = weston_compositor_add_binding(compositor, 0, 0, 0,
						modifier, handler, data);
	if (binding == NULL)
		return NULL;

	wl_list_insert(compositor->modifier_binding_list.prev, &binding->link);

	return binding;
}

WL_EXPORT struct weston_binding *
weston_compositor_add_button_binding(struct weston_compositor *compositor,
				     uint32_t button, uint32_t modifier,
				     weston_button_binding_handler_t handler,
				     void *data)
{
	struct weston_binding *binding;

	binding = weston_compositor_add_binding(compositor, 0, button, 0,
						modifier, handler, data);
	if (binding == NULL)
		return NULL;

	wl_list_insert(compositor->button_binding_list.prev, &binding->link);

	return binding;
}

WL_EXPORT struct weston_binding *
weston_compositor_add_touch_binding(struct weston_compositor *compositor,
				    uint32_t modifier,
				    weston_touch_binding_handler_t handler,
				    void *data)
{
	struct weston_binding *binding;

	binding = weston_compositor_add_binding(compositor, 0, 0, 0,
						modifier, handler, data);
	if (binding == NULL)
		return NULL;

	wl_list_insert(compositor->touch_binding_list.prev, &binding->link);

	return binding;
}

WL_EXPORT struct weston_binding *
weston_compositor_add_axis_binding(struct weston_compositor *compositor,
				   uint32_t axis, uint32_t modifier,
				   weston_axis_binding_handler_t handler,
				   void *data)
{
	struct weston_binding *binding;

	binding = weston_compositor_add_binding(compositor, 0, 0, axis,
						modifier, handler, data);
	if (binding == NULL)
		return NULL;

	wl_list_insert(compositor->axis_binding_list.prev, &binding->link);

	return binding;
}

WL_EXPORT struct weston_binding *
weston_compositor_add_debug_binding(struct weston_compositor *compositor,
				    uint32_t key,
				    weston_key_binding_handler_t handler,
				    void *data)
{
	struct weston_binding *binding;

	binding = weston_compositor_add_binding(compositor, key, 0, 0, 0,
						handler, data);

	wl_list_insert(compositor->debug_binding_list.prev, &binding->link);

	return binding;
}

WL_EXPORT void
weston_binding_destroy(struct weston_binding *binding)
{
	wl_list_remove(&binding->link);
	free(binding);
}

WL_EXPORT void
weston_binding_list_destroy_all(struct wl_list *list)
{
	struct weston_binding *binding, *tmp;

	wl_list_for_each_safe(binding, tmp, list, link)
		weston_binding_destroy(binding);
}

struct binding_keyboard_grab {
	uint32_t key;
	struct weston_keyboard_grab grab;
};

static void
binding_key(struct weston_keyboard_grab *grab,
	    uint32_t time, uint32_t key, uint32_t state_w)
{
	struct binding_keyboard_grab *b =
		container_of(grab, struct binding_keyboard_grab, grab);
	struct wl_resource *resource;
	enum wl_keyboard_key_state state = state_w;
	uint32_t serial;
	struct weston_keyboard *keyboard = grab->keyboard;
	struct wl_display *display = keyboard->seat->compositor->wl_display;

	if (key == b->key) {
		if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
			weston_keyboard_end_grab(grab->keyboard);
			if (keyboard->input_method_resource)
				keyboard->grab = &keyboard->input_method_grab;
			free(b);
		}
	} else if (!wl_list_empty(&keyboard->focus_resource_list)) {
		serial = wl_display_next_serial(display);
		wl_resource_for_each(resource, &keyboard->focus_resource_list) {
			wl_keyboard_send_key(resource,
					     serial,
					     time,
					     key,
					     state);
		}
	}
}

static void
binding_modifiers(struct weston_keyboard_grab *grab, uint32_t serial,
		  uint32_t mods_depressed, uint32_t mods_latched,
		  uint32_t mods_locked, uint32_t group)
{
	struct wl_resource *resource;

	wl_resource_for_each(resource, &grab->keyboard->focus_resource_list) {
		wl_keyboard_send_modifiers(resource, serial, mods_depressed,
					   mods_latched, mods_locked, group);
	}
}

static void
binding_cancel(struct weston_keyboard_grab *grab)
{
	struct binding_keyboard_grab *binding_grab =
		container_of(grab, struct binding_keyboard_grab, grab);

	weston_keyboard_end_grab(grab->keyboard);
	free(binding_grab);
}

static const struct weston_keyboard_grab_interface binding_grab = {
	binding_key,
	binding_modifiers,
	binding_cancel,
};

static void
install_binding_grab(struct weston_seat *seat, uint32_t time, uint32_t key)
{
	struct binding_keyboard_grab *grab;

	grab = malloc(sizeof *grab);
	grab->key = key;
	grab->grab.interface = &binding_grab;
	weston_keyboard_start_grab(seat->keyboard, &grab->grab);
}

WL_EXPORT void
weston_compositor_run_key_binding(struct weston_compositor *compositor,
				  struct weston_seat *seat,
				  uint32_t time, uint32_t key,
				  enum wl_keyboard_key_state state)
{
	struct weston_binding *b;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	/* Invalidate all active modifier bindings. */
	wl_list_for_each(b, &compositor->modifier_binding_list, link)
		b->key = key;

	wl_list_for_each(b, &compositor->key_binding_list, link) {
		if (b->key == key && b->modifier == seat->modifier_state) {
			weston_key_binding_handler_t handler = b->handler;
			handler(seat, time, key, b->data);

			/* If this was a key binding and it didn't
			 * install a keyboard grab, install one now to
			 * swallow the key release. */
			if (seat->keyboard->grab ==
			    &seat->keyboard->default_grab)
				install_binding_grab(seat, time, key);
		}
	}
}

WL_EXPORT void
weston_compositor_run_modifier_binding(struct weston_compositor *compositor,
				       struct weston_seat *seat,
				       enum weston_keyboard_modifier modifier,
				       enum wl_keyboard_key_state state)
{
	struct weston_binding *b;

	if (seat->keyboard->grab != &seat->keyboard->default_grab)
		return;

	wl_list_for_each(b, &compositor->modifier_binding_list, link) {
		weston_modifier_binding_handler_t handler = b->handler;

		if (b->modifier != modifier)
			continue;

		/* Prime the modifier binding. */
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			b->key = 0;
			continue;
		}
		/* Ignore the binding if a key was pressed in between. */
		else if (b->key != 0) {
			return;
		}

		handler(seat, modifier, b->data);
	}
}

WL_EXPORT void
weston_compositor_run_button_binding(struct weston_compositor *compositor,
				     struct weston_seat *seat,
				     uint32_t time, uint32_t button,
				     enum wl_pointer_button_state state)
{
	struct weston_binding *b;

	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		return;

	/* Invalidate all active modifier bindings. */
	wl_list_for_each(b, &compositor->modifier_binding_list, link)
		b->key = button;

	wl_list_for_each(b, &compositor->button_binding_list, link) {
		if (b->button == button && b->modifier == seat->modifier_state) {
			weston_button_binding_handler_t handler = b->handler;
			handler(seat, time, button, b->data);
		}
	}
}

WL_EXPORT void
weston_compositor_run_touch_binding(struct weston_compositor *compositor,
				    struct weston_seat *seat, uint32_t time,
				    int touch_type)
{
	struct weston_binding *b;

	if (seat->touch->num_tp != 1 || touch_type != WL_TOUCH_DOWN)
		return;

	wl_list_for_each(b, &compositor->touch_binding_list, link) {
		if (b->modifier == seat->modifier_state) {
			weston_touch_binding_handler_t handler = b->handler;
			handler(seat, time, b->data);
		}
	}
}

WL_EXPORT int
weston_compositor_run_axis_binding(struct weston_compositor *compositor,
				   struct weston_seat *seat,
				   uint32_t time, uint32_t axis,
				   wl_fixed_t value)
{
	struct weston_binding *b;

	/* Invalidate all active modifier bindings. */
	wl_list_for_each(b, &compositor->modifier_binding_list, link)
		b->key = axis;

	wl_list_for_each(b, &compositor->axis_binding_list, link) {
		if (b->axis == axis && b->modifier == seat->modifier_state) {
			weston_axis_binding_handler_t handler = b->handler;
			handler(seat, time, axis, value, b->data);
			return 1;
		}
	}

	return 0;
}

WL_EXPORT int
weston_compositor_run_debug_binding(struct weston_compositor *compositor,
				    struct weston_seat *seat,
				    uint32_t time, uint32_t key,
				    enum wl_keyboard_key_state state)
{
	weston_key_binding_handler_t handler;
	struct weston_binding *binding;
	int count = 0;

	wl_list_for_each(binding, &compositor->debug_binding_list, link) {
		if (key != binding->key)
			continue;

		count++;
		handler = binding->handler;
		handler(seat, time, key, binding->data);
	}

	return count;
}
