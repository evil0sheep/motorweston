/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
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

#include <linux/input.h>

#include "shell.h"

struct exposay_surface {
	struct desktop_shell *shell;
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_list link;

	int x;
	int y;
	int width;
	int height;
	double scale;

	int row;
	int column;

	/* The animations only apply a transformation for their own lifetime,
	 * and don't have an option to indefinitely maintain the
	 * transformation in a steady state - so, we apply our own once the
	 * animation has finished. */
	struct weston_transform transform;
};

static void exposay_set_state(struct desktop_shell *shell,
                              enum exposay_target_state state,
			      struct weston_seat *seat);
static void exposay_check_state(struct desktop_shell *shell);

static void
exposay_in_flight_inc(struct desktop_shell *shell)
{
	shell->exposay.in_flight++;
}

static void
exposay_in_flight_dec(struct desktop_shell *shell)
{
	if (--shell->exposay.in_flight > 0)
		return;

	exposay_check_state(shell);
}

static void
exposay_animate_in_done(struct weston_view_animation *animation, void *data)
{
	struct exposay_surface *esurface = data;

	wl_list_insert(&esurface->view->geometry.transformation_list,
	               &esurface->transform.link);
	weston_matrix_init(&esurface->transform.matrix);
	weston_matrix_scale(&esurface->transform.matrix,
	                    esurface->scale, esurface->scale, 1.0f);
	weston_matrix_translate(&esurface->transform.matrix,
	                        esurface->x - esurface->view->geometry.x,
				esurface->y - esurface->view->geometry.y,
				0);

	weston_view_geometry_dirty(esurface->view);
	weston_compositor_schedule_repaint(esurface->view->surface->compositor);

	exposay_in_flight_dec(esurface->shell);
}

static void
exposay_animate_in(struct exposay_surface *esurface)
{
	exposay_in_flight_inc(esurface->shell);

	weston_move_scale_run(esurface->view,
	                      esurface->x - esurface->view->geometry.x,
	                      esurface->y - esurface->view->geometry.y,
			      1.0, esurface->scale, 0,
	                      exposay_animate_in_done, esurface);
}

static void
exposay_animate_out_done(struct weston_view_animation *animation, void *data)
{
	struct exposay_surface *esurface = data;
	struct desktop_shell *shell = esurface->shell;

	wl_list_remove(&esurface->link);
	free(esurface);

	exposay_in_flight_dec(shell);
}

static void
exposay_animate_out(struct exposay_surface *esurface)
{
	exposay_in_flight_inc(esurface->shell);

	/* Remove the static transformation set up by
	 * exposay_transform_in_done(). */
	wl_list_remove(&esurface->transform.link);
	weston_view_geometry_dirty(esurface->view);

	weston_move_scale_run(esurface->view,
	                      esurface->x - esurface->view->geometry.x,
	                      esurface->y - esurface->view->geometry.y,
			      1.0, esurface->scale, 1,
			      exposay_animate_out_done, esurface);
}

static void
exposay_highlight_surface(struct desktop_shell *shell,
                          struct exposay_surface *esurface)
{
	struct weston_view *view = NULL;

	if (esurface) {
		shell->exposay.row_current = esurface->row;
		shell->exposay.column_current = esurface->column;
		view = esurface->view;
	}

	activate(shell, view->surface, shell->exposay.seat);
	shell->exposay.focus_current = view;
}

static int
exposay_is_animating(struct desktop_shell *shell)
{
	if (shell->exposay.state_cur == EXPOSAY_LAYOUT_INACTIVE ||
	    shell->exposay.state_cur == EXPOSAY_LAYOUT_OVERVIEW)
		return 0;

	return (shell->exposay.in_flight > 0);
}

static void
exposay_pick(struct desktop_shell *shell, int x, int y)
{
	struct exposay_surface *esurface;

        if (exposay_is_animating(shell))
            return;

	wl_list_for_each(esurface, &shell->exposay.surface_list, link) {
		if (x < esurface->x || x > esurface->x + esurface->width)
			continue;
		if (y < esurface->y || y > esurface->y + esurface->height)
			continue;

		exposay_highlight_surface(shell, esurface);
		return;
	}
}

/* Pretty lame layout for now; just tries to make a square.  Should take
 * aspect ratio into account really.  Also needs to be notified of surface
 * addition and removal and adjust layout/animate accordingly. */
static enum exposay_layout_state
exposay_layout(struct desktop_shell *shell)
{
	struct workspace *workspace = shell->exposay.workspace;
	struct weston_compositor *compositor = shell->compositor;
	struct weston_output *output = get_default_output(compositor);
	struct weston_view *view;
	struct exposay_surface *esurface;
	int w, h;
	int i;
	int last_row_removed = 0;

	wl_list_init(&shell->exposay.surface_list);

	shell->exposay.num_surfaces = 0;
	wl_list_for_each(view, &workspace->layer.view_list, layer_link) {
		if (!get_shell_surface(view->surface))
			continue;
		shell->exposay.num_surfaces++;
	}

	if (shell->exposay.num_surfaces == 0) {
		shell->exposay.grid_size = 0;
		shell->exposay.hpadding_outer = 0;
		shell->exposay.vpadding_outer = 0;
		shell->exposay.padding_inner = 0;
		shell->exposay.surface_size = 0;
		return EXPOSAY_LAYOUT_OVERVIEW;
	}

	/* Lay the grid out as square as possible, losing surfaces from the
	 * bottom row if required.  Start with fixed padding of a 10% margin
	 * around the outside and 80px internal padding between surfaces, and
	 * maximise the area made available to surfaces after this, but only
	 * to a maximum of 1/3rd the total output size.
	 *
	 * If we can't make a square grid, add one extra row at the bottom
	 * which will have a smaller number of columns.
	 *
	 * XXX: Surely there has to be a better way to express this maths,
	 *      right?!
	 */
	shell->exposay.grid_size = floor(sqrtf(shell->exposay.num_surfaces));
	if (pow(shell->exposay.grid_size, 2) != shell->exposay.num_surfaces)
		shell->exposay.grid_size++;
	last_row_removed = pow(shell->exposay.grid_size, 2) - shell->exposay.num_surfaces;

	shell->exposay.hpadding_outer = (output->width / 10);
	shell->exposay.vpadding_outer = (output->height / 10);
	shell->exposay.padding_inner = 80;

	w = output->width - (shell->exposay.hpadding_outer * 2);
	w -= shell->exposay.padding_inner * (shell->exposay.grid_size - 1);
	w /= shell->exposay.grid_size;

	h = output->height - (shell->exposay.vpadding_outer * 2);
	h -= shell->exposay.padding_inner * (shell->exposay.grid_size - 1);
	h /= shell->exposay.grid_size;

	shell->exposay.surface_size = (w < h) ? w : h;
	if (shell->exposay.surface_size > (output->width / 2))
		shell->exposay.surface_size = output->width / 2;
	if (shell->exposay.surface_size > (output->height / 2))
		shell->exposay.surface_size = output->height / 2;

	i = 0;
	wl_list_for_each(view, &workspace->layer.view_list, layer_link) {
		int pad;

		pad = shell->exposay.surface_size + shell->exposay.padding_inner;

		if (!get_shell_surface(view->surface))
			continue;

		esurface = malloc(sizeof(*esurface));
		if (!esurface) {
			exposay_set_state(shell, EXPOSAY_TARGET_CANCEL,
			                  shell->exposay.seat);
			break;
		}

		wl_list_insert(&shell->exposay.surface_list, &esurface->link);
		esurface->shell = shell;
		esurface->view = view;

		esurface->row = i / shell->exposay.grid_size;
		esurface->column = i % shell->exposay.grid_size;

		esurface->x = shell->exposay.hpadding_outer;
		esurface->x += pad * esurface->column;
		esurface->y = shell->exposay.vpadding_outer;
		esurface->y += pad * esurface->row;

		if (esurface->row == shell->exposay.grid_size - 1)
			esurface->x += (shell->exposay.surface_size + shell->exposay.padding_inner) * last_row_removed / 2;

		if (view->surface->width > view->surface->height)
			esurface->scale = shell->exposay.surface_size / (float) view->surface->width;
		else
			esurface->scale = shell->exposay.surface_size / (float) view->surface->height;
		esurface->width = view->surface->width * esurface->scale;
		esurface->height = view->surface->height * esurface->scale;

		if (shell->exposay.focus_current == esurface->view)
			exposay_highlight_surface(shell, esurface);

		exposay_animate_in(esurface);

		i++;
	}

	weston_compositor_schedule_repaint(shell->compositor);

	return EXPOSAY_LAYOUT_ANIMATE_TO_OVERVIEW;
}

static void
exposay_focus(struct weston_pointer_grab *grab)
{
}

static void
exposay_motion(struct weston_pointer_grab *grab, uint32_t time,
	       wl_fixed_t x, wl_fixed_t y)
{
	struct desktop_shell *shell =
		container_of(grab, struct desktop_shell, exposay.grab_ptr);

	weston_pointer_move(grab->pointer, x, y);

	exposay_pick(shell,
	             wl_fixed_to_int(grab->pointer->x),
	             wl_fixed_to_int(grab->pointer->y));
}

static void
exposay_button(struct weston_pointer_grab *grab, uint32_t time, uint32_t button,
               uint32_t state_w)
{
	struct desktop_shell *shell =
		container_of(grab, struct desktop_shell, exposay.grab_ptr);
	struct weston_seat *seat = grab->pointer->seat;
	enum wl_pointer_button_state state = state_w;

	if (button != BTN_LEFT)
		return;

	/* Store the surface we clicked on, and don't do anything if we end up
	 * releasing on a different surface. */
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		shell->exposay.clicked = shell->exposay.focus_current;
		return;
	}

	if (shell->exposay.focus_current == shell->exposay.clicked)
		exposay_set_state(shell, EXPOSAY_TARGET_SWITCH, seat);
	else
		shell->exposay.clicked = NULL;
}

static void
exposay_pointer_grab_cancel(struct weston_pointer_grab *grab)
{
	struct desktop_shell *shell =
		container_of(grab, struct desktop_shell, exposay.grab_ptr);

	exposay_set_state(shell, EXPOSAY_TARGET_CANCEL, shell->exposay.seat);
}

static const struct weston_pointer_grab_interface exposay_ptr_grab = {
	exposay_focus,
	exposay_motion,
	exposay_button,
	exposay_pointer_grab_cancel,
};

static int
exposay_maybe_move(struct desktop_shell *shell, int row, int column)
{
	struct exposay_surface *esurface;

	wl_list_for_each(esurface, &shell->exposay.surface_list, link) {
		if (esurface->row != row || esurface->column != column)
			continue;

		exposay_highlight_surface(shell, esurface);
		return 1;
	}

	return 0;
}

static void
exposay_key(struct weston_keyboard_grab *grab, uint32_t time, uint32_t key,
            uint32_t state_w)
{
	struct weston_seat *seat = grab->keyboard->seat;
	struct desktop_shell *shell =
		container_of(grab, struct desktop_shell, exposay.grab_kbd);
	enum wl_keyboard_key_state state = state_w;

	if (state != WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	switch (key) {
	case KEY_ESC:
		exposay_set_state(shell, EXPOSAY_TARGET_CANCEL, seat);
		break;
	case KEY_ENTER:
		exposay_set_state(shell, EXPOSAY_TARGET_SWITCH, seat);
		break;
	case KEY_UP:
		exposay_maybe_move(shell, shell->exposay.row_current - 1,
		                   shell->exposay.column_current);
		break;
	case KEY_DOWN:
		/* Special case for trying to move to the bottom row when it
		 * has fewer items than all the others. */
		if (!exposay_maybe_move(shell, shell->exposay.row_current + 1,
		                        shell->exposay.column_current) &&
		    shell->exposay.row_current < (shell->exposay.grid_size - 1)) {
			exposay_maybe_move(shell, shell->exposay.row_current + 1,
					   (shell->exposay.num_surfaces %
					    shell->exposay.grid_size) - 1);
		}
		break;
	case KEY_LEFT:
		exposay_maybe_move(shell, shell->exposay.row_current,
		                   shell->exposay.column_current - 1);
		break;
	case KEY_RIGHT:
		exposay_maybe_move(shell, shell->exposay.row_current,
		                   shell->exposay.column_current + 1);
		break;
	case KEY_TAB:
		/* Try to move right, then down (and to the leftmost column),
		 * then if all else fails, to the top left. */
		if (!exposay_maybe_move(shell, shell->exposay.row_current,
					shell->exposay.column_current + 1) &&
		    !exposay_maybe_move(shell, shell->exposay.row_current + 1, 0))
			exposay_maybe_move(shell, 0, 0);
		break;
	default:
		break;
	}
}

static void
exposay_modifier(struct weston_keyboard_grab *grab, uint32_t serial,
                 uint32_t mods_depressed, uint32_t mods_latched,
                 uint32_t mods_locked, uint32_t group)
{
	struct desktop_shell *shell =
		container_of(grab, struct desktop_shell, exposay.grab_kbd);
	struct weston_seat *seat = (struct weston_seat *) grab->keyboard->seat;

	/* We want to know when mod has been pressed and released.
	 * FIXME: There is a problem here: if mod is pressed, then a key
	 * is pressed and released, then mod is released, we will treat that
	 * as if only mod had been pressed and released. */
	if (seat->modifier_state) {
		if (seat->modifier_state == shell->binding_modifier) {
			shell->exposay.mod_pressed = true;
		} else {
			shell->exposay.mod_invalid = true;
		}
	} else {
		if (shell->exposay.mod_pressed && !shell->exposay.mod_invalid)
			exposay_set_state(shell, EXPOSAY_TARGET_CANCEL, seat);

		shell->exposay.mod_invalid = false;
		shell->exposay.mod_pressed = false;
	}

	return;
}

static void
exposay_cancel(struct weston_keyboard_grab *grab)
{
	struct desktop_shell *shell =
		container_of(grab, struct desktop_shell, exposay.grab_kbd);

	exposay_set_state(shell, EXPOSAY_TARGET_CANCEL, shell->exposay.seat);
}

static const struct weston_keyboard_grab_interface exposay_kbd_grab = {
	exposay_key,
	exposay_modifier,
	exposay_cancel,
};

/**
 * Called when the transition from overview -> inactive has completed.
 */
static enum exposay_layout_state
exposay_set_inactive(struct desktop_shell *shell)
{
	struct weston_seat *seat = shell->exposay.seat;

	weston_keyboard_end_grab(seat->keyboard);
	weston_pointer_end_grab(seat->pointer);
	if (seat->keyboard->input_method_resource)
		seat->keyboard->grab = &seat->keyboard->input_method_grab;

	return EXPOSAY_LAYOUT_INACTIVE;
}

/**
 * Begins the transition from overview to inactive. */
static enum exposay_layout_state
exposay_transition_inactive(struct desktop_shell *shell, int switch_focus)
{
	struct exposay_surface *esurface;

	/* Call activate() before we start the animations to avoid
	 * animating back the old state and then immediately transitioning
	 * to the new. */
	if (switch_focus && shell->exposay.focus_current)
		activate(shell, shell->exposay.focus_current->surface,
		         shell->exposay.seat);
	else if (shell->exposay.focus_prev)
		activate(shell, shell->exposay.focus_prev->surface,
		         shell->exposay.seat);

	wl_list_for_each(esurface, &shell->exposay.surface_list, link)
		exposay_animate_out(esurface);
	weston_compositor_schedule_repaint(shell->compositor);

	return EXPOSAY_LAYOUT_ANIMATE_TO_INACTIVE;
}

static enum exposay_layout_state
exposay_transition_active(struct desktop_shell *shell)
{
	struct weston_seat *seat = shell->exposay.seat;

	shell->exposay.workspace = get_current_workspace(shell);
	shell->exposay.focus_prev = get_default_view (seat->keyboard->focus);
	shell->exposay.focus_current = get_default_view (seat->keyboard->focus);
	shell->exposay.clicked = NULL;
	wl_list_init(&shell->exposay.surface_list);

	lower_fullscreen_layer(shell);
	shell->exposay.grab_kbd.interface = &exposay_kbd_grab;
	weston_keyboard_start_grab(seat->keyboard,
	                           &shell->exposay.grab_kbd);
	weston_keyboard_set_focus(seat->keyboard, NULL);

	shell->exposay.grab_ptr.interface = &exposay_ptr_grab;
	weston_pointer_start_grab(seat->pointer,
	                          &shell->exposay.grab_ptr);
	weston_pointer_set_focus(seat->pointer, NULL,
			         seat->pointer->x, seat->pointer->y);

	return exposay_layout(shell);
}

static void
exposay_check_state(struct desktop_shell *shell)
{
	enum exposay_layout_state state_new = shell->exposay.state_cur;
	int do_switch = 0;

	/* Don't do anything whilst animations are running, just store up
	 * target state changes and only act on them when the animations have
	 * completed. */
	if (exposay_is_animating(shell))
		return;

	switch (shell->exposay.state_target) {
	case EXPOSAY_TARGET_OVERVIEW:
		switch (shell->exposay.state_cur) {
		case EXPOSAY_LAYOUT_OVERVIEW:
			goto out;
		case EXPOSAY_LAYOUT_ANIMATE_TO_OVERVIEW:
			state_new = EXPOSAY_LAYOUT_OVERVIEW;
			break;
		default:
			state_new = exposay_transition_active(shell);
			break;
		}
		break;

	case EXPOSAY_TARGET_SWITCH:
		do_switch = 1; /* fallthrough */
	case EXPOSAY_TARGET_CANCEL:
		switch (shell->exposay.state_cur) {
		case EXPOSAY_LAYOUT_INACTIVE:
			goto out;
		case EXPOSAY_LAYOUT_ANIMATE_TO_INACTIVE:
			state_new = exposay_set_inactive(shell);
			break;
		default:
			state_new = exposay_transition_inactive(shell, do_switch);
			break;
		}

		break;
	}

out:
	shell->exposay.state_cur = state_new;
}

static void
exposay_set_state(struct desktop_shell *shell, enum exposay_target_state state,
                  struct weston_seat *seat)
{
	shell->exposay.state_target = state;
	shell->exposay.seat = seat;
	exposay_check_state(shell);
}

void
exposay_binding(struct weston_seat *seat, enum weston_keyboard_modifier modifier,
		void *data)
{
	struct desktop_shell *shell = data;

	exposay_set_state(shell, EXPOSAY_TARGET_OVERVIEW, seat);
}
