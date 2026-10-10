/*
 * Locking the screen and noticing you've gone (ext-session-lock-v1,
 * ext-idle-notify-v1, idle-inhibit-unstable-v1).
 *
 * A screen locker (gemwm-lock) locks the session: every window, the menu
 * bar and other panels are hidden, the desktop alone stays, and the
 * locker's surfaces go over it, one an output; keys and the pointer go to
 * nothing else, and key bindings stop. Only once a frame without them has
 * been drawn is the locker told it's locked (so a laptop locking as it
 * sleeps wakes up locked). If the locker dies without unlocking, the
 * session stays locked and it's started again.
 *
 * Idle: after [idle] lock minutes with no input the locker is started,
 * and after screen-off minutes the screens go off (a minute, while
 * locked); any input brings them back. A program that asks (a video
 * playing) holds both off. Other programs can watch for idleness too
 * (swayidle, through ext-idle-notify).
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>
#include "server.h"

struct session_lock {
	struct server *server;
	struct wlr_session_lock_v1 *wlr;
	bool locked_sent;
	struct wl_list surfaces; /* lock_surface.link */
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
};

struct lock_surface {
	struct wl_list link;
	struct session_lock *lock;
	struct wlr_session_lock_surface_v1 *wlr;
	struct wlr_scene_tree *tree;
	struct wl_listener map;
	struct wl_listener destroy;
};

static int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

bool lock_active(struct server *server) {
	return server->locked;
}

/* Everything but the desktop hidden, or shown again. */
static void hide_session(struct server *server, bool hide) {
	for (int i = 0; i < 4; i++) {
		wlr_scene_node_set_enabled(&server->layers[i]->node, !hide);
	}
	wlr_scene_node_set_enabled(&server->layer_views->node, !hide);
	wlr_scene_node_set_enabled(&server->layer_fullscreen->node, !hide);
	wlr_scene_node_set_enabled(&server->layer_drag->node, !hide);
	wlr_scene_node_set_enabled(&server->layer_lock->node, hide);
}

static void redraw_all(struct server *server) {
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		output->lock_rendered = false;
		if (output->wlr_output->enabled) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static void give_keyboard(struct server *server, struct wlr_surface *surface) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat, surface, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
	}
}

/* The lock surface on the output under the pointer, or any. */
struct wlr_surface *lock_focus_surface(struct server *server) {
	if (server->lock == NULL) {
		return NULL;
	}
	struct wlr_output *under = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	struct lock_surface *ls, *any = NULL;
	wl_list_for_each(ls, &server->lock->surfaces, link) {
		if (!ls->wlr->surface->mapped) {
			continue;
		}
		if (ls->wlr->output == under) {
			return ls->wlr->surface;
		}
		any = any != NULL ? any : ls;
	}
	return any != NULL ? any->wlr->surface : NULL;
}

/* ---- The lock ------------------------------------------------------------ */

static void lock_surface_map(struct wl_listener *listener, void *data) {
	struct lock_surface *ls = wl_container_of(listener, ls, map);
	struct server *server = ls->lock->server;
	/* The keyboard to the one under the pointer, or this if it's first. */
	struct wlr_surface *focus = lock_focus_surface(server);
	if (focus != NULL) {
		give_keyboard(server, focus);
	}
	cursor_rebase(server);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
	struct lock_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->link);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->destroy.link);
	free(ls);
}

/* Each output's lock surface, the output's size, where the output is. */
static void place_surface(struct lock_surface *ls) {
	struct wlr_output *wlr_output = ls->wlr->output;
	struct server *server = ls->lock->server;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
	wlr_scene_node_set_position(&ls->tree->node, box.x, box.y);
	wlr_session_lock_surface_v1_configure(ls->wlr, box.width, box.height);
}

static void lock_new_surface(struct wl_listener *listener, void *data) {
	struct session_lock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *wlr = data;
	struct lock_surface *ls = calloc(1, sizeof(*ls));
	ls->lock = lock;
	ls->wlr = wlr;
	ls->tree = wlr_scene_subsurface_tree_create(lock->server->layer_lock,
		wlr->surface);
	ls->map.notify = lock_surface_map;
	wl_signal_add(&wlr->surface->events.map, &ls->map);
	ls->destroy.notify = lock_surface_destroy;
	wl_signal_add(&wlr->events.destroy, &ls->destroy);
	wl_list_insert(&lock->surfaces, &ls->link);
	place_surface(ls);
}

static void lock_free(struct session_lock *lock) {
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);
	if (lock->server->lock == lock) {
		lock->server->lock = NULL;
	}
	free(lock);
}

static void idle_rearm(struct server *server);

static void lock_unlock(struct wl_listener *listener, void *data) {
	struct session_lock *lock = wl_container_of(listener, lock, unlock);
	struct server *server = lock->server;
	wlr_log(WLR_INFO, "Unlocked");
	server->locked = false;
	lock_free(lock);
	hide_session(server, false);
	wlr_seat_keyboard_clear_focus(server->seat);
	/* The keyboard back to an alert box that had it, or the top window. */
	struct wlr_surface *exclusive = layers_exclusive_focus(server);
	if (exclusive != NULL) {
		give_keyboard(server, exclusive);
	} else if (server->focused_view != NULL) {
		struct view *view = server->focused_view;
		server->focused_view = NULL;
		focus_view(view);
	} else {
		workspace_focus_top(server);
	}
	cursor_rebase(server);
	redraw_all(server);
	idle_rearm(server);
	ipc_notify_focus(server);
}

static int relock(void *data);

/* The locker went away without unlocking (it crashed, or was killed):
 * still locked, and a locker started again. */
static void lock_destroy(struct wl_listener *listener, void *data) {
	struct session_lock *lock = wl_container_of(listener, lock, destroy);
	struct server *server = lock->server;
	bool was_current = server->lock == lock;
	lock_free(lock);
	if (was_current && server->locked) {
		wlr_log(WLR_ERROR, "The screen locker went away while locked; "
			"starting it again");
		wlr_seat_keyboard_clear_focus(server->seat);
		wlr_seat_pointer_clear_focus(server->seat);
		wl_event_source_timer_update(server->relock_timer, 1000);
	}
}

static void new_lock(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_lock);
	struct wlr_session_lock_v1 *wlr = data;
	if (server->lock != NULL || server->greeter) {
		/* Locked already, by a locker that's still here. */
		wlr_session_lock_v1_destroy(wlr);
		return;
	}
	struct session_lock *lock = calloc(1, sizeof(*lock));
	lock->server = server;
	lock->wlr = wlr;
	wl_list_init(&lock->surfaces);
	lock->new_surface.notify = lock_new_surface;
	wl_signal_add(&wlr->events.new_surface, &lock->new_surface);
	lock->unlock.notify = lock_unlock;
	wl_signal_add(&wlr->events.unlock, &lock->unlock);
	lock->destroy.notify = lock_destroy;
	wl_signal_add(&wlr->events.destroy, &lock->destroy);
	server->lock = lock;
	if (!server->locked) {
		wlr_log(WLR_INFO, "Locked");
		server->locked = true;
		/* Nothing in hand: no drag, no cycle, no overview. */
		end_interactive(server);
		view_cycle_end(server);
		overview_cancel(server);
		hide_session(server, true);
		wlr_seat_keyboard_clear_focus(server->seat);
		wlr_seat_pointer_clear_focus(server->seat);
		ipc_notify_focus(server);
	}
	server->lock_requested = 0;
	redraw_all(server);
	idle_rearm(server);
	/* With every screen off, there's nothing to draw, so nothing shows:
	 * locked now (a laptop locking as it goes to sleep). */
	bool any = false;
	struct output *o;
	wl_list_for_each(o, &server->outputs, link) {
		any |= o->wlr_output->enabled;
	}
	if (!any) {
		lock->locked_sent = true;
		wlr_session_lock_v1_send_locked(wlr);
	}
}

/* After each output's frame: once every output has drawn one since
 * locking, the locker is told it's locked. */
void lock_output_frame(struct output *output) {
	struct server *server = output->server;
	output->lock_rendered = true;
	if (server->lock == NULL || server->lock->locked_sent) {
		return;
	}
	struct output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->enabled && !o->lock_rendered) {
			return;
		}
	}
	server->lock->locked_sent = true;
	wlr_session_lock_v1_send_locked(server->lock->wlr);
}

bool lock_shown(struct server *server) {
	return server->lock != NULL && server->lock->locked_sent;
}

/* The locker: [idle] locker, else gemwm-lock. Once at a time. */
void lock_request(struct server *server) {
	if (server->greeter || server->lock != NULL) {
		return;
	}
	int64_t now = now_ms();
	if (server->lock_requested != 0 && now - server->lock_requested < 5000) {
		return; /* it's starting */
	}
	server->lock_requested = now;
	spawn(server->lock_command != NULL ? server->lock_command : "gemwm-lock");
}

static int relock(void *data) {
	struct server *server = data;
	if (server->locked && server->lock == NULL) {
		server->lock_requested = 0;
		lock_request(server);
		/* And again in a while, if it still hasn't come. */
		wl_event_source_timer_update(server->relock_timer, 5000);
	}
	return 0;
}

/* Where the pointer is, while locked: a lock surface, or nothing. */
struct wlr_surface *lock_surface_at(struct server *server, double lx,
		double ly, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&server->layer_lock->node,
		lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_surface *s = wlr_scene_surface_try_from_buffer(
		wlr_scene_buffer_from_node(node));
	return s != NULL ? s->surface : NULL;
}

/* An output came, went, or changed size: the lock surfaces follow. */
void lock_outputs_changed(struct server *server) {
	if (server->lock == NULL) {
		return;
	}
	struct lock_surface *ls;
	wl_list_for_each(ls, &server->lock->surfaces, link) {
		place_surface(ls);
	}
}

/* ---- Idle ---------------------------------------------------------------- */

static void screens_power(struct server *server, bool on) {
	if (server->screens_off == !on) {
		return;
	}
	server->screens_off = !on;
	wlr_log(WLR_INFO, "Screens %s", on ? "on" : "off");
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, on);
		wlr_output_commit_state(output->wlr_output, &state);
		wlr_output_state_finish(&state);
		if (on) {
			output->lock_rendered = false;
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static int idle_lock_timeout(void *data) {
	struct server *server = data;
	if (server->inhibitors == 0) {
		wlr_log(WLR_INFO, "Idle: locking");
		lock_request(server);
	}
	return 0;
}

static int idle_off_timeout(void *data) {
	struct server *server = data;
	if (server->inhibitors == 0) {
		screens_power(server, false);
	}
	return 0;
}

/* The timers from now: lock after [idle] lock minutes, screens off after
 * screen-off (a minute when locked); neither while held off. */
static void idle_rearm(struct server *server) {
	if (server->idle_lock_timer == NULL) {
		return;
	}
	bool held = server->inhibitors > 0;
	int lock_ms = !held && !server->locked && !server->greeter &&
		server->idle_lock_min > 0 ? server->idle_lock_min * 60000 : 0;
	int off_min = server->locked ? 1 : server->idle_off_min;
	int off_ms = !held && off_min > 0 && server->idle_off_min > 0 ?
		off_min * 60000 : 0;
	wl_event_source_timer_update(server->idle_lock_timer, lock_ms);
	wl_event_source_timer_update(server->idle_off_timer, off_ms);
}

/* Any input: not idle, the screens on, and the timers from now. */
void idle_activity(struct server *server) {
	if (server->idle_notifier == NULL) {
		return;
	}
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	screens_power(server, true);
	/* Not on every motion event: at most every half second. */
	int64_t now = now_ms();
	if (now - server->idle_last_rearm >= 500) {
		server->idle_last_rearm = now;
		idle_rearm(server);
	}
}

/* [idle] read again. */
void idle_configure(struct server *server) {
	idle_rearm(server);
}

struct inhibitor {
	struct server *server;
	struct wl_listener destroy;
};

static void inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct inhibitor *in = wl_container_of(listener, in, destroy);
	struct server *server = in->server;
	wl_list_remove(&in->destroy.link);
	free(in);
	server->inhibitors--;
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier,
		server->inhibitors > 0);
	if (server->inhibitors == 0) {
		server->idle_last_rearm = 0;
		idle_rearm(server);
	}
}

static void new_inhibitor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr = data;
	struct inhibitor *in = calloc(1, sizeof(*in));
	in->server = server;
	in->destroy.notify = inhibitor_destroy;
	wl_signal_add(&wlr->events.destroy, &in->destroy);
	server->inhibitors++;
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, true);
	idle_rearm(server);
}

void lock_init(struct server *server) {
	server->layer_lock = wlr_scene_tree_create(&server->scene->tree);
	wlr_scene_node_set_enabled(&server->layer_lock->node, false);
	server->lock_manager = wlr_session_lock_manager_v1_create(server->wl_display);
	server->new_lock.notify = new_lock;
	wl_signal_add(&server->lock_manager->events.new_lock, &server->new_lock);

	server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
	struct wlr_idle_inhibit_manager_v1 *inhibit =
		wlr_idle_inhibit_v1_create(server->wl_display);
	server->new_inhibitor.notify = new_inhibitor;
	wl_signal_add(&inhibit->events.new_inhibitor, &server->new_inhibitor);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	server->idle_lock_timer = wl_event_loop_add_timer(loop, idle_lock_timeout,
		server);
	server->idle_off_timer = wl_event_loop_add_timer(loop, idle_off_timeout,
		server);
	server->relock_timer = wl_event_loop_add_timer(loop, relock, server);
	idle_rearm(server);
}

void lock_finish(struct server *server) {
	wl_list_remove(&server->new_lock.link);
	wl_list_remove(&server->new_inhibitor.link);
	wl_event_source_remove(server->idle_lock_timer);
	wl_event_source_remove(server->idle_off_timer);
	wl_event_source_remove(server->relock_timer);
	server->idle_lock_timer = NULL;
}
