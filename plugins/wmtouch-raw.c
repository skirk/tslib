/*
*  tslib/plugins/mswin_input-raw.c
*
*  Copyright (C) 2018 Tuomo Rinne <tuomo.rinne@gmail.com>
*
* This file is placed under the LGPL.  Please see the file
* COPYING for more details.
*
* SPDX-License-Identifier: LGPL-2.1
*
*
* Read raw pressure, x, y, and timestamp from a touchscreen device.
*/

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <stdint.h>

#include <stdlib.h>
#include <time.h>

#include <Windows.h>
#include <sys/types.h>

#include "tslib-private.h"

#define GRAB_EVENTS_WANTED	1

struct tslib_input {
	struct tslib_module_info module;

	int	current_x;
	int	current_y;
	int	current_p;

	int8_t	grab_events;

	WNDPROC prevWndProc;
	TOUCHINPUT *buf;

	int	slot;
	int	max_slots;
	int	nr;
	int	pen_down;
	int	last_fd;
	int8_t	mt;
	int8_t	no_pressure;
	int8_t	type_a;
	int32_t *last_pressure;

	uint16_t	special_device; /* broken device we work around, see below */
};

static int ts_mswin_input_read(struct tslib_module_info *inf,
	struct ts_sample *samp, int nr)
{
	struct tslib_input *i = (struct tslib_input *)inf;

	if (!i->max_slots)
		return 0;

	if (i->buf[0].dwFlags & TOUCHEVENTF_DOWN ||
		i->buf[0].dwFlags & TOUCHEVENTF_MOVE) {

		samp[0].x = i->buf[0].x;
		samp[0].y = i->buf[0].y;
		samp[0].tv.tv_usec = i->buf[0].dwTime * 1000;
		samp[0].tv.tv_sec = i->buf[0].dwTime / 1000;
	}

	return 1;
}

static int ts_mswin_input_read_mt(struct tslib_module_info *inf,
	struct ts_sample_mt **samp, int max_slots, int nr)
{
	struct tslib_input *i = (struct tslib_input *)inf;

	int j = 0, k = 0;

	for (; j < max_slots && j < i->max_slots; j++) {
		if (i->buf[j].dwFlags & TOUCHEVENTF_DOWN ||
			i->buf[j].dwFlags & TOUCHEVENTF_MOVE) {

			samp[0][k].x = i->buf[j].x;
			samp[0][k].y = i->buf[j].y;
			samp[0][k].tv.tv_usec = i->buf[j].dwTime * 1000;
			samp[0][k].tv.tv_sec = i->buf[j].dwTime / 1000;
			k++;
		}
	}
	return k;
}

static int ts_mswin_input_fini(struct tslib_module_info *inf)
{
	struct tslib_input *i = (struct tslib_input *)inf;
	free(i->buf);
	return 0;
}

static const struct tslib_ops __ts_input_ops = {
	.read = ts_mswin_input_read,
	.read_mt = ts_mswin_input_read_mt,
	.fini = ts_mswin_input_fini,
};

static int parse_raw_grab(struct tslib_module_info *inf, char *str, void *data)
{
	struct tslib_input *i = (struct tslib_input *)inf;
	unsigned long v;
	int err = errno;

	v = strtoul(str, NULL, 0);

	if (v == ULONG_MAX && errno == ERANGE)
		return -1;

	errno = err;
	switch ((int)(intptr_t)data) {
	case 1:
		if (v)
			i->grab_events = GRAB_EVENTS_WANTED;
		break;
	default:
		return -1;
	}
	return 0;
}

static const struct tslib_vars raw_vars[] = {
	{ "grab_events", (void *)1, parse_raw_grab },
};

#define NR_VARS (sizeof(raw_vars) / sizeof(raw_vars[0]))


LRESULT CALLBACK tslibWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	struct tslib_input *i = (struct tslib_input*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	BOOL handled = FALSE;
	int done = 0;

	if (!done) {

		switch (uMsg) {
		case WM_TOUCH:

			unsigned int ni = (unsigned int)wParam; // Number of actual per-contact messages

			if (ni > i->max_slots) {
				free(i->buf);
				i->buf = malloc(ni * sizeof(TOUCHINPUT));

				if (!i->buf)
					break; //TODO: proper error handling

				i->max_slots = ni;
			}

			memset(i->buf, 0, i->max_slots * sizeof(TOUCHINPUT));

			// Unpack message parameters into the array of TOUCHINPUT structures, each
			// representing a message for one single contact.
			if (GetTouchInputInfo((HTOUCHINPUT)lParam, ni, i->buf, sizeof(TOUCHINPUT))) {
				handled = TRUE;
			}

			break;
		}
	}

	if (handled) {
		// if you handled the message, close the touch input handle and return
		CloseTouchInputHandle((HTOUCHINPUT)lParam);
		return 0;
	}

	return CallWindowProc(i->prevWndProc, hwnd, uMsg, wParam, lParam);
}

TSAPI struct tslib_module_info *wmtouch_mod_init(ATTR_UNUSED struct tsdev *dev,
	const char *params)
{

	struct tslib_input *i;

	i = malloc(sizeof(struct tslib_input));
	if (i == NULL)
		return NULL;

	i->module.ops = &__ts_input_ops;
	i->current_x = 0;
	i->current_y = 0;
	i->current_p = 0;
	i->grab_events = 0;
	i->slot = 0;
	i->pen_down = 0;
	i->buf = NULL;
	i->max_slots = 0;
	i->nr = 0;
	i->mt = 0;
	i->no_pressure = 0;
	i->last_fd = -2;
	i->type_a = 0;
	i->special_device = 0;
	i->last_pressure = NULL;


	if (tslib_parse_vars(&i->module, raw_vars, NR_VARS, params)) {
		free(i);
		return NULL;
	}

	SetWindowLongPtr((HWND)dev->fd, GWLP_USERDATA, (LONG_PTR)i);
	i->prevWndProc = (WNDPROC)SetWindowLongPtr((HWND)dev->fd, GWLP_WNDPROC, (LONG_PTR)&tslibWndProc);

	return &(i->module);
}

#ifndef TSLIB_STATIC_MSWIN_INPUT_MODULE
TSLIB_MODULE_INIT(wmtouch_mod_init);
#endif
