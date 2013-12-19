/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 Collabora, Ltd.
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

#include "weston-test-client-helper.h"

static void
check_pointer(struct client *client, int x, int y)
{
	int sx, sy;

	/* check that the client got the global pointer update */
	assert(client->test->pointer_x == x);
	assert(client->test->pointer_y == y);

	/* Does global pointer map onto the surface? */
	if (surface_contains(client->surface, x, y)) {
		/* check that the surface has the pointer focus */
		assert(client->input->pointer->focus == client->surface);

		/*
		 * check that the local surface pointer maps
		 * to the global pointer.
		 */
		sx = client->input->pointer->x + client->surface->x;
		sy = client->input->pointer->y + client->surface->y;
		assert(sx == x);
		assert(sy == y);
	} else {
		/*
		 * The global pointer does not map onto surface.  So
		 * check that it doesn't have the pointer focus.
		 */
		assert(client->input->pointer->focus == NULL);
	}
}

static void
check_pointer_move(struct client *client, int x, int y)
{
	wl_test_move_pointer(client->test->wl_test, x, y);
	client_roundtrip(client);
	check_pointer(client, x, y);
}

TEST(test_pointer_top_left)
{
	struct client *client;
	int x, y;

	client = client_create(46, 76, 111, 134);
	assert(client);

	/* move pointer outside top left */
	x = client->surface->x - 1;
	y = client->surface->y - 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on top left */
	x += 1; y += 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside top left */
	x -= 1; y -= 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_bottom_left)
{
	struct client *client;
	int x, y;

	client = client_create(99, 100, 100, 98);
	assert(client);

	/* move pointer outside bottom left */
	x = client->surface->x - 1;
	y = client->surface->y + client->surface->height;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on bottom left */
	x += 1; y -= 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside bottom left */
	x -= 1; y += 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_top_right)
{
	struct client *client;
	int x, y;

	client = client_create(48, 100, 67, 100);
	assert(client);

	/* move pointer outside top right */
	x = client->surface->x + client->surface->width;
	y = client->surface->y - 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on top right */
	x -= 1; y += 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside top right */
	x += 1; y -= 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_bottom_right)
{
	struct client *client;
	int x, y;

	client = client_create(100, 123, 100, 69);
	assert(client);

	/* move pointer outside bottom right */
	x = client->surface->x + client->surface->width;
	y = client->surface->y + client->surface->height;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on bottom right */
	x -= 1; y -= 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside bottom right */
	x += 1; y += 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_top_center)
{
	struct client *client;
	int x, y;

	client = client_create(100, 201, 100, 50);
	assert(client);

	/* move pointer outside top center */
	x = client->surface->x + client->surface->width/2;
	y = client->surface->y - 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on top center */
	y += 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside top center */
	y -= 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_bottom_center)
{
	struct client *client;
	int x, y;

	client = client_create(100, 45, 67, 100);
	assert(client);

	/* move pointer outside bottom center */
	x = client->surface->x + client->surface->width/2;
	y = client->surface->y + client->surface->height;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on bottom center */
	y -= 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside bottom center */
	y += 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_left_center)
{
	struct client *client;
	int x, y;

	client = client_create(167, 45, 78, 100);
	assert(client);

	/* move pointer outside left center */
	x = client->surface->x - 1;
	y = client->surface->y + client->surface->height/2;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on left center */
	x += 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside left center */
	x -= 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_right_center)
{
	struct client *client;
	int x, y;

	client = client_create(110, 37, 100, 46);
	assert(client);

	/* move pointer outside right center */
	x = client->surface->x + client->surface->width;
	y = client->surface->y + client->surface->height/2;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer on right center */
	x -= 1;
	assert(surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);

	/* move pointer outside right center */
	x += 1;
	assert(!surface_contains(client->surface, x, y));
	check_pointer_move(client, x, y);
}

TEST(test_pointer_surface_move)
{
	struct client *client;

	client = client_create(100, 100, 100, 100);
	assert(client);

	/* move pointer outside of client */
	assert(!surface_contains(client->surface, 50, 50));
	check_pointer_move(client, 50, 50);

	/* move client center to pointer */
	move_client(client, 0, 0);
	assert(surface_contains(client->surface, 50, 50));
	check_pointer(client, 50, 50);
}

static int
output_contains_client(struct client *client)
{
	struct output *output = client->output;
	struct surface *surface = client->surface;

	return !(output->x >= surface->x + surface->width
		|| output->x + output->width <= surface->x
		|| output->y >= surface->y + surface->height
		|| output->y + output->height <= surface->y);
}

static void
check_client_move(struct client *client, int x, int y)
{
	move_client(client, x, y);

	if (output_contains_client(client)) {
		assert(client->surface->output == client->output);
	} else {
		assert(client->surface->output == NULL);
	}
}

TEST(test_surface_output)
{
	struct client *client;
	int x, y;

	client = client_create(100, 100, 100, 100);
	assert(client);

	assert(output_contains_client(client));

	/* not visible */
	x = 0;
	y = -client->surface->height;
	check_client_move(client, x, y);

	/* visible */
	check_client_move(client, x, ++y);

	/* not visible */
	x = -client->surface->width;
	y = 0;
	check_client_move(client, x, y);

	/* visible */
	check_client_move(client, ++x, y);

	/* not visible */
	x = client->output->width;
	y = 0;
	check_client_move(client, x, y);

	/* visible */
	check_client_move(client, --x, y);
	assert(output_contains_client(client));

	/* not visible */
	x = 0;
	y = client->output->height;
	check_client_move(client, x, y);
	assert(!output_contains_client(client));

	/* visible */
	check_client_move(client, x, --y);
	assert(output_contains_client(client));
}

static void
buffer_release_handler(void *data, struct wl_buffer *buffer)
{
	int *released = data;

	*released = 1;
}

static struct wl_buffer_listener buffer_listener = {
	buffer_release_handler
};

TEST(buffer_release)
{
	struct client *client;
	struct wl_surface *surface;
	struct wl_buffer *buf1;
	struct wl_buffer *buf2;
	struct wl_buffer *buf3;
	int buf1_released = 0;
	int buf2_released = 0;
	int buf3_released = 0;
	int frame;

	client = client_create(100, 100, 100, 100);
	assert(client);
	surface = client->surface->wl_surface;

	buf1 = create_shm_buffer(client, 100, 100, NULL);
	wl_buffer_add_listener(buf1, &buffer_listener, &buf1_released);

	buf2 = create_shm_buffer(client, 100, 100, NULL);
	wl_buffer_add_listener(buf2, &buffer_listener, &buf2_released);

	buf3 = create_shm_buffer(client, 100, 100, NULL);
	wl_buffer_add_listener(buf3, &buffer_listener, &buf3_released);

	/*
	 * buf1 must never be released, since it is replaced before
	 * it is committed, therefore it never becomes busy.
	 */

	wl_surface_attach(surface, buf1, 0, 0);
	wl_surface_attach(surface, buf2, 0, 0);
	frame_callback_set(surface, &frame);
	wl_surface_commit(surface);
	frame_callback_wait(client, &frame);
	assert(buf1_released == 0);
	/* buf2 may or may not be released */
	assert(buf3_released == 0);

	wl_surface_attach(surface, buf3, 0, 0);
	frame_callback_set(surface, &frame);
	wl_surface_commit(surface);
	frame_callback_wait(client, &frame);
	assert(buf1_released == 0);
	assert(buf2_released == 1);
	/* buf3 may or may not be released */

	wl_surface_attach(surface, client->surface->wl_buffer, 0, 0);
	frame_callback_set(surface, &frame);
	wl_surface_commit(surface);
	frame_callback_wait(client, &frame);
	assert(buf1_released == 0);
	assert(buf2_released == 1);
	assert(buf3_released == 1);
}
