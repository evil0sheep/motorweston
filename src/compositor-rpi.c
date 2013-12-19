/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2012-2013 Raspberry Pi Foundation
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

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <libudev.h>

#ifdef HAVE_BCM_HOST
#  include <bcm_host.h>
#else
#  include "rpi-bcm-stubs.h"
#endif

#include "compositor.h"
#include "rpi-renderer.h"
#include "evdev.h"
#include "launcher-util.h"
#include "udev-seat.h"

#if 0
#define DBG(...) \
	weston_log(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

struct rpi_compositor;
struct rpi_output;

struct rpi_flippipe {
	int readfd;
	int writefd;
	struct wl_event_source *source;
};

struct rpi_output {
	struct rpi_compositor *compositor;
	struct weston_output base;
	int single_buffer;

	struct weston_mode mode;
	struct rpi_flippipe flippipe;

	DISPMANX_DISPLAY_HANDLE_T display;
};

struct rpi_seat {
	struct weston_seat base;
	struct wl_list devices_list;

	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_monitor_source;
	char *seat_id;
};

struct rpi_compositor {
	struct weston_compositor base;
	uint32_t prev_state;

	struct udev *udev;
	struct udev_input input;
	struct wl_listener session_listener;

	int single_buffer;
};

static inline struct rpi_output *
to_rpi_output(struct weston_output *base)
{
	return container_of(base, struct rpi_output, base);
}

static inline struct rpi_seat *
to_rpi_seat(struct weston_seat *base)
{
	return container_of(base, struct rpi_seat, base);
}

static inline struct rpi_compositor *
to_rpi_compositor(struct weston_compositor *base)
{
	return container_of(base, struct rpi_compositor, base);
}

static uint64_t
rpi_get_current_time(void)
{
	struct timeval tv;

	/* XXX: use CLOCK_MONOTONIC instead? */
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
rpi_flippipe_update_complete(DISPMANX_UPDATE_HANDLE_T update, void *data)
{
	/* This function runs in a different thread. */
	struct rpi_flippipe *flippipe = data;
	uint64_t time;
	ssize_t ret;

	/* manufacture flip completion timestamp */
	time = rpi_get_current_time();

	ret = write(flippipe->writefd, &time, sizeof time);
	if (ret != sizeof time)
		weston_log("ERROR: %s failed to write, ret %zd, errno %d\n",
			   __func__, ret, errno);
}

static int
rpi_dispmanx_update_submit(DISPMANX_UPDATE_HANDLE_T update,
			   struct rpi_output *output)
{
	/*
	 * The callback registered here will eventually be called
	 * in a different thread context. Therefore we cannot call
	 * the usual functions from rpi_flippipe_update_complete().
	 * Instead, we have a pipe for passing the message from the
	 * thread, waking up the Weston main event loop, calling
	 * rpi_flippipe_handler(), and then ending up in
	 * rpi_output_update_complete() in the main thread context,
	 * where we can do the frame finishing work.
	 */
	return vc_dispmanx_update_submit(update, rpi_flippipe_update_complete,
					 &output->flippipe);
}

static void
rpi_output_update_complete(struct rpi_output *output, uint64_t time);

static int
rpi_flippipe_handler(int fd, uint32_t mask, void *data)
{
	struct rpi_output *output = data;
	ssize_t ret;
	uint64_t time;

	if (mask != WL_EVENT_READABLE)
		weston_log("ERROR: unexpected mask 0x%x in %s\n",
			   mask, __func__);

	ret = read(fd, &time, sizeof time);
	if (ret != sizeof time) {
		weston_log("ERROR: %s failed to read, ret %zd, errno %d\n",
			   __func__, ret, errno);
	}

	rpi_output_update_complete(output, time);

	return 1;
}

static int
rpi_flippipe_init(struct rpi_flippipe *flippipe, struct rpi_output *output)
{
	struct wl_event_loop *loop;
	int fd[2];

	if (pipe2(fd, O_CLOEXEC) == -1)
		return -1;

	flippipe->readfd = fd[0];
	flippipe->writefd = fd[1];

	loop = wl_display_get_event_loop(output->compositor->base.wl_display);
	flippipe->source = wl_event_loop_add_fd(loop, flippipe->readfd,
						WL_EVENT_READABLE,
						rpi_flippipe_handler, output);

	if (!flippipe->source) {
		close(flippipe->readfd);
		close(flippipe->writefd);
		return -1;
	}

	return 0;
}

static void
rpi_flippipe_release(struct rpi_flippipe *flippipe)
{
	wl_event_source_remove(flippipe->source);
	close(flippipe->readfd);
	close(flippipe->writefd);
}

static void
rpi_output_start_repaint_loop(struct weston_output *output)
{
	uint64_t time;

	time = rpi_get_current_time();
	weston_output_finish_frame(output, time);
}

static int
rpi_output_repaint(struct weston_output *base, pixman_region32_t *damage)
{
	struct rpi_output *output = to_rpi_output(base);
	struct rpi_compositor *compositor = output->compositor;
	struct weston_plane *primary_plane = &compositor->base.primary_plane;
	DISPMANX_UPDATE_HANDLE_T update;

	DBG("frame update start\n");

	/* Update priority higher than in rpi-renderer's
	 * output destroy function, see rpi_output_destroy().
	 */
	update = vc_dispmanx_update_start(1);

	rpi_renderer_set_update_handle(&output->base, update);
	compositor->base.renderer->repaint_output(&output->base, damage);

	pixman_region32_subtract(&primary_plane->damage,
				 &primary_plane->damage, damage);

	/* schedule callback to rpi_output_update_complete() */
	rpi_dispmanx_update_submit(update, output);
	DBG("frame update submitted\n");
	return 0;
}

static void
rpi_output_update_complete(struct rpi_output *output, uint64_t time)
{
	DBG("frame update complete(%" PRIu64 ")\n", time);
	rpi_renderer_finish_frame(&output->base);
	weston_output_finish_frame(&output->base, time);
}

static void
rpi_output_destroy(struct weston_output *base)
{
	struct rpi_output *output = to_rpi_output(base);

	DBG("%s\n", __func__);

	rpi_renderer_output_destroy(base);

	/* rpi_renderer_output_destroy() will schedule a removal of
	 * all Dispmanx Elements, and wait for the update to complete.
	 * Assuming updates are sequential, the wait should guarantee,
	 * that any pending rpi_flippipe_update_complete() callbacks
	 * have happened already. Therefore we can destroy the flippipe
	 * now.
	 */
	rpi_flippipe_release(&output->flippipe);

	weston_output_destroy(&output->base);

	vc_dispmanx_display_close(output->display);

	free(output);
}

static const char *transform_names[] = {
	[WL_OUTPUT_TRANSFORM_NORMAL] = "normal",
	[WL_OUTPUT_TRANSFORM_90] = "90",
	[WL_OUTPUT_TRANSFORM_180] = "180",
	[WL_OUTPUT_TRANSFORM_270] = "270",
	[WL_OUTPUT_TRANSFORM_FLIPPED] = "flipped",
	[WL_OUTPUT_TRANSFORM_FLIPPED_90] = "flipped-90",
	[WL_OUTPUT_TRANSFORM_FLIPPED_180] = "flipped-180",
	[WL_OUTPUT_TRANSFORM_FLIPPED_270] = "flipped-270",
};

static int
str2transform(const char *name)
{
	unsigned i;

	for (i = 0; i < ARRAY_LENGTH(transform_names); i++)
		if (strcmp(name, transform_names[i]) == 0)
			return i;

	return -1;
}

static const char *
transform2str(uint32_t output_transform)
{
	if (output_transform >= ARRAY_LENGTH(transform_names))
		return "<illegal value>";

	return transform_names[output_transform];
}

static int
rpi_output_create(struct rpi_compositor *compositor, uint32_t transform)
{
	struct rpi_output *output;
	DISPMANX_MODEINFO_T modeinfo;
	int ret;
	float mm_width, mm_height;

	output = calloc(1, sizeof *output);
	if (!output)
		return -1;

	output->compositor = compositor;
	output->single_buffer = compositor->single_buffer;

	if (rpi_flippipe_init(&output->flippipe, output) < 0) {
		weston_log("Creating message pipe failed.\n");
		goto out_free;
	}

	output->display = vc_dispmanx_display_open(DISPMANX_ID_HDMI);
	if (!output->display) {
		weston_log("Failed to open dispmanx HDMI display.\n");
		goto out_pipe;
	}

	ret = vc_dispmanx_display_get_info(output->display, &modeinfo);
	if (ret < 0) {
		weston_log("Failed to get display mode information.\n");
		goto out_dmx_close;
	}

	output->base.start_repaint_loop = rpi_output_start_repaint_loop;
	output->base.repaint = rpi_output_repaint;
	output->base.destroy = rpi_output_destroy;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	/* XXX: use tvservice to get information from and control the
	 * HDMI and SDTV outputs. See:
	 * /opt/vc/include/interface/vmcs_host/vc_tvservice.h
	 */

	/* only one static mode in list */
	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = modeinfo.width;
	output->mode.height = modeinfo.height;
	output->mode.refresh = 60000;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;
	output->base.subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
	output->base.make = "unknown";
	output->base.model = "unknown";

	/* guess 96 dpi */
	mm_width  = modeinfo.width * (25.4f / 96.0f);
	mm_height = modeinfo.height * (25.4f / 96.0f);

	weston_output_init(&output->base, &compositor->base,
			   0, 0, round(mm_width), round(mm_height),
			   transform, 1);

	if (rpi_renderer_output_create(&output->base, output->display) < 0)
		goto out_output;

	wl_list_insert(compositor->base.output_list.prev, &output->base.link);

	weston_log("Raspberry Pi HDMI output %dx%d px\n",
		   output->mode.width, output->mode.height);
	weston_log_continue(STAMP_SPACE "guessing %d Hz and 96 dpi\n",
			    output->mode.refresh / 1000);
	weston_log_continue(STAMP_SPACE "orientation: %s\n",
			    transform2str(output->base.transform));

	if (!strncmp(transform2str(output->base.transform), "flipped", 7))
		weston_log("warning: flipped output transforms may not work\n");

	return 0;

out_output:
	weston_output_destroy(&output->base);

out_dmx_close:
	vc_dispmanx_display_close(output->display);

out_pipe:
	rpi_flippipe_release(&output->flippipe);

out_free:
	free(output);
	return -1;
}

static void
rpi_compositor_destroy(struct weston_compositor *base)
{
	struct rpi_compositor *compositor = to_rpi_compositor(base);

	udev_input_destroy(&compositor->input);

	compositor->base.renderer->destroy(&compositor->base);

	/* destroys outputs, too */
	weston_compositor_shutdown(&compositor->base);

	weston_launcher_destroy(compositor->base.launcher);

	bcm_host_deinit();
	free(compositor);
}

static void
session_notify(struct wl_listener *listener, void *data)
{
	struct rpi_compositor *compositor = data;
	struct weston_output *output;

	if (compositor->base.session_active) {
		weston_log("activating session\n");
		compositor->base.state = compositor->prev_state;
		weston_compositor_damage_all(&compositor->base);
		udev_input_enable(&compositor->input, compositor->udev);
	} else {
		weston_log("deactivating session\n");
		udev_input_disable(&compositor->input);

		compositor->prev_state = compositor->base.state;
		weston_compositor_offscreen(&compositor->base);

		/* If we have a repaint scheduled (either from a
		 * pending pageflip or the idle handler), make sure we
		 * cancel that so we don't try to pageflip when we're
		 * vt switched away.  The OFFSCREEN state will prevent
		 * further attemps at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

		wl_list_for_each(output,
				 &compositor->base.output_list, link) {
			output->repaint_needed = 0;
		}
	};
}

static void
rpi_restore(struct weston_compositor *compositor)
{
	weston_launcher_restore(compositor->launcher);
}

static void
switch_vt_binding(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
	struct weston_compositor *compositor = data;

	weston_launcher_activate_vt(compositor->launcher, key - KEY_F1 + 1);
}

struct rpi_parameters {
	int tty;
	struct rpi_renderer_parameters renderer;
	uint32_t output_transform;
};

static struct weston_compositor *
rpi_compositor_create(struct wl_display *display, int *argc, char *argv[],
		      struct weston_config *config,
		      struct rpi_parameters *param)
{
	struct rpi_compositor *compositor;
	uint32_t key;

	weston_log("initializing Raspberry Pi backend\n");

	compositor = calloc(1, sizeof *compositor);
	if (compositor == NULL)
		return NULL;

	if (weston_compositor_init(&compositor->base, display, argc, argv,
				   config) < 0)
		goto out_free;

	compositor->udev = udev_new();
	if (compositor->udev == NULL) {
		weston_log("Failed to initialize udev context.\n");
		goto out_compositor;
	}

	compositor->session_listener.notify = session_notify;
	wl_signal_add(&compositor->base.session_signal,
		      &compositor ->session_listener);
	compositor->base.launcher =
		weston_launcher_connect(&compositor->base, param->tty, "seat0");
	if (!compositor->base.launcher) {
		weston_log("Failed to initialize tty.\n");
		goto out_udev;
	}

	compositor->base.destroy = rpi_compositor_destroy;
	compositor->base.restore = rpi_restore;

	compositor->prev_state = WESTON_COMPOSITOR_ACTIVE;
	compositor->single_buffer = param->renderer.single_buffer;

	weston_log("Dispmanx planes are %s buffered.\n",
		   compositor->single_buffer ? "single" : "double");

	if (udev_input_init(&compositor->input,
			    &compositor->base,
			    compositor->udev, "seat0") != 0) {
		weston_log("Failed to initialize udev input.\n");
		goto out_launcher;
	}

	for (key = KEY_F1; key < KEY_F9; key++)
		weston_compositor_add_key_binding(&compositor->base, key,
						  MODIFIER_CTRL | MODIFIER_ALT,
						  switch_vt_binding, compositor);

	/*
	 * bcm_host_init() creates threads.
	 * Therefore we must have all signal handlers set and signals blocked
	 * before calling it. Otherwise the signals may end in the bcm
	 * threads and cause the default behaviour there. For instance,
	 * SIGUSR1 used for VT switching caused Weston to terminate there.
	 */
	bcm_host_init();

	if (rpi_renderer_create(&compositor->base, &param->renderer) < 0)
		goto out_udev_input;

	if (rpi_output_create(compositor, param->output_transform) < 0)
		goto out_renderer;

	return &compositor->base;

out_renderer:
	compositor->base.renderer->destroy(&compositor->base);

out_udev_input:
	udev_input_destroy(&compositor->input);

out_launcher:
	weston_launcher_destroy(compositor->base.launcher);

out_udev:
	udev_unref(compositor->udev);

out_compositor:
	weston_compositor_shutdown(&compositor->base);

out_free:
	bcm_host_deinit();
	free(compositor);

	return NULL;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int *argc, char *argv[],
	     struct weston_config *config)
{
	const char *transform = "normal";
	int ret;

	struct rpi_parameters param = {
		.tty = 0, /* default to current tty */
		.renderer.single_buffer = 0,
		.output_transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.renderer.opaque_regions = 0,
	};

	const struct weston_option rpi_options[] = {
		{ WESTON_OPTION_INTEGER, "tty", 0, &param.tty },
		{ WESTON_OPTION_BOOLEAN, "single-buffer", 0,
		  &param.renderer.single_buffer },
		{ WESTON_OPTION_STRING, "transform", 0, &transform },
		{ WESTON_OPTION_BOOLEAN, "opaque-regions", 0,
		  &param.renderer.opaque_regions },
	};

	parse_options(rpi_options, ARRAY_LENGTH(rpi_options), argc, argv);

	ret = str2transform(transform);
	if (ret < 0)
		weston_log("invalid transform \"%s\"\n", transform);
	else
		param.output_transform = ret;

	return rpi_compositor_create(display, argc, argv, config, &param);
}
