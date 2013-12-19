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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <linux/input.h>
#include <cairo.h>

#include <pango/pangocairo.h>

#include "window.h"
#include "text-client-protocol.h"

struct text_entry {
	struct widget *widget;
	struct window *window;
	char *text;
	int active;
	uint32_t cursor;
	uint32_t anchor;
	struct {
		char *text;
		int32_t cursor;
		char *commit;
		PangoAttrList *attr_list;
	} preedit;
	struct {
		PangoAttrList *attr_list;
		int32_t cursor;
	} preedit_info;
	struct {
		int32_t cursor;
		int32_t anchor;
		uint32_t delete_index;
		uint32_t delete_length;
		bool invalid_delete;
	} pending_commit;
	struct wl_text_input *text_input;
	PangoLayout *layout;
	struct {
		xkb_mod_mask_t shift_mask;
	} keysym;
	uint32_t serial;
	uint32_t reset_serial;
	uint32_t content_purpose;
	uint32_t click_to_show;
	char *preferred_language;
	bool button_pressed;
};

struct editor {
	struct wl_text_input_manager *text_input_manager;
	struct display *display;
	struct window *window;
	struct widget *widget;
	struct text_entry *entry;
	struct text_entry *editor;
	struct text_entry *active_entry;
};

static const char *
utf8_end_char(const char *p)
{
	while ((*p & 0xc0) == 0x80)
		p++;
	return p;
}

static const char *
utf8_prev_char(const char *s, const char *p)
{
	for (--p; p >= s; --p) {
		if ((*p & 0xc0) != 0x80)
			return p;
	}
	return NULL;
}

static const char *
utf8_next_char(const char *p)
{
	if (*p != 0)
		return utf8_end_char(++p);
	return NULL;
}

static void text_entry_redraw_handler(struct widget *widget, void *data);
static void text_entry_button_handler(struct widget *widget,
				      struct input *input, uint32_t time,
				      uint32_t button,
				      enum wl_pointer_button_state state, void *data);
static int text_entry_motion_handler(struct widget *widget,
				     struct input *input, uint32_t time,
				     float x, float y, void *data);
static void text_entry_insert_at_cursor(struct text_entry *entry, const char *text,
					int32_t cursor, int32_t anchor);
static void text_entry_set_preedit(struct text_entry *entry,
				   const char *preedit_text,
				   int preedit_cursor);
static void text_entry_delete_text(struct text_entry *entry,
				   uint32_t index, uint32_t length);
static void text_entry_delete_selected_text(struct text_entry *entry);
static void text_entry_reset_preedit(struct text_entry *entry);
static void text_entry_commit_and_reset(struct text_entry *entry);
static void text_entry_get_cursor_rectangle(struct text_entry *entry, struct rectangle *rectangle);
static void text_entry_update(struct text_entry *entry);

static void
text_input_commit_string(void *data,
			 struct wl_text_input *text_input,
			 uint32_t serial,
			 const char *text)
{
	struct text_entry *entry = data;

	if ((entry->serial - serial) > (entry->serial - entry->reset_serial)) {
		fprintf(stderr, "Ignore commit. Serial: %u, Current: %u, Reset: %u\n",
			serial, entry->serial, entry->reset_serial);
		return;
	}

	if (entry->pending_commit.invalid_delete) {
		fprintf(stderr, "Ignore commit. Invalid previous delete_surrounding event.\n");
		memset(&entry->pending_commit, 0, sizeof entry->pending_commit);
		return;
	}

	text_entry_reset_preedit(entry);

	if (entry->pending_commit.delete_length) {
		text_entry_delete_text(entry,
				       entry->pending_commit.delete_index,
				       entry->pending_commit.delete_length);
	} else {
		text_entry_delete_selected_text(entry);
	}

	text_entry_insert_at_cursor(entry, text,
				    entry->pending_commit.cursor,
				    entry->pending_commit.anchor);

	memset(&entry->pending_commit, 0, sizeof entry->pending_commit);

	widget_schedule_redraw(entry->widget);
}

static void
clear_pending_preedit(struct text_entry *entry)
{
	memset(&entry->pending_commit, 0, sizeof entry->pending_commit);

	pango_attr_list_unref(entry->preedit_info.attr_list);

	entry->preedit_info.cursor = 0;
	entry->preedit_info.attr_list = NULL;

	memset(&entry->preedit_info, 0, sizeof entry->preedit_info);
}

static void
text_input_preedit_string(void *data,
			  struct wl_text_input *text_input,
			  uint32_t serial,
			  const char *text,
			  const char *commit)
{
	struct text_entry *entry = data;

	if ((entry->serial - serial) > (entry->serial - entry->reset_serial)) {
		fprintf(stderr, "Ignore preedit_string. Serial: %u, Current: %u, Reset: %u\n",
			serial, entry->serial, entry->reset_serial);
		clear_pending_preedit(entry);
		return;
	}

	if (entry->pending_commit.invalid_delete) {
		fprintf(stderr, "Ignore preedit_string. Invalid previous delete_surrounding event.\n");
		clear_pending_preedit(entry);
		return;
	}

	if (entry->pending_commit.delete_length) {
		text_entry_delete_text(entry,
				       entry->pending_commit.delete_index,
				       entry->pending_commit.delete_length);
	} else {
		text_entry_delete_selected_text(entry);
	}

	text_entry_set_preedit(entry, text, entry->preedit_info.cursor);
	entry->preedit.commit = strdup(commit);
	entry->preedit.attr_list = pango_attr_list_ref(entry->preedit_info.attr_list);

	clear_pending_preedit(entry);

	text_entry_update(entry);

	widget_schedule_redraw(entry->widget);
}

static void
text_input_delete_surrounding_text(void *data,
				   struct wl_text_input *text_input,
				   int32_t index,
				   uint32_t length)
{
	struct text_entry *entry = data;
	uint32_t text_length;

	entry->pending_commit.delete_index = entry->cursor + index;
	entry->pending_commit.delete_length = length;
	entry->pending_commit.invalid_delete = false;

	text_length = strlen(entry->text);

	if (entry->pending_commit.delete_index > text_length ||
	    length > text_length ||
	    entry->pending_commit.delete_index + length > text_length) {
		fprintf(stderr, "delete_surrounding_text: Invalid index: %d," \
			"length %u'; cursor: %u text length: %u\n", index, length, entry->cursor, text_length);
		entry->pending_commit.invalid_delete = true;
		return;
	}
}

static void
text_input_cursor_position(void *data,
			   struct wl_text_input *text_input,
			   int32_t index,
			   int32_t anchor)
{
	struct text_entry *entry = data;

	entry->pending_commit.cursor = index;
	entry->pending_commit.anchor = anchor;
}

static void
text_input_preedit_styling(void *data,
			   struct wl_text_input *text_input,
			   uint32_t index,
			   uint32_t length,
			   uint32_t style)
{
	struct text_entry *entry = data;
	PangoAttribute *attr1 = NULL;
	PangoAttribute *attr2 = NULL;

	if (!entry->preedit_info.attr_list)
		entry->preedit_info.attr_list = pango_attr_list_new();

	switch (style) {
		case WL_TEXT_INPUT_PREEDIT_STYLE_DEFAULT:
		case WL_TEXT_INPUT_PREEDIT_STYLE_UNDERLINE:
			attr1 = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
			break;
		case WL_TEXT_INPUT_PREEDIT_STYLE_INCORRECT:
			attr1 = pango_attr_underline_new(PANGO_UNDERLINE_ERROR);
			attr2 = pango_attr_underline_color_new(65535, 0, 0);
			break;
		case WL_TEXT_INPUT_PREEDIT_STYLE_SELECTION:
			attr1 = pango_attr_background_new(0.3 * 65535, 0.3 * 65535, 65535);
			attr2 = pango_attr_foreground_new(65535, 65535, 65535);
			break;
		case WL_TEXT_INPUT_PREEDIT_STYLE_HIGHLIGHT:
		case WL_TEXT_INPUT_PREEDIT_STYLE_ACTIVE:
			attr1 = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
			attr2 = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
			break;
		case WL_TEXT_INPUT_PREEDIT_STYLE_INACTIVE:
			attr1 = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
			attr2 = pango_attr_foreground_new(0.3 * 65535, 0.3 * 65535, 0.3 * 65535);
			break;
	}

	if (attr1) {
		attr1->start_index = entry->cursor + index;
		attr1->end_index = entry->cursor + index + length;
		pango_attr_list_insert(entry->preedit_info.attr_list, attr1);
	}

	if (attr2) {
		attr2->start_index = entry->cursor + index;
		attr2->end_index = entry->cursor + index + length;
		pango_attr_list_insert(entry->preedit_info.attr_list, attr2);
	}
}

static void
text_input_preedit_cursor(void *data,
			  struct wl_text_input *text_input,
			  int32_t index)
{
	struct text_entry *entry = data;

	entry->preedit_info.cursor = index;
}

static void
text_input_modifiers_map(void *data,
			 struct wl_text_input *text_input,
			 struct wl_array *map)
{
	struct text_entry *entry = data;

	entry->keysym.shift_mask = keysym_modifiers_get_mask(map, "Shift");
}

static void
text_input_keysym(void *data,
		  struct wl_text_input *text_input,
		  uint32_t serial,
		  uint32_t time,
		  uint32_t key,
		  uint32_t state,
		  uint32_t modifiers)
{
	struct text_entry *entry = data;
	const char *state_label = "release";
	const char *key_label = "Unknown";
	const char *new_char;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		state_label = "pressed";
	}

	if (key == XKB_KEY_Left ||
	    key == XKB_KEY_Right) {
		if (state != WL_KEYBOARD_KEY_STATE_RELEASED)
			return;

		if (key == XKB_KEY_Left)
			new_char = utf8_prev_char(entry->text, entry->text + entry->cursor);
		else
			new_char = utf8_next_char(entry->text + entry->cursor);

		if (new_char != NULL) {
			entry->cursor = new_char - entry->text;
		}

		if (!(modifiers & entry->keysym.shift_mask))
			entry->anchor = entry->cursor;
		widget_schedule_redraw(entry->widget);

		return;
	}

	if (key == XKB_KEY_BackSpace) {
		const char *start, *end;

		if (state != WL_KEYBOARD_KEY_STATE_RELEASED)
			return;

		text_entry_commit_and_reset(entry);

		start = utf8_prev_char(entry->text, entry->text + entry->cursor);
		if (start == NULL)
			return;

		end = utf8_next_char(start);

		text_entry_delete_text(entry,
				       start - entry->text,
				       end - start);

		return;
	}

	switch (key) {
		case XKB_KEY_Tab:
			key_label = "Tab";
			break;
		case XKB_KEY_KP_Enter:
		case XKB_KEY_Return:
			key_label = "Enter";
			break;
	}

	fprintf(stderr, "%s key was %s.\n", key_label, state_label);
}

static void
text_input_enter(void *data,
		 struct wl_text_input *text_input,
		 struct wl_surface *surface)
{
	struct text_entry *entry = data;

	if (surface != window_get_wl_surface(entry->window))
		return;

	entry->active = 1;

	text_entry_update(entry);
	entry->reset_serial = entry->serial;

	widget_schedule_redraw(entry->widget);
}

static void
text_input_leave(void *data,
		 struct wl_text_input *text_input)
{
	struct text_entry *entry = data;

	text_entry_commit_and_reset(entry);

	entry->active = 0;

	wl_text_input_hide_input_panel(text_input);

	widget_schedule_redraw(entry->widget);
}

static void
text_input_input_panel_state(void *data,
			     struct wl_text_input *text_input,
			     uint32_t state)
{
}

static void
text_input_language(void *data,
		    struct wl_text_input *text_input,
		    uint32_t serial,
		    const char *language)
{
	fprintf(stderr, "input language is %s \n", language);
}

static void
text_input_text_direction(void *data,
			  struct wl_text_input *text_input,
			  uint32_t serial,
			  uint32_t direction)
{
	struct text_entry *entry = data;
	PangoContext *context = pango_layout_get_context(entry->layout);
	PangoDirection pango_direction;


	switch (direction) {
		case WL_TEXT_INPUT_TEXT_DIRECTION_LTR:
			pango_direction = PANGO_DIRECTION_LTR;
			break;
		case WL_TEXT_INPUT_TEXT_DIRECTION_RTL:
			pango_direction = PANGO_DIRECTION_RTL;
			break;
		case WL_TEXT_INPUT_TEXT_DIRECTION_AUTO:
		default:
			pango_direction = PANGO_DIRECTION_NEUTRAL;
	}

	pango_context_set_base_dir(context, pango_direction);
}

static const struct wl_text_input_listener text_input_listener = {
	text_input_enter,
	text_input_leave,
	text_input_modifiers_map,
	text_input_input_panel_state,
	text_input_preedit_string,
	text_input_preedit_styling,
	text_input_preedit_cursor,
	text_input_commit_string,
	text_input_cursor_position,
	text_input_delete_surrounding_text,
	text_input_keysym,
	text_input_language,
	text_input_text_direction
};

static struct text_entry*
text_entry_create(struct editor *editor, const char *text)
{
	struct text_entry *entry;

	entry = xmalloc(sizeof *entry);
	memset(entry, 0, sizeof *entry);

	entry->widget = widget_add_widget(editor->widget, entry);
	entry->window = editor->window;
	entry->text = strdup(text);
	entry->active = 0;
	entry->cursor = strlen(text);
	entry->anchor = entry->cursor;
	entry->text_input = wl_text_input_manager_create_text_input(editor->text_input_manager);
	wl_text_input_add_listener(entry->text_input, &text_input_listener, entry);

	widget_set_redraw_handler(entry->widget, text_entry_redraw_handler);
	widget_set_button_handler(entry->widget, text_entry_button_handler);
	widget_set_motion_handler(entry->widget, text_entry_motion_handler);

	return entry;
}

static void
text_entry_destroy(struct text_entry *entry)
{
	widget_destroy(entry->widget);
	wl_text_input_destroy(entry->text_input);
	g_clear_object(&entry->layout);
	free(entry->text);
	free(entry);
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct editor *editor = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;

	surface = window_get_surface(editor->window);
	widget_get_allocation(editor->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_translate(cr, allocation.x, allocation.y);

	/* Draw background */
	cairo_push_group(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
	cairo_fill(cr);

	cairo_pop_group_to_source(cr);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
text_entry_allocate(struct text_entry *entry, int32_t x, int32_t y,
		    int32_t width, int32_t height)
{
	widget_set_allocation(entry->widget, x, y, width, height);
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct editor *editor = data;
	struct rectangle allocation;

	widget_get_allocation(editor->widget, &allocation);

	text_entry_allocate(editor->entry,
			    allocation.x + 20, allocation.y + 20,
			    width - 40, height / 2 - 40);
	text_entry_allocate(editor->editor,
			    allocation.x + 20, allocation.y + height / 2 + 20,
			    width - 40, height / 2 - 40);
}

static void
text_entry_activate(struct text_entry *entry,
		    struct wl_seat *seat)
{
	struct wl_surface *surface = window_get_wl_surface(entry->window);

	if (entry->click_to_show && entry->active) {
		wl_text_input_show_input_panel(entry->text_input);

		return;
	}

	if (!entry->click_to_show)
		wl_text_input_show_input_panel(entry->text_input);

	wl_text_input_activate(entry->text_input,
			       seat,
			       surface);
}

static void
text_entry_deactivate(struct text_entry *entry,
		      struct wl_seat *seat)
{
	wl_text_input_deactivate(entry->text_input,
				 seat);
}

static void
text_entry_update_layout(struct text_entry *entry)
{
	char *text;
	PangoAttrList *attr_list;

	assert(entry->cursor <= (strlen(entry->text) +
	       (entry->preedit.text ? strlen(entry->preedit.text) : 0)));

	if (entry->preedit.text) {
		text = malloc(strlen(entry->text) + strlen(entry->preedit.text) + 1);
		strncpy(text, entry->text, entry->cursor);
		strcpy(text + entry->cursor, entry->preedit.text);
		strcpy(text + entry->cursor + strlen(entry->preedit.text),
		       entry->text + entry->cursor);
	} else {
		text = strdup(entry->text);
	}

	if (entry->cursor != entry->anchor) {
		int start_index = MIN(entry->cursor, entry->anchor);
		int end_index = MAX(entry->cursor, entry->anchor);
		PangoAttribute *attr;

		attr_list = pango_attr_list_copy(entry->preedit.attr_list);

		if (!attr_list)
			attr_list = pango_attr_list_new();

		attr = pango_attr_background_new(0.3 * 65535, 0.3 * 65535, 65535);
		attr->start_index = start_index;
		attr->end_index = end_index;
		pango_attr_list_insert(attr_list, attr);

		attr = pango_attr_foreground_new(65535, 65535, 65535);
		attr->start_index = start_index;
		attr->end_index = end_index;
		pango_attr_list_insert(attr_list, attr);
	} else {
		attr_list = pango_attr_list_ref(entry->preedit.attr_list);
	}

	if (entry->preedit.text && !entry->preedit.attr_list) {
		PangoAttribute *attr;

		if (!attr_list)
			attr_list = pango_attr_list_new();

		attr = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
		attr->start_index = entry->cursor;
		attr->end_index = entry->cursor + strlen(entry->preedit.text);
		pango_attr_list_insert(attr_list, attr);
	}

	if (entry->layout) {
		pango_layout_set_text(entry->layout, text, -1);
		pango_layout_set_attributes(entry->layout, attr_list);
	}

	free(text);
	pango_attr_list_unref(attr_list);
}

static void
text_entry_update(struct text_entry *entry)
{
	struct rectangle cursor_rectangle;

	wl_text_input_set_content_type(entry->text_input,
				       WL_TEXT_INPUT_CONTENT_HINT_NONE,
				       entry->content_purpose);

	wl_text_input_set_surrounding_text(entry->text_input,
					   entry->text,
					   entry->cursor,
					   entry->anchor);

	if (entry->preferred_language)
		wl_text_input_set_preferred_language(entry->text_input,
						     entry->preferred_language);

	text_entry_get_cursor_rectangle(entry, &cursor_rectangle);
	wl_text_input_set_cursor_rectangle(entry->text_input, cursor_rectangle.x, cursor_rectangle.y,
					   cursor_rectangle.width, cursor_rectangle.height);

	wl_text_input_commit_state(entry->text_input, ++entry->serial);
}

static void
text_entry_insert_at_cursor(struct text_entry *entry, const char *text,
			    int32_t cursor, int32_t anchor)
{
	char *new_text = malloc(strlen(entry->text) + strlen(text) + 1);

	strncpy(new_text, entry->text, entry->cursor);
	strcpy(new_text + entry->cursor, text);
	strcpy(new_text + entry->cursor + strlen(text),
	       entry->text + entry->cursor);

	free(entry->text);
	entry->text = new_text;
	if (anchor >= 0)
		entry->anchor = entry->cursor + strlen(text) + anchor;
	else
		entry->anchor = entry->cursor + 1 + anchor;

	if (cursor >= 0)
		entry->cursor += strlen(text) + cursor;
	else
		entry->cursor += 1 + cursor;

	text_entry_update_layout(entry);

	widget_schedule_redraw(entry->widget);

	text_entry_update(entry);
}

static void
text_entry_reset_preedit(struct text_entry *entry)
{
	entry->preedit.cursor = 0;

	free(entry->preedit.text);
	entry->preedit.text = NULL;

	free(entry->preedit.commit);
	entry->preedit.commit = NULL;

	pango_attr_list_unref(entry->preedit.attr_list);
	entry->preedit.attr_list = NULL;
}

static void
text_entry_commit_and_reset(struct text_entry *entry)
{
	char *commit = NULL;

	if (entry->preedit.commit)
		commit = strdup(entry->preedit.commit);

	text_entry_reset_preedit(entry);
	if (commit) {
		text_entry_insert_at_cursor(entry, commit, 0, 0);
		free(commit);
	}

	wl_text_input_reset(entry->text_input);
	text_entry_update(entry);
	entry->reset_serial = entry->serial;
}

static void
text_entry_set_preedit(struct text_entry *entry,
		       const char *preedit_text,
		       int preedit_cursor)
{
	text_entry_reset_preedit(entry);

	if (!preedit_text)
		return;

	entry->preedit.text = strdup(preedit_text);
	entry->preedit.cursor = preedit_cursor;

	text_entry_update_layout(entry);

	widget_schedule_redraw(entry->widget);
}

static uint32_t
text_entry_try_invoke_preedit_action(struct text_entry *entry,
				     int32_t x, int32_t y,
				     uint32_t button,
				     enum wl_pointer_button_state state)
{
	int index, trailing;
	uint32_t cursor;
	const char *text;

	if (!entry->preedit.text)
		return 0;

	pango_layout_xy_to_index(entry->layout,
				 x * PANGO_SCALE, y * PANGO_SCALE,
				 &index, &trailing);

	text = pango_layout_get_text(entry->layout);
	cursor = g_utf8_offset_to_pointer(text + index, trailing) - text;

	if (cursor < entry->cursor ||
	    cursor > entry->cursor + strlen(entry->preedit.text)) {
		return 0;
	}

	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		wl_text_input_invoke_action(entry->text_input,
					    button,
					    cursor - entry->cursor);

	return 1;
}

static bool
text_entry_has_preedit(struct text_entry *entry)
{
	return entry->preedit.text && (strlen(entry->preedit.text) > 0);
}

static void
text_entry_set_cursor_position(struct text_entry *entry,
			       int32_t x, int32_t y,
			       bool move_anchor)
{
	int index, trailing;
	const char *text;
	uint32_t cursor;

	pango_layout_xy_to_index(entry->layout,
				 x * PANGO_SCALE, y * PANGO_SCALE,
				 &index, &trailing);

	text = pango_layout_get_text(entry->layout);

	cursor = g_utf8_offset_to_pointer(text + index, trailing) - text;

	if (move_anchor)
		entry->anchor = cursor;

	if (text_entry_has_preedit(entry)) {
		text_entry_commit_and_reset(entry);

		assert(!text_entry_has_preedit(entry));
	}

	if (entry->cursor == cursor)
		return;

	entry->cursor = cursor;

	text_entry_update_layout(entry);

	widget_schedule_redraw(entry->widget);

	text_entry_update(entry);
}

static void
text_entry_delete_text(struct text_entry *entry,
		       uint32_t index, uint32_t length)
{
	uint32_t l;

	assert(index <= strlen(entry->text));
	assert(index + length <= strlen(entry->text));
	assert(index + length >= length);

	l = strlen(entry->text + index + length);
	memmove(entry->text + index,
		entry->text + index + length,
		l + 1);

	if (entry->cursor > (index + length))
		entry->cursor -= length;
	else if (entry->cursor > index)
		entry->cursor = index;

	entry->anchor = entry->cursor;

	text_entry_update_layout(entry);

	widget_schedule_redraw(entry->widget);

	text_entry_update(entry);
}

static void
text_entry_delete_selected_text(struct text_entry *entry)
{
	uint32_t start_index = entry->anchor < entry->cursor ? entry->anchor : entry->cursor;
	uint32_t end_index = entry->anchor < entry->cursor ? entry->cursor : entry->anchor;

	if (entry->anchor == entry->cursor)
		return;

	text_entry_delete_text(entry, start_index, end_index - start_index);

	entry->anchor = entry->cursor;
}

static void
text_entry_get_cursor_rectangle(struct text_entry *entry, struct rectangle *rectangle)
{
	struct rectangle allocation;
	PangoRectangle extents;
	PangoRectangle cursor_pos;

	widget_get_allocation(entry->widget, &allocation);

	if (entry->preedit.text && entry->preedit.cursor < 0) {
		rectangle->x = 0;
		rectangle->y = 0;
		rectangle->width = 0;
		rectangle->height = 0;
		return;
	}


	pango_layout_get_extents(entry->layout, &extents, NULL);
	pango_layout_get_cursor_pos(entry->layout,
				    entry->cursor + entry->preedit.cursor,
				    &cursor_pos, NULL);

	rectangle->x = allocation.x + (allocation.height / 2) + PANGO_PIXELS(cursor_pos.x);
	rectangle->y = allocation.y + 10 + PANGO_PIXELS(cursor_pos.y);
	rectangle->width = PANGO_PIXELS(cursor_pos.width);
	rectangle->height = PANGO_PIXELS(cursor_pos.height);
}

static void
text_entry_draw_cursor(struct text_entry *entry, cairo_t *cr)
{
	PangoRectangle extents;
	PangoRectangle cursor_pos;

	if (entry->preedit.text && entry->preedit.cursor < 0)
		return;

	pango_layout_get_extents(entry->layout, &extents, NULL);
	pango_layout_get_cursor_pos(entry->layout,
				    entry->cursor + entry->preedit.cursor,
				    &cursor_pos, NULL);

	cairo_set_line_width(cr, 1.0);
	cairo_move_to(cr, PANGO_PIXELS(cursor_pos.x), PANGO_PIXELS(cursor_pos.y));
	cairo_line_to(cr, PANGO_PIXELS(cursor_pos.x), PANGO_PIXELS(cursor_pos.y) + PANGO_PIXELS(cursor_pos.height));
	cairo_stroke(cr);
}

static const int text_offset_left = 10;

static void
text_entry_redraw_handler(struct widget *widget, void *data)
{
	struct text_entry *entry = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;

	surface = window_get_surface(entry->window);
	widget_get_allocation(entry->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

	cairo_push_group(cr);
	cairo_translate(cr, allocation.x, allocation.y);

	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
	cairo_fill(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	if (entry->active) {
		cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
		cairo_set_line_width (cr, 3);
		cairo_set_source_rgba(cr, 0, 0, 1, 1.0);
		cairo_stroke(cr);
	}

	cairo_set_source_rgba(cr, 0, 0, 0, 1);

	cairo_translate(cr, text_offset_left, allocation.height / 2);

	if (!entry->layout)
		entry->layout = pango_cairo_create_layout(cr);
	else
		pango_cairo_update_layout(cr, entry->layout);

	text_entry_update_layout(entry);

	pango_cairo_show_layout(cr, entry->layout);

	text_entry_draw_cursor(entry, cr);

	cairo_pop_group_to_source(cr);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static int
text_entry_motion_handler(struct widget *widget,
			  struct input *input, uint32_t time,
			  float x, float y, void *data)
{
	struct text_entry *entry = data;
	struct rectangle allocation;

	if (!entry->button_pressed) {
		return CURSOR_IBEAM;
	}

	widget_get_allocation(entry->widget, &allocation);

	text_entry_set_cursor_position(entry,
				       x - allocation.x - text_offset_left,
				       y - allocation.y - text_offset_left,
				       false);

	return CURSOR_IBEAM;
}

static void
text_entry_button_handler(struct widget *widget,
			  struct input *input, uint32_t time,
			  uint32_t button,
			  enum wl_pointer_button_state state, void *data)
{
	struct text_entry *entry = data;
	struct rectangle allocation;
	struct editor *editor;
	int32_t x, y;
	uint32_t result;

	widget_get_allocation(entry->widget, &allocation);
	input_get_position(input, &x, &y);

	x -= allocation.x + text_offset_left;
	y -= allocation.y + text_offset_left;

	editor = window_get_user_data(entry->window);

	if (button == BTN_LEFT) {
		entry->button_pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);

		if (state == WL_POINTER_BUTTON_STATE_PRESSED)
			input_grab(input, entry->widget, button);
		else
			input_ungrab(input);
	}

	if (text_entry_has_preedit(entry)) {
		result = text_entry_try_invoke_preedit_action(entry, x, y, button, state);

		if (result)
			return;
	}

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct wl_seat *seat = input_get_seat(input);

		text_entry_activate(entry, seat);
		editor->active_entry = entry;

		text_entry_set_cursor_position(entry, x, y, true);
	}
}

static void
editor_button_handler(struct widget *widget,
		      struct input *input, uint32_t time,
		      uint32_t button,
		      enum wl_pointer_button_state state, void *data)
{
	struct editor *editor = data;

	if (button != BTN_LEFT) {
		return;
	}

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct wl_seat *seat = input_get_seat(input);

		text_entry_deactivate(editor->entry, seat);
		text_entry_deactivate(editor->editor, seat);
		editor->active_entry = NULL;
	}
}

static void
key_handler(struct window *window,
	    struct input *input, uint32_t time,
	    uint32_t key, uint32_t sym, enum wl_keyboard_key_state state,
	    void *data)
{
	struct editor *editor = data;
	struct text_entry *entry;
	const char *new_char;
	char text[16];

	if (!editor->active_entry)
		return;

	entry = editor->active_entry;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	switch (sym) {
		case XKB_KEY_BackSpace:
			text_entry_commit_and_reset(entry);

			new_char = utf8_prev_char(entry->text, entry->text + entry->cursor);
			if (new_char != NULL)
				text_entry_delete_text(entry,
						       new_char - entry->text,
						       (entry->text + entry->cursor) - new_char);
			break;
		case XKB_KEY_Delete:
			text_entry_commit_and_reset(entry);

			new_char = utf8_next_char(entry->text + entry->cursor);
			if (new_char != NULL)
				text_entry_delete_text(entry,
						       entry->cursor,
						       new_char - (entry->text + entry->cursor));
			break;
		case XKB_KEY_Left:
			text_entry_commit_and_reset(entry);

			new_char = utf8_prev_char(entry->text, entry->text + entry->cursor);
			if (new_char != NULL) {
				entry->cursor = new_char - entry->text;
				if (!(input_get_modifiers(input) & MOD_SHIFT_MASK))
					entry->anchor = entry->cursor;
				widget_schedule_redraw(entry->widget);
			}
			break;
		case XKB_KEY_Right:
			text_entry_commit_and_reset(entry);

			new_char = utf8_next_char(entry->text + entry->cursor);
			if (new_char != NULL) {
				entry->cursor = new_char - entry->text;
				if (!(input_get_modifiers(input) & MOD_SHIFT_MASK))
					entry->anchor = entry->cursor;
				widget_schedule_redraw(entry->widget);
			}
			break;
		case XKB_KEY_Escape:
			break;
		default:
			if (xkb_keysym_to_utf8(sym, text, sizeof(text)) <= 0)
				break;

 			text_entry_commit_and_reset(entry);

			text_entry_insert_at_cursor(entry, text, 0, 0);
			break;
	}

	widget_schedule_redraw(entry->widget);
}

static void
global_handler(struct display *display, uint32_t name,
	       const char *interface, uint32_t version, void *data)
{
	struct editor *editor = data;

	if (!strcmp(interface, "wl_text_input_manager")) {
		editor->text_input_manager =
			display_bind(display, name,
				     &wl_text_input_manager_interface, 1);
	}
}

int
main(int argc, char *argv[])
{
	struct editor editor;
	int i;
	uint32_t click_to_show = 0;
	const char *preferred_language = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp("--click-to-show", argv[i]) == 0)
			click_to_show = 1;
		else if (strcmp("--preferred-language", argv[i]) == 0) {
			if (i + 1 < argc) {
				preferred_language = argv[i + 1];
				i++;
			}
		}
	}

	memset(&editor, 0, sizeof editor);

#ifdef HAVE_PANGO
	g_type_init();
#endif

	editor.display = display_create(&argc, argv);
	if (editor.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	display_set_user_data(editor.display, &editor);
	display_set_global_handler(editor.display, global_handler);

	editor.window = window_create(editor.display);
	editor.widget = window_frame_create(editor.window, &editor);

	editor.entry = text_entry_create(&editor, "Entry");
	editor.entry->click_to_show = click_to_show;
	if (preferred_language)
		editor.entry->preferred_language = strdup(preferred_language);
	editor.editor = text_entry_create(&editor, "Numeric");
	editor.editor->content_purpose = WL_TEXT_INPUT_CONTENT_PURPOSE_NUMBER;
	editor.editor->click_to_show = click_to_show;

	window_set_title(editor.window, "Text Editor");
	window_set_key_handler(editor.window, key_handler);
	window_set_user_data(editor.window, &editor);

	widget_set_redraw_handler(editor.widget, redraw_handler);
	widget_set_resize_handler(editor.widget, resize_handler);
	widget_set_button_handler(editor.widget, editor_button_handler);

	window_schedule_resize(editor.window, 500, 400);

	display_run(editor.display);

	text_entry_destroy(editor.entry);
	text_entry_destroy(editor.editor);

	return 0;
}
