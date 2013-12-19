/*
 * Copyright © 2010 Intel Corporation
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
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <mtdev.h>
#include <assert.h>

#include "compositor.h"
#include "evdev.h"

#define DEFAULT_AXIS_STEP_DISTANCE wl_fixed_from_int(10)

void
evdev_led_update(struct evdev_device *device, enum weston_led leds)
{
	static const struct {
		enum weston_led weston;
		int evdev;
	} map[] = {
		{ LED_NUM_LOCK, LED_NUML },
		{ LED_CAPS_LOCK, LED_CAPSL },
		{ LED_SCROLL_LOCK, LED_SCROLLL },
	};
	struct input_event ev[ARRAY_LENGTH(map) + 1];
	unsigned int i;

	if (!(device->seat_caps & EVDEV_SEAT_KEYBOARD))
		return;

	memset(ev, 0, sizeof(ev));
	for (i = 0; i < ARRAY_LENGTH(map); i++) {
		ev[i].type = EV_LED;
		ev[i].code = map[i].evdev;
		ev[i].value = !!(leds & map[i].weston);
	}
	ev[i].type = EV_SYN;
	ev[i].code = SYN_REPORT;

	i = write(device->fd, ev, sizeof ev);
	(void)i; /* no, we really don't care about the return value */
}

static void
transform_absolute(struct evdev_device *device, int32_t *x, int32_t *y)
{
       if (!device->abs.apply_calibration) {
               *x = device->abs.x;
               *y = device->abs.y;
               return;
       } else {
               *x = device->abs.x * device->abs.calibration[0] +
                       device->abs.y * device->abs.calibration[1] +
                       device->abs.calibration[2];

               *y = device->abs.x * device->abs.calibration[3] +
                       device->abs.y * device->abs.calibration[4] +
                       device->abs.calibration[5];
       }
}

static void
evdev_flush_pending_event(struct evdev_device *device, uint32_t time)
{
	struct weston_seat *master = device->seat;
	wl_fixed_t x, y;
	int32_t cx, cy;
	int slot;

	slot = device->mt.slot;

	switch (device->pending_event) {
	case EVDEV_NONE:
		return;
	case EVDEV_RELATIVE_MOTION:
		notify_motion(master, time, device->rel.dx, device->rel.dy);
		device->rel.dx = 0;
		device->rel.dy = 0;
		goto handled;
	case EVDEV_ABSOLUTE_MT_DOWN:
		weston_output_transform_coordinate(device->output,
						   wl_fixed_from_int(device->mt.slots[slot].x),
						   wl_fixed_from_int(device->mt.slots[slot].y),
						   &x, &y);
		notify_touch(master, time,
			     slot, x, y, WL_TOUCH_DOWN);
		goto handled;
	case EVDEV_ABSOLUTE_MT_MOTION:
		weston_output_transform_coordinate(device->output,
						   wl_fixed_from_int(device->mt.slots[slot].x),
						   wl_fixed_from_int(device->mt.slots[slot].y),
						   &x, &y);
		notify_touch(master, time,
			     slot, x, y, WL_TOUCH_MOTION);
		goto handled;
	case EVDEV_ABSOLUTE_MT_UP:
		notify_touch(master, time, slot, 0, 0,
			     WL_TOUCH_UP);
		goto handled;
	case EVDEV_ABSOLUTE_TOUCH_DOWN:
		transform_absolute(device, &cx, &cy);
		weston_output_transform_coordinate(device->output,
						   wl_fixed_from_int(cx),
						   wl_fixed_from_int(cy),
						   &x, &y);
		notify_touch(master, time, 0, x, y, WL_TOUCH_DOWN);
		goto handled;
	case EVDEV_ABSOLUTE_MOTION:
		transform_absolute(device, &cx, &cy);
		weston_output_transform_coordinate(device->output,
						   wl_fixed_from_int(cx),
						   wl_fixed_from_int(cy),
						   &x, &y);

		if (device->seat_caps & EVDEV_SEAT_TOUCH)
			notify_touch(master, time, 0, x, y, WL_TOUCH_MOTION);
		else if (device->seat_caps & EVDEV_SEAT_POINTER)
			notify_motion_absolute(master, time, x, y);
		goto handled;
	case EVDEV_ABSOLUTE_TOUCH_UP:
		notify_touch(master, time, 0, 0, 0, WL_TOUCH_UP);
		goto handled;
	}

	assert(0 && "Unknown pending event type");

handled:
	device->pending_event = EVDEV_NONE;
}

static void
evdev_process_touch_button(struct evdev_device *device, int time, int value)
{
	if (device->pending_event != EVDEV_NONE &&
	    device->pending_event != EVDEV_ABSOLUTE_MOTION)
		evdev_flush_pending_event(device, time);

	device->pending_event = (value ?
				 EVDEV_ABSOLUTE_TOUCH_DOWN :
				 EVDEV_ABSOLUTE_TOUCH_UP);
}

static inline void
evdev_process_key(struct evdev_device *device, struct input_event *e, int time)
{
	/* ignore kernel key repeat */
	if (e->value == 2)
		return;

	if (e->code == BTN_TOUCH) {
		if (!device->is_mt)
			evdev_process_touch_button(device, time, e->value);
		return;
	}

	evdev_flush_pending_event(device, time);

	switch (e->code) {
	case BTN_LEFT:
	case BTN_RIGHT:
	case BTN_MIDDLE:
	case BTN_SIDE:
	case BTN_EXTRA:
	case BTN_FORWARD:
	case BTN_BACK:
	case BTN_TASK:
		notify_button(device->seat,
			      time, e->code,
			      e->value ? WL_POINTER_BUTTON_STATE_PRESSED :
					 WL_POINTER_BUTTON_STATE_RELEASED);
		break;

	default:
		notify_key(device->seat,
			   time, e->code,
			   e->value ? WL_KEYBOARD_KEY_STATE_PRESSED :
				      WL_KEYBOARD_KEY_STATE_RELEASED,
			   STATE_UPDATE_AUTOMATIC);
		break;
	}
}

static void
evdev_process_touch(struct evdev_device *device,
		    struct input_event *e,
		    uint32_t time)
{
	const int screen_width = device->output->current_mode->width;
	const int screen_height = device->output->current_mode->height;

	switch (e->code) {
	case ABS_MT_SLOT:
		evdev_flush_pending_event(device, time);
		device->mt.slot = e->value;
		break;
	case ABS_MT_TRACKING_ID:
		if (device->pending_event != EVDEV_NONE &&
		    device->pending_event != EVDEV_ABSOLUTE_MT_MOTION)
			evdev_flush_pending_event(device, time);
		if (e->value >= 0)
			device->pending_event = EVDEV_ABSOLUTE_MT_DOWN;
		else
			device->pending_event = EVDEV_ABSOLUTE_MT_UP;
		break;
	case ABS_MT_POSITION_X:
		device->mt.slots[device->mt.slot].x =
			(e->value - device->abs.min_x) * screen_width /
			(device->abs.max_x - device->abs.min_x);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	case ABS_MT_POSITION_Y:
		device->mt.slots[device->mt.slot].y =
			(e->value - device->abs.min_y) * screen_height /
			(device->abs.max_y - device->abs.min_y);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	}
}

static inline void
evdev_process_absolute_motion(struct evdev_device *device,
			      struct input_event *e)
{
	const int screen_width = device->output->current_mode->width;
	const int screen_height = device->output->current_mode->height;

	switch (e->code) {
	case ABS_X:
		device->abs.x =
			(e->value - device->abs.min_x) * screen_width /
			(device->abs.max_x - device->abs.min_x);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	case ABS_Y:
		device->abs.y =
			(e->value - device->abs.min_y) * screen_height /
			(device->abs.max_y - device->abs.min_y);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	}
}

static inline void
evdev_process_relative(struct evdev_device *device,
		       struct input_event *e, uint32_t time)
{
	switch (e->code) {
	case REL_X:
		if (device->pending_event != EVDEV_RELATIVE_MOTION)
			evdev_flush_pending_event(device, time);
		device->rel.dx += wl_fixed_from_int(e->value);
		device->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_Y:
		if (device->pending_event != EVDEV_RELATIVE_MOTION)
			evdev_flush_pending_event(device, time);
		device->rel.dy += wl_fixed_from_int(e->value);
		device->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_WHEEL:
		evdev_flush_pending_event(device, time);
		switch (e->value) {
		case -1:
			/* Scroll down */
		case 1:
			/* Scroll up */
			notify_axis(device->seat,
				    time,
				    WL_POINTER_AXIS_VERTICAL_SCROLL,
				    -1 * e->value * DEFAULT_AXIS_STEP_DISTANCE);
			break;
		default:
			break;
		}
		break;
	case REL_HWHEEL:
		evdev_flush_pending_event(device, time);
		switch (e->value) {
		case -1:
			/* Scroll left */
		case 1:
			/* Scroll right */
			notify_axis(device->seat,
				    time,
				    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				    e->value * DEFAULT_AXIS_STEP_DISTANCE);
			break;
		default:
			break;

		}
	}
}

static inline void
evdev_process_absolute(struct evdev_device *device,
		       struct input_event *e,
		       uint32_t time)
{
	if (device->is_mt) {
		evdev_process_touch(device, e, time);
	} else {
		evdev_process_absolute_motion(device, e);
	}
}

static void
fallback_process(struct evdev_dispatch *dispatch,
		 struct evdev_device *device,
		 struct input_event *event,
		 uint32_t time)
{
	switch (event->type) {
	case EV_REL:
		evdev_process_relative(device, event, time);
		break;
	case EV_ABS:
		evdev_process_absolute(device, event, time);
		break;
	case EV_KEY:
		evdev_process_key(device, event, time);
		break;
	case EV_SYN:
		evdev_flush_pending_event(device, time);
		break;
	}
}

static void
fallback_destroy(struct evdev_dispatch *dispatch)
{
	free(dispatch);
}

struct evdev_dispatch_interface fallback_interface = {
	fallback_process,
	fallback_destroy
};

static struct evdev_dispatch *
fallback_dispatch_create(void)
{
	struct evdev_dispatch *dispatch = malloc(sizeof *dispatch);
	if (dispatch == NULL)
		return NULL;

	dispatch->interface = &fallback_interface;

	return dispatch;
}

static void
evdev_process_events(struct evdev_device *device,
		     struct input_event *ev, int count)
{
	struct evdev_dispatch *dispatch = device->dispatch;
	struct input_event *e, *end;
	uint32_t time = 0;

	e = ev;
	end = e + count;
	for (e = ev; e < end; e++) {
		time = e->time.tv_sec * 1000 + e->time.tv_usec / 1000;

		dispatch->interface->process(dispatch, device, e, time);
	}
}

static int
evdev_device_data(int fd, uint32_t mask, void *data)
{
	struct weston_compositor *ec;
	struct evdev_device *device = data;
	struct input_event ev[32];
	int len;

	ec = device->seat->compositor;
	if (!ec->session_active)
		return 1;

	/* If the compositor is repainting, this function is called only once
	 * per frame and we have to process all the events available on the
	 * fd, otherwise there will be input lag. */
	do {
		if (device->mtdev)
			len = mtdev_get(device->mtdev, fd, ev,
					ARRAY_LENGTH(ev)) *
				sizeof (struct input_event);
		else
			len = read(fd, &ev, sizeof ev);

		if (len < 0 || len % sizeof ev[0] != 0) {
			if (len < 0 && errno != EAGAIN && errno != EINTR) {
				weston_log("device %s died\n",
					   device->devnode);
				wl_event_source_remove(device->source);
				device->source = NULL;
			}

			return 1;
		}

		evdev_process_events(device, ev, len / sizeof ev[0]);

	} while (len > 0);

	return 1;
}

static int
evdev_configure_device(struct evdev_device *device)
{
	struct input_absinfo absinfo;
	unsigned long ev_bits[NBITS(EV_MAX)];
	unsigned long abs_bits[NBITS(ABS_MAX)];
	unsigned long rel_bits[NBITS(REL_MAX)];
	unsigned long key_bits[NBITS(KEY_MAX)];
	int has_abs, has_rel, has_mt;
	int has_button, has_keyboard, has_touch;
	unsigned int i;

	has_rel = 0;
	has_abs = 0;
	has_mt = 0;
	has_button = 0;
	has_keyboard = 0;
	has_touch = 0;

	ioctl(device->fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
	if (TEST_BIT(ev_bits, EV_ABS)) {
		ioctl(device->fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)),
		      abs_bits);

		if (TEST_BIT(abs_bits, ABS_X)) {
			ioctl(device->fd, EVIOCGABS(ABS_X), &absinfo);
			device->abs.min_x = absinfo.minimum;
			device->abs.max_x = absinfo.maximum;
			has_abs = 1;
		}
		if (TEST_BIT(abs_bits, ABS_Y)) {
			ioctl(device->fd, EVIOCGABS(ABS_Y), &absinfo);
			device->abs.min_y = absinfo.minimum;
			device->abs.max_y = absinfo.maximum;
			has_abs = 1;
		}
                /* We only handle the slotted Protocol B in weston.
                   Devices with ABS_MT_POSITION_* but not ABS_MT_SLOT
                   require mtdev for conversion. */
		if (TEST_BIT(abs_bits, ABS_MT_POSITION_X) &&
		    TEST_BIT(abs_bits, ABS_MT_POSITION_Y)) {
			ioctl(device->fd, EVIOCGABS(ABS_MT_POSITION_X),
			      &absinfo);
			device->abs.min_x = absinfo.minimum;
			device->abs.max_x = absinfo.maximum;
			ioctl(device->fd, EVIOCGABS(ABS_MT_POSITION_Y),
			      &absinfo);
			device->abs.min_y = absinfo.minimum;
			device->abs.max_y = absinfo.maximum;
			device->is_mt = 1;
			has_touch = 1;
			has_mt = 1;

			if (!TEST_BIT(abs_bits, ABS_MT_SLOT)) {
				device->mtdev = mtdev_new_open(device->fd);
				if (!device->mtdev) {
					weston_log("mtdev required but failed to open for %s\n",
						   device->devnode);
					return 0;
				}
				device->mt.slot = device->mtdev->caps.slot.value;
			} else {
				ioctl(device->fd, EVIOCGABS(ABS_MT_SLOT),
				      &absinfo);
				device->mt.slot = absinfo.value;
			}
		}
	}
	if (TEST_BIT(ev_bits, EV_REL)) {
		ioctl(device->fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)),
		      rel_bits);
		if (TEST_BIT(rel_bits, REL_X) || TEST_BIT(rel_bits, REL_Y))
			has_rel = 1;
	}
	if (TEST_BIT(ev_bits, EV_KEY)) {
		ioctl(device->fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)),
		      key_bits);
		if (TEST_BIT(key_bits, BTN_TOOL_FINGER) &&
		    !TEST_BIT(key_bits, BTN_TOOL_PEN) &&
		    (has_abs || has_mt)) {
			device->dispatch = evdev_touchpad_create(device);
			weston_log("input device %s, %s is a touchpad\n",
				   device->devname, device->devnode);
		}
		for (i = KEY_ESC; i < KEY_MAX; i++) {
			if (i >= BTN_MISC && i < KEY_OK)
				continue;
			if (TEST_BIT(key_bits, i)) {
				has_keyboard = 1;
				break;
			}
		}
		if (TEST_BIT(key_bits, BTN_TOUCH))
			has_touch = 1;
		for (i = BTN_MISC; i < BTN_JOYSTICK; i++) {
			if (TEST_BIT(key_bits, i)) {
				has_button = 1;
				break;
			}
		}
	}
	if (TEST_BIT(ev_bits, EV_LED))
		has_keyboard = 1;

	if ((has_abs || has_rel) && has_button) {
		weston_seat_init_pointer(device->seat);
		device->seat_caps |= EVDEV_SEAT_POINTER;
		weston_log("input device %s, %s is a pointer caps =%s%s%s\n",
			   device->devname, device->devnode,
			   has_abs ? " absolute-motion" : "",
			   has_rel ? " relative-motion": "",
			   has_button ? " button" : "");
	}
	if (has_keyboard) {
		if (weston_seat_init_keyboard(device->seat, NULL) < 0)
			return -1;
		device->seat_caps |= EVDEV_SEAT_KEYBOARD;
		weston_log("input device %s, %s is a keyboard\n",
			   device->devname, device->devnode);
	}
	if (has_touch && !has_button) {
		weston_seat_init_touch(device->seat);
		device->seat_caps |= EVDEV_SEAT_TOUCH;
		weston_log("input device %s, %s is a touch device\n",
			   device->devname, device->devnode);
	}

	return 0;
}

struct evdev_device *
evdev_device_create(struct weston_seat *seat, const char *path, int device_fd)
{
	struct evdev_device *device;
	struct weston_compositor *ec;
	char devname[256] = "unknown";

	device = zalloc(sizeof *device);
	if (device == NULL)
		return NULL;

	ec = seat->compositor;
	device->output =
		container_of(ec->output_list.next, struct weston_output, link);

	device->seat = seat;
	device->seat_caps = 0;
	device->is_mt = 0;
	device->mtdev = NULL;
	device->devnode = strdup(path);
	device->mt.slot = -1;
	device->rel.dx = 0;
	device->rel.dy = 0;
	device->dispatch = NULL;
	device->fd = device_fd;
	device->pending_event = EVDEV_NONE;
	wl_list_init(&device->link);

	ioctl(device->fd, EVIOCGNAME(sizeof(devname)), devname);
	devname[sizeof(devname) - 1] = '\0';
	device->devname = strdup(devname);

	if (evdev_configure_device(device) == -1)
		goto err;

	if (device->seat_caps == 0) {
		evdev_device_destroy(device);
		return EVDEV_UNHANDLED_DEVICE;
	}

	/* If the dispatch was not set up use the fallback. */
	if (device->dispatch == NULL)
		device->dispatch = fallback_dispatch_create();
	if (device->dispatch == NULL)
		goto err;

	device->source = wl_event_loop_add_fd(ec->input_loop, device->fd,
					      WL_EVENT_READABLE,
					      evdev_device_data, device);
	if (device->source == NULL)
		goto err;

	return device;

err:
	evdev_device_destroy(device);
	return NULL;
}

void
evdev_device_destroy(struct evdev_device *device)
{
	struct evdev_dispatch *dispatch;

	if (device->seat_caps & EVDEV_SEAT_POINTER)
		weston_seat_release_pointer(device->seat);
	if (device->seat_caps & EVDEV_SEAT_KEYBOARD)
		weston_seat_release_keyboard(device->seat);
	if (device->seat_caps & EVDEV_SEAT_TOUCH)
		weston_seat_release_touch(device->seat);

	dispatch = device->dispatch;
	if (dispatch)
		dispatch->interface->destroy(dispatch);

	if (device->source)
		wl_event_source_remove(device->source);
	wl_list_remove(&device->link);
	if (device->mtdev)
		mtdev_close_delete(device->mtdev);
	close(device->fd);
	free(device->devname);
	free(device->devnode);
	free(device);
}

void
evdev_notify_keyboard_focus(struct weston_seat *seat,
			    struct wl_list *evdev_devices)
{
	struct evdev_device *device;
	struct wl_array keys;
	unsigned int i, set;
	char evdev_keys[(KEY_CNT + 7) / 8];
	char all_keys[(KEY_CNT + 7) / 8];
	uint32_t *k;
	int ret;

	if (!seat->keyboard_device_count > 0)
		return;

	memset(all_keys, 0, sizeof all_keys);
	wl_list_for_each(device, evdev_devices, link) {
		memset(evdev_keys, 0, sizeof evdev_keys);
		ret = ioctl(device->fd,
			    EVIOCGKEY(sizeof evdev_keys), evdev_keys);
		if (ret < 0) {
			weston_log("failed to get keys for device %s\n",
				device->devnode);
			continue;
		}
		for (i = 0; i < ARRAY_LENGTH(evdev_keys); i++)
			all_keys[i] |= evdev_keys[i];
	}

	wl_array_init(&keys);
	for (i = 0; i < KEY_CNT; i++) {
		set = all_keys[i >> 3] & (1 << (i & 7));
		if (set) {
			k = wl_array_add(&keys, sizeof *k);
			*k = i;
		}
	}

	notify_keyboard_focus_in(seat, &keys, STATE_UPDATE_AUTOMATIC);

	wl_array_release(&keys);
}
