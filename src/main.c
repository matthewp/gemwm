#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "server.h"

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	bool swallowed[KEY_CNT]; /* bound keys: hide their release from clients too */
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

void spawn(const char *cmd) {
	/* Double fork so the child is reparented to init and we never have to
	 * reap it. */
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		/* The event loop blocks SIGTERM and SIGINT to receive them (see
		 * main); a blocked mask is inherited, so unblock them for the
		 * program, or it could never be told to quit. */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

/* The greeter (gemwm -G): run as our own child, not double-forked, so we
 * know when it's done: then so are we, and greetd starts the session it
 * chose. */
static int greeter_exited(int fd, uint32_t mask, void *data) {
	struct server *server = data;
	waitpid(-1, NULL, WNOHANG);
	wlr_log(WLR_INFO, "The greeter has finished");
	wl_display_terminate(server->wl_display);
	return 0;
}

static void spawn_greeter(struct server *server, const char *cmd) {
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
	int fd = pid > 0 ? pidfd_open(pid, 0) : -1;
	if (fd < 0) {
		wlr_log(WLR_ERROR, "Couldn't start the greeter: %s", cmd);
		wl_display_terminate(server->wl_display);
		return;
	}
	wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), fd,
		WL_EVENT_READABLE, greeter_exited, server);
}

/* Starts the menu bar: the gemwm-menu next to our own binary (so it works
 * straight from the build directory), or else the one in $PATH. */
static void spawn_menu(void) {
	char path[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n > 0) {
		path[n] = '\0';
		char *slash = strrchr(path, '/');
		if (slash != NULL && (size_t)(slash - path) + sizeof("/gemwm-menu") <=
				sizeof(path)) {
			strcpy(slash, "/gemwm-menu");
			if (access(path, X_OK) == 0) {
				spawn(path);
				return;
			}
		}
	}
	spawn("gemwm-menu");
}

/* SIGTERM/SIGINT end the session cleanly, like Alt+Escape. */
static int handle_terminate(int signal, void *data) {
	struct server *server = data;
	wl_display_terminate(server->wl_display);
	return 0;
}

/* ---- Keyboard ----------------------------------------------------------- */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	struct server *server = keyboard->server;
	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat,
		&keyboard->wlr_keyboard->modifiers);

	uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	/* Letting go of the cycling key's modifier settles on the window. */
	if (!(mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL))) {
		view_cycle_end(server);
	}
	bool super = mods & WLR_MODIFIER_LOGO;
	if (super != server->super_held) {
		server->super_held = super;
		highlight_update(server);
	}
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	uint32_t code = event->keycode;

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		uint32_t keycode = code + 8;
		struct xkb_state *state = keyboard->wlr_keyboard->xkb_state;
		const xkb_keysym_t *syms, *base;
		int nsyms = xkb_state_key_get_syms(state, keycode, &syms);
		int nbase = xkb_keymap_key_get_syms_by_level(
			keyboard->wlr_keyboard->keymap, keycode,
			xkb_state_key_get_layout(state, keycode), 0, &base);
		handled = bindings_handle(server,
			wlr_keyboard_get_modifiers(keyboard->wlr_keyboard),
			base, nbase, syms, nsyms);
		if (code < KEY_CNT) {
			keyboard->swallowed[code] = handled;
		}
	} else if (code < KEY_CNT && keyboard->swallowed[code]) {
		keyboard->swallowed[code] = false;
		handled = true;
	}
	if (!handled) {
		wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/* Layout etc. come from XKB_DEFAULT_* environment variables. */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);

	/* A keyboard that appears later (plugged in, or virtual) still has to
	 * reach whoever should have focus: an exclusive layer surface such as
	 * an alert box first, otherwise the focused window. */
	struct wlr_surface *target = layers_exclusive_focus(server);
	if (target == NULL && server->focused_view != NULL) {
		target = server->focused_view->xdg_toplevel->base->surface;
	}
	if (target != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat, target,
			wlr_keyboard->keycodes, wlr_keyboard->num_keycodes,
			&wlr_keyboard->modifiers);
	}
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* Pointers created by clients (wlrctl, test tools), handled like real ones. */
static void server_new_virtual_pointer(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor,
		&event->new_pointer->pointer.base);
}

/* Keyboards created by clients (test tools, on-screen keyboards). */
static void server_new_virtual_keyboard(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	server_new_keyboard(server, &keyboard->keyboard.base);
	wlr_seat_set_capabilities(server->seat,
		WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ---- Pointer ------------------------------------------------------------ */

static void process_cursor_motion(struct server *server, uint32_t time) {
	if (server->cursor_mode != CURSOR_PASSTHROUGH) {
		process_interactive_motion(server);
		return;
	}

	double sx, sy;
	bool on_frame;
	struct wlr_surface *surface = NULL;
	struct view *view = view_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy, &on_frame);
	if (view == NULL || on_frame) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface != NULL) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

/* Re-picks what's under a pointer that hasn't moved, after something
 * appeared or went away beneath it; otherwise the next click would go to
 * whatever used to be there (or nowhere). */
void cursor_rebase(struct server *server) {
	if (server->cursor_mode != CURSOR_PASSTHROUGH) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	process_cursor_motion(server, now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	/* During an outline drag the button belongs to us, not the client. */
	if (server->cursor_mode != CURSOR_PASSTHROUGH) {
		if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
			end_interactive(server);
			process_cursor_motion(server, event->time_msec);
		}
		return;
	}

	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);
	if (event->state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}

	double sx, sy;
	bool on_frame;
	struct wlr_surface *surface = NULL;
	struct view *view = view_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy, &on_frame);
	if (view == NULL) {
		/* Layer surfaces that want the keyboard get it when clicked. */
		struct wlr_layer_surface_v1 *layer = surface == NULL ? NULL :
			wlr_layer_surface_v1_try_from_wlr_surface(
				wlr_surface_get_root_surface(surface));
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		if (layer != NULL && kb != NULL && layer->current.keyboard_interactive !=
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			wlr_seat_keyboard_notify_enter(server->seat, layer->surface,
				kb->keycodes, kb->num_keycodes, &kb->modifiers);
		}
		return;
	}
	/* Like GEM, a click on a background window's frame only tops it; its
	 * gadgets work once it is on top. */
	bool was_focused = server->focused_view == view;
	focus_view(view);
	if (on_frame && was_focused && event->button == BTN_LEFT) {
		view_frame_click(view, sx, sy, event->time_msec);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	/* Super+wheel scrolls the strip a column at a time, as in niri. */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (server->mode == MODE_SCROLLING && kb != NULL &&
			(wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO) &&
			event->source == WL_POINTER_AXIS_SOURCE_WHEEL &&
			event->delta_discrete != 0) {
		scroll_focus_direction(server, event->delta_discrete > 0 ?
			"right" : "left");
		return;
	}
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* ---- Outputs ------------------------------------------------------------ */

static void output_frame(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		output->server->scene, output->wlr_output);
	/* Something moving asks for the frame after this one too. */
	if (animate_tick(output->server)) {
		wlr_output_schedule_frame(output->wlr_output);
	}
	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* E.g. the nested window was resized. */
	struct output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	desktop_update_output(output);
	layers_arrange(output);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, destroy);
	layers_output_destroyed(output);
	output->wlr_output->data = NULL;
	if (output->desktop != NULL) {
		wlr_scene_node_destroy(&output->desktop->node);
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

/* Whole-number scale from the panel's pixel density, so the Atari pixels
 * stay crisp: one step per 96 DPI. Nested outputs report no physical size
 * and get 1. */
static float auto_scale(struct wlr_output *output,
		struct wlr_output_mode *mode) {
	int width = mode != NULL ? mode->width : output->width;
	if (output->phys_width <= 0 || width <= 0) {
		return 1.0f;
	}
	int scale = (int)(width * 25.4 / output->phys_width / 96.0);
	return scale < 1 ? 1.0f : (float)scale;
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_state_set_scale(&state, server->output_scale > 0 ?
		server->output_scale : auto_scale(wlr_output, mode));
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	wlr_output->data = output;
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output,
		scene_output);

	desktop_update_output(output);
	layers_arrange(output);
}

/* ---- Main --------------------------------------------------------------- */

static void usage(const char *argv0) {
	printf("Usage: %s [-s startup command] [-S output scale, default auto] [-M]\n"
		"       %s -G greeter   (the login screen, for greetd)\n"
		"\n"
		"  -M          don't start the gemwm-menu menu bar\n"
		"  -G greeter  run the greeter, frameless under the menu bar, with no\n"
		"              key bindings and /etc/gemwm/greeter.conf; quit when it\n"
		"              does\n"
		"\n"
		"       %s msg <command>   control a running GemWM (no command: help)\n"
		"\n"
		"Key bindings are set in ~/.config/gemwm/config (see README).\n",
		argv0, argv0, argv0);
}

int main(int argc, char *argv[]) {
	if (argc > 1 && strcmp(argv[1], "msg") == 0) {
		return ipc_client_main(argc - 2, argv + 2);
	}
	wlr_log_init(WLR_INFO, NULL);
	char *startup_cmd = NULL;
	bool start_menu = true;
	struct server server = {0};
	server.output_scale = 0; /* auto */

	int c;
	while ((c = getopt(argc, argv, "s:S:G:Mh")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'G':
			server.greeter = true;
			startup_cmd = optarg;
			/* For the menu bar, which then shows only what's safe
			 * before anyone's logged in. */
			setenv("GEMWM_GREETER", "1", true);
			break;
		case 'M':
			start_menu = false;
			break;
		case 'S':
			server.output_scale = strtof(optarg, NULL);
			if (server.output_scale < 0) {
				server.output_scale = 0;
			}
			break;
		default:
			usage(argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		usage(argv[0]);
		return 0;
	}

	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.wl_display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	wlr_viewporter_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	/* Lets tools like grim take screenshots. */
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);
	struct wlr_scene_tree *root = &server.scene->tree;
	server.layer_desktop = wlr_scene_tree_create(root);
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(root);
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(root);
	server.layer_views = wlr_scene_tree_create(root);
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(root);
	server.layer_fullscreen = wlr_scene_tree_create(root);
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(root);
	server.layer_drag = wlr_scene_tree_create(root);

	view_init_shell(&server);
	scrollbars_init(&server);
	app_menus_init(&server);
	workspaces_init(&server);
	config_load(&server);
	layers_init(&server);
	view_init_decorations(&server);
	view_init_activation(&server);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.cursor_mode = CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	/* We always draw a cursor, even before any pointer device appears. */
	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER);
	struct wlr_virtual_pointer_manager_v1 *vptr =
		wlr_virtual_pointer_manager_v1_create(server.wl_display);
	server.new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&vptr->events.new_virtual_pointer,
		&server.new_virtual_pointer);
	struct wlr_virtual_keyboard_manager_v1 *vkbd =
		wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&vkbd->events.new_virtual_keyboard,
		&server.new_virtual_keyboard);
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	ipc_init(&server, socket); /* sets GEMWM_SOCKET for what we start */
	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
	struct wl_event_source *sigterm = wl_event_loop_add_signal(loop, SIGTERM,
		handle_terminate, &server);
	struct wl_event_source *sigint = wl_event_loop_add_signal(loop, SIGINT,
		handle_terminate, &server);
	if (start_menu) {
		spawn_menu();
	}
	if (startup_cmd && server.greeter) {
		spawn_greeter(&server, startup_cmd);
	} else if (startup_cmd) {
		spawn(startup_cmd);
	}
	wlr_log(WLR_INFO, "Running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.wl_display);

	wl_event_source_remove(sigterm);
	wl_event_source_remove(sigint);
	ipc_finish(&server);
	config_finish(&server);
	wl_display_destroy_clients(server.wl_display);
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_xdg_decoration.link);
	wl_list_remove(&server.new_kde_decoration.link);
	wl_list_remove(&server.request_activate.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_virtual_pointer.link);
	wl_list_remove(&server.new_virtual_keyboard.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.new_output.link);

	/* Outputs are destroyed with the backend, after the scene: drop their
	 * scene nodes now so output_destroy doesn't touch freed ones. */
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->desktop != NULL) {
			wlr_scene_node_destroy(&output->desktop->node);
			output->desktop = NULL;
		}
	}
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	return 0;
}
