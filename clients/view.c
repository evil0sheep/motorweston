/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2009 Chris Wilson
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <cairo.h>
#include <glib.h>
#include <gio/gio.h>
#include <linux/input.h>

#include <glib/poppler-document.h>
#include <glib/poppler-page.h>

#include <wayland-client.h>

#include "window.h"

struct view {
	struct window *window;
	struct widget *widget;
	struct display *display;

	PopplerDocument *document;
	int page;
	int fullscreen;
	int *view_counter;
};

static void
redraw_handler(struct widget *widget, void *data)
{
	struct view *view = data;

	struct rectangle allocation;
	cairo_surface_t *surface;
	cairo_t *cr;
	PopplerPage *page;
	double width, height, doc_aspect, window_aspect, scale;

	widget_get_allocation(view->widget, &allocation);

	surface = window_get_surface(view->window);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y,
			 allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

        if(!view->document) {
                cairo_destroy(cr);
                cairo_surface_destroy(surface);
                return;
        }

	page = poppler_document_get_page(view->document, view->page);
	poppler_page_get_size(page, &width, &height);
	doc_aspect = width / height;
	window_aspect = (double) allocation.width / allocation.height;
	if (doc_aspect < window_aspect)
		scale = allocation.height / height;
	else
		scale = allocation.width / width;
	cairo_translate(cr, allocation.x, allocation.y);
	cairo_scale(cr, scale, scale);
	cairo_translate(cr,
			(allocation.width - width * scale) / 2 / scale,
			(allocation.height - height * scale) / 2 / scale);
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);
	poppler_page_render(page, cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	g_object_unref(G_OBJECT(page));
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct view *view = data;

	widget_set_size(view->widget, width, height);
}

static void
view_page_up(struct view *view)
{
        if(view->page <= 0)
                return;

        view->page--;
        window_schedule_redraw(view->window);
}

static void
view_page_down(struct view *view)
{
        if(!view->document ||
           view->page >= poppler_document_get_n_pages(view->document) - 1) {
                return;
        }

        view->page++;
        window_schedule_redraw(view->window);
}

static void
button_handler(struct widget *widget, struct input *input, uint32_t time,
               uint32_t button, enum wl_pointer_button_state state,
	       void *data)
{
        struct view *view = data;

        if (state == WL_POINTER_BUTTON_STATE_RELEASED)
                return;

        switch(button) {
        case 275:
                view_page_up(view);
                break;
        case 276:
                view_page_down(view);
                break;
        default:
                break;
        }
}

static void
fullscreen_handler(struct window *window, void *data)
{
	struct view *view = data;

	view->fullscreen ^= 1;
	window_set_fullscreen(window, view->fullscreen);
}

static void
close_handler(void *data)
{
	struct view *view = data;

	*view->view_counter -= 1;
	if (*view->view_counter == 0)
		display_exit(view->display);

	widget_destroy(view->widget);
	window_destroy(view->window);
	if (view->document)
		g_object_unref(view->document);

	free(view);
}

static void
key_handler(struct window *window, struct input *input, uint32_t time,
	    uint32_t key, uint32_t unicode,
	    enum wl_keyboard_key_state state, void *data)
{
	struct view *view = data;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
	        return;

	switch (key) {
	case KEY_SPACE:
	case KEY_PAGEDOWN:
	case KEY_RIGHT:
	case KEY_DOWN:
                view_page_down(view);
		break;
	case KEY_BACKSPACE:
	case KEY_PAGEUP:
	case KEY_LEFT:
	case KEY_UP:
                view_page_up(view);
		break;
	default:
		break;
	}
}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	struct view *view = data;
	window_schedule_redraw(view->window);
}

static struct view *
view_create(struct display *display,
	    uint32_t key, const char *filename, int fullscreen, int *view_counter)
{
	struct view *view;
	gchar *basename;
	gchar *title;
	GFile *file = NULL;
	GError *error = NULL;

	view = zalloc(sizeof *view);
	if (view == NULL)
		return view;

	file = g_file_new_for_commandline_arg(filename);
	basename = g_file_get_basename(file);
	if(!basename) {
		title = g_strdup("Wayland View");
	} else {
	        title = g_strdup_printf("Wayland View - %s", basename);
	        g_free(basename);
	}

        view->document = poppler_document_new_from_file(g_file_get_uri(file),
                                                        NULL, &error);

        if(error) {
		title = g_strdup("File not found");
        }

	view->window = window_create(display);
	view->widget = window_frame_create(view->window, view);
	window_set_title(view->window, title);
	g_free(title);
	view->display = display;

	window_set_user_data(view->window, view);
	window_set_key_handler(view->window, key_handler);
	window_set_keyboard_focus_handler(view->window,
					  keyboard_focus_handler);
	window_set_fullscreen_handler(view->window, fullscreen_handler);
	window_set_close_handler(view->window, close_handler);

	widget_set_button_handler(view->widget, button_handler);
	widget_set_resize_handler(view->widget, resize_handler);
	widget_set_redraw_handler(view->widget, redraw_handler);

	view->page = 0;

	view->fullscreen = fullscreen;
	window_set_fullscreen(view->window, view->fullscreen);

	window_schedule_resize(view->window, 500, 400);
	view->view_counter = view_counter;
	*view_counter += 1;

	return view;
}

static int option_fullscreen;

static const struct weston_option view_options[] = {
	{ WESTON_OPTION_BOOLEAN, "fullscreen", 0, &option_fullscreen },
};

int
main(int argc, char *argv[])
{
	struct display *d;
	int i;
	int view_counter = 0;

#if !GLIB_CHECK_VERSION(2, 35, 0)
	g_type_init();
#endif

	parse_options(view_options, ARRAY_LENGTH(view_options), &argc, argv);

	d = display_create(&argc, argv);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	for (i = 1; i < argc; i++)
		view_create (d, i, argv[i], option_fullscreen, &view_counter);

	if (view_counter > 0)
		display_run(d);

	return 0;
}
