/*
 * Copyright © 2013 Collabora Ltd.
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <linux/input.h>
#include <cairo.h>

#include "window.h"

struct stacking {
	struct display *display;
	struct window *root_window;
};

static void
button_handler(struct widget *widget,
               struct input *input, uint32_t time,
               uint32_t button,
               enum wl_pointer_button_state state, void *data);
static void
key_handler(struct window *window,
            struct input *input, uint32_t time,
            uint32_t key, uint32_t sym, enum wl_keyboard_key_state state,
            void *data);
static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data);
static void
fullscreen_handler(struct window *window, void *data);
static void
redraw_handler(struct widget *widget, void *data);

/* Iff parent_window is set, the new window will be transient. */
static struct window *
new_window(struct stacking *stacking, struct window *parent_window)
{
	struct window *new_window;
	struct widget *new_widget;

	if (parent_window == NULL) {
		new_window = window_create(stacking->display);
	} else {
		new_window = window_create_transient(stacking->display,
		                                     parent_window, 50, 50, 0);
	}

	new_widget = window_frame_create(new_window, new_window);

	window_set_title(new_window, "Stacking Test");
	window_set_key_handler(new_window, key_handler);
	window_set_keyboard_focus_handler(new_window, keyboard_focus_handler);
	window_set_fullscreen_handler(new_window, fullscreen_handler);
	widget_set_button_handler(new_widget, button_handler);
	widget_set_redraw_handler(new_widget, redraw_handler);
	window_set_user_data(new_window, stacking);

	window_schedule_resize(new_window, 300, 300);

	return new_window;
}

static void
show_popup_cb(struct window *window, struct input *input, int index, void *data)
{
	/* Ignore the selected menu item. */
}

static void
show_popup(struct stacking *stacking, struct input *input, uint32_t time,
           struct window *window)
{
	int32_t x, y;
	static const char *entries[] = {
		"Test Entry",
		"Another Test Entry",
	};

	input_get_position(input, &x, &y);
	window_show_menu(stacking->display, input, time, window, x, y,
	                 show_popup_cb, entries, ARRAY_LENGTH(entries));
}

static void
button_handler(struct widget *widget,
               struct input *input, uint32_t time,
               uint32_t button,
               enum wl_pointer_button_state state, void *data)
{
	struct stacking *stacking = data;

	switch (button) {
	case BTN_RIGHT:
		if (state == WL_POINTER_BUTTON_STATE_PRESSED)
			show_popup(stacking, input, time,
			           widget_get_user_data(widget));
		break;

	case BTN_LEFT:
	default:
		break;
	}
}

static void
key_handler(struct window *window,
            struct input *input, uint32_t time,
            uint32_t key, uint32_t sym, enum wl_keyboard_key_state state,
            void *data)
{
	struct stacking *stacking = data;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	switch (sym) {
	case XKB_KEY_f:
		fullscreen_handler(window, data);
		break;

	case XKB_KEY_m:
		window_set_maximized(window, !window_is_maximized(window));
		break;

	case XKB_KEY_n:
		/* New top-level window. */
		new_window(stacking, NULL);
		break;

	case XKB_KEY_p:
		show_popup(stacking, input, time, window);
		break;

	case XKB_KEY_q:
		exit (0);
		break;

	case XKB_KEY_t:
		/* New transient window. */
		new_window(stacking, window);
		break;

	default:
		break;
	}
}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	window_schedule_redraw(window);
}

static void
fullscreen_handler(struct window *window, void *data)
{
	window_set_fullscreen(window, !window_is_fullscreen(window));
}

static void
draw_string(cairo_t *cr,
            const char *fmt, ...) __attribute__((format (gnu_printf, 2, 3)));

static void
draw_string(cairo_t *cr,
            const char *fmt, ...)
{
	char buffer[4096];
	char *p, *end;
	va_list argp;
	cairo_text_extents_t text_extents;
	cairo_font_extents_t font_extents;

	cairo_save(cr);

	cairo_select_font_face(cr, "sans",
	                       CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);

	cairo_font_extents(cr, &font_extents);

	va_start(argp, fmt);

	vsnprintf(buffer, sizeof(buffer), fmt, argp);

	p = buffer;
	while (*p) {
		end = strchr(p, '\n');
		if (end)
			*end = 0;

		cairo_show_text(cr, p);
		cairo_text_extents(cr, p, &text_extents);
		cairo_rel_move_to(cr, -text_extents.x_advance, font_extents.height);

		if (end)
			p = end + 1;
		else
			break;
	}

	va_end(argp);

	cairo_restore(cr);
}

static void
set_window_background_colour(cairo_t *cr, struct window *window)
{
	if (window_is_transient(window))
		cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.4);
	else if (window_is_maximized(window))
		cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.6);
	else if (window_is_fullscreen(window))
		cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 0.6);
	else
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct window *window;
	struct rectangle allocation;
	cairo_t *cr;

	widget_get_allocation(widget, &allocation);
	window = widget_get_user_data(widget);

	cr = widget_cairo_create(widget);
	cairo_translate(cr, allocation.x, allocation.y);

	/* Draw background. */
	cairo_push_group(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_window_background_colour(cr, window);
	cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
	cairo_fill(cr);

	cairo_pop_group_to_source(cr);
	cairo_paint(cr);

	/* Print the instructions. */
	cairo_move_to(cr, 5, 15);
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

	draw_string(cr,
	            "Window: %p\n"
	            "Fullscreen? %u\n"
	            "Maximized? %u\n"
	            "Transient? %u\n"
	            "Keys: (f)ullscreen, (m)aximize,\n"
	            "      (n)ew window, (p)opup,\n"
	            "      (q)uit, (t)ransient window\n",
	            window, window_is_fullscreen(window),
	            window_is_maximized(window), window_is_transient(window));

	cairo_destroy(cr);
}

int
main(int argc, char *argv[])
{
	struct stacking stacking;

	memset(&stacking, 0, sizeof stacking);

#ifdef HAVE_PANGO
	g_type_init();
#endif

	stacking.display = display_create(&argc, argv);
	if (stacking.display == NULL) {
		fprintf(stderr, "Failed to create display: %m\n");
		return -1;
	}

	display_set_user_data(stacking.display, &stacking);

	stacking.root_window = new_window(&stacking, NULL);

	display_run(stacking.display);

	return 0;
}
