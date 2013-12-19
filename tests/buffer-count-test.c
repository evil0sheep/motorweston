/*
 * Copyright © 2013 Intel Corporation
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

#include <string.h>

#include "weston-test-client-helper.h"
#include <stdio.h>
#include <poll.h>
#include <time.h>

#include <EGL/egl.h>
#include <wayland-egl.h>
#include <GLES2/gl2.h>

struct test_data {
	struct client *client;

	EGLDisplay egl_dpy;
	EGLContext egl_ctx;
	EGLConfig egl_conf;
	EGLSurface egl_surface;
};

static void
init_egl(struct test_data *test_data)
{
	struct wl_egl_window *native_window;
	struct surface *surface = test_data->client->surface;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint major, minor, n;
	EGLBoolean ret;

	test_data->egl_dpy = eglGetDisplay((EGLNativeDisplayType)
					   test_data->client->wl_display);
	assert(test_data->egl_dpy);

	ret = eglInitialize(test_data->egl_dpy, &major, &minor);
	assert(ret == EGL_TRUE);
	ret = eglBindAPI(EGL_OPENGL_ES_API);
	assert(ret == EGL_TRUE);

	ret = eglChooseConfig(test_data->egl_dpy, config_attribs,
			      &test_data->egl_conf, 1, &n);
	assert(ret && n == 1);

	test_data->egl_ctx = eglCreateContext(test_data->egl_dpy,
					      test_data->egl_conf,
					      EGL_NO_CONTEXT, context_attribs);
	assert(test_data->egl_ctx);

	native_window =
		wl_egl_window_create(surface->wl_surface,
				     surface->width,
				     surface->height);
	test_data->egl_surface =
		eglCreateWindowSurface(test_data->egl_dpy,
				       test_data->egl_conf,
				       (EGLNativeWindowType) native_window,
				       NULL);

	ret = eglMakeCurrent(test_data->egl_dpy, test_data->egl_surface,
			     test_data->egl_surface, test_data->egl_ctx);
	assert(ret == EGL_TRUE);
}

TEST(test_buffer_count)
{
	struct test_data test_data;
	uint32_t buffer_count;
	int i;

	test_data.client = client_create(10, 10, 10, 10);
	init_egl(&test_data);

	/* This is meant to represent a typical game loop which is
	 * expecting eglSwapBuffers to block and throttle the
	 * rendering to a sensible frame rate. Therefore it doesn't
	 * expect to have to install a frame callback itself. I'd
	 * imagine this is what a typical SDL game would end up
	 * doing */

	for (i = 0; i < 10; i++) {
		glClear(GL_COLOR_BUFFER_BIT);
		eglSwapBuffers(test_data.egl_dpy, test_data.egl_surface);
	}

	buffer_count = get_n_egl_buffers(test_data.client);

	printf("buffers used = %i\n", buffer_count);

	/* The implementation should only end up creating two buffers
	 * and cycling between them */
	assert(buffer_count == 2);
}
