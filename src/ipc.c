#define _GNU_SOURCE /* accept4 */
/*
 * The control socket: lets scripts (and the menu bar) list and target
 * workspaces and windows.
 *
 * Clients send one command per line and get one JSON object per line back.
 * After "subscribe", a client also receives event objects whenever
 * workspaces or windows change. `gemwm msg <command>` is the command-line
 * client. The socket path is in $GEMWM_SOCKET for programs we start.
 *
 * Commands:
 *   workspaces                    list workspaces
 *   windows                       list windows
 *   workspace <n>|new|next|prev   switch workspace (n = count+1 makes one)
 *   move-window <id>|focused <n>|new
 *   focus-window <id>             switches to its workspace if needed
 *   maximize <id>|focused         toggle: a tile fills the screen, or a
 *                                 window maximizes (the fuller gadget)
 *   close-window <id>|focused
 *   cycle-windows next|prev       focus the next/previous window (window mode)
 *   mode [window|tiling|scrolling|toggle]  report or change the window
 *                                 mode (toggle steps through the three)
 *   focus-direction left|right|up|down  focus the neighbouring window
 *                                 (tiling and scrolling)
 *   move-direction left|right|up|down   scrolling: move the column (left,
 *                                 right) or the window in its column
 *   consume-or-expel left|right   scrolling: join the neighbouring column,
 *                                 or leave a shared one
 *   column-width cycle|<fraction> scrolling: the focused column's width
 *   exec <shell command>          run a program
 *   reload-config                 re-read ~/.config/gemwm/config
 *   quit                          end the session
 *   menu-activate <id> <item>     pick an item of the window's own menus
 *                                 (gemwm-app-menu-v1; for the menu bar)
 *   subscribe                     stream "workspaces", "windows", "focus"
 *                                 and "mode" events
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "server.h"

#define MAX_LINE 4096
#define MAX_PENDING (1 << 20) /* drop subscribers that stop reading */

struct ipc_client {
	struct wl_list link;
	struct server *server;
	int fd;
	struct wl_event_source *source;
	char in[MAX_LINE];
	size_t in_len;
	char *out;
	size_t out_len, out_cap;
	bool subscribed;
	bool dead; /* write failed; closed from its own handler */
};

/* ---- JSON output -------------------------------------------------------- */

struct strbuf {
	char *data;
	size_t len, cap;
};

static void sb_append(struct strbuf *sb, const char *s, size_t n) {
	if (sb->len + n + 1 > sb->cap) {
		size_t cap = sb->cap ? sb->cap : 256;
		while (sb->len + n + 1 > cap) {
			cap *= 2;
		}
		sb->data = realloc(sb->data, cap);
		sb->cap = cap;
	}
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = '\0';
}

static void sb_printf(struct strbuf *sb, const char *fmt, ...) {
	char tmp[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0) {
		sb_append(sb, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
	}
}

static void sb_json_string(struct strbuf *sb, const char *s) {
	sb_append(sb, "\"", 1);
	for (; s != NULL && *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			char esc[2] = { '\\', (char)c };
			sb_append(sb, esc, 2);
		} else if (c < 0x20) {
			sb_printf(sb, "\\u%04x", c);
		} else {
			sb_append(sb, (const char *)&c, 1);
		}
	}
	sb_append(sb, "\"", 1);
}

/* {"active":2,"count":3,"workspaces":[{"number":1,"active":false,
 * "windows":[4,7]},...]}. The menu bar reads "active" and "count" from the
 * start of the event form, so keep them first. */
static void json_workspaces(struct server *server, struct strbuf *sb,
		const char *event) {
	sb_append(sb, "{", 1);
	if (event != NULL) {
		sb_printf(sb, "\"event\":\"%s\",", event);
	}
	sb_printf(sb, "\"active\":%d,\"count\":%d,\"workspaces\":[",
		workspace_number(server->active_workspace), workspace_count(server));
	int n = 1;
	struct workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		sb_printf(sb, "%s{\"number\":%d,\"active\":%s,\"windows\":[",
			n > 1 ? "," : "", n, ws == server->active_workspace ? "true" : "false");
		bool first = true;
		struct view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->workspace == ws) {
				sb_printf(sb, "%s%u", first ? "" : ",", view->id);
				first = false;
			}
		}
		sb_append(sb, "]}", 2);
		n++;
	}
	sb_append(sb, "]}", 2);
}

/* {"windows":[{"id":4,"title":"...","app_id":"foot","workspace":1,
 * "focused":true,"tiled":false,"x":..,"y":..,"width":..,"height":..},...]},
 * front to back; the box is the window's frame, in screen coordinates. */
static void json_windows(struct server *server, struct strbuf *sb,
		const char *event) {
	sb_append(sb, "{", 1);
	if (event != NULL) {
		sb_printf(sb, "\"event\":\"%s\",", event);
	}
	sb_append(sb, "\"windows\":[", 11);
	bool first = true;
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		sb_printf(sb, "%s{\"id\":%u,\"title\":", first ? "" : ",", view->id);
		sb_json_string(sb, view->xdg_toplevel->title);
		sb_append(sb, ",\"app_id\":", 10);
		sb_json_string(sb, view->xdg_toplevel->app_id);
		struct wlr_box box;
		view_frame_box(view, &box);
		bool maximized = view->maximized ||
			(view->workspace != NULL && view->workspace->zoomed == view);
		sb_printf(sb, ",\"workspace\":%d,\"focused\":%s,\"tiled\":%s,"
			"\"maximized\":%s,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
			view->workspace ? workspace_number(view->workspace) : 0,
			server->focused_view == view ? "true" : "false",
			view_is_tiled(view) ? "true" : "false",
			maximized ? "true" : "false",
			box.x, box.y, box.width, box.height);
		first = false;
	}
	sb_append(sb, "]}", 2);
}

/* The focused window, for the menu bar's window menu, flat so it's easy to
 * read: {"window":3,"workspace":1,"maximized":false,"app_id":"foot"}, or
 * {"window":0} when no window has focus. A window with menus of its own
 * adds "menus", their text (see appmenu.c), last. */
static void json_focus(struct server *server, struct strbuf *sb,
		const char *event) {
	sb_append(sb, "{", 1);
	if (event != NULL) {
		sb_printf(sb, "\"event\":\"%s\",", event);
	}
	struct view *view = server->focused_view;
	if (view == NULL) {
		sb_append(sb, "\"window\":0}", 11);
		return;
	}
	bool maximized = view->maximized ||
		(view->workspace != NULL && view->workspace->zoomed == view);
	sb_printf(sb, "\"window\":%u,\"workspace\":%d,\"maximized\":%s,"
		"\"app_id\":", view->id,
		view->workspace ? workspace_number(view->workspace) : 0,
		maximized ? "true" : "false");
	sb_json_string(sb, view->xdg_toplevel->app_id);
	if (view->app_menus != NULL) {
		sb_printf(sb, ",\"menus\":");
		sb_json_string(sb, view->app_menus);
	}
	sb_append(sb, "}", 1);
}

/* {"mode":"tiling"} */
static void json_mode(struct server *server, struct strbuf *sb,
		const char *event) {
	sb_append(sb, "{", 1);
	if (event != NULL) {
		sb_printf(sb, "\"event\":\"%s\",", event);
	}
	static const char *names[] = { "window", "tiling", "scrolling" };
	sb_printf(sb, "\"mode\":\"%s\",\"gap\":%d}",
		names[server->mode], server->gap);
}

/* ---- Connections -------------------------------------------------------- */

static void client_update_mask(struct ipc_client *client) {
	uint32_t mask = WL_EVENT_READABLE;
	if (client->out_len > 0) {
		mask |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(client->source, mask);
}

static void client_flush(struct ipc_client *client) {
	while (client->out_len > 0 && !client->dead) {
		ssize_t n = write(client->fd, client->out, client->out_len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				break;
			}
			client->dead = true;
			client->out_len = 0;
			break;
		}
		memmove(client->out, client->out + n, client->out_len - n);
		client->out_len -= n;
	}
	client_update_mask(client);
}

/* Queues one line (a JSON object) for the client. */
static void client_send(struct ipc_client *client, const char *line) {
	if (client->dead) {
		return;
	}
	size_t n = strlen(line);
	if (client->out_len + n + 1 > MAX_PENDING) {
		client->dead = true;
		return;
	}
	if (client->out_len + n + 1 > client->out_cap) {
		client->out_cap = (client->out_len + n + 1) * 2;
		client->out = realloc(client->out, client->out_cap);
	}
	memcpy(client->out + client->out_len, line, n);
	client->out[client->out_len + n] = '\n';
	client->out_len += n + 1;
	client_flush(client);
}

static void client_close(struct ipc_client *client) {
	wl_event_source_remove(client->source);
	close(client->fd);
	wl_list_remove(&client->link);
	free(client->out);
	free(client);
}

static void broadcast(struct server *server,
		void (*json)(struct server *, struct strbuf *, const char *),
		const char *event) {
	struct strbuf sb = {0};
	struct ipc_client *client;
	wl_list_for_each(client, &server->ipc_clients, link) {
		if (client->subscribed) {
			if (sb.data == NULL) {
				json(server, &sb, event);
			}
			client_send(client, sb.data);
		}
	}
	free(sb.data);
}

void ipc_notify_workspaces(struct server *server) {
	broadcast(server, json_workspaces, "workspaces");
}

void ipc_notify_windows(struct server *server) {
	broadcast(server, json_windows, "windows");
	broadcast(server, json_focus, "focus");
}

void ipc_notify_focus(struct server *server) {
	broadcast(server, json_focus, "focus");
}

void ipc_notify_mode(struct server *server) {
	broadcast(server, json_mode, "mode");
}

/* ---- Commands ----------------------------------------------------------- */

static struct view *view_by_id(struct server *server, const char *arg) {
	if (arg == NULL) {
		return NULL;
	}
	if (strcmp(arg, "focused") == 0) {
		return server->focused_view;
	}
	char *end;
	unsigned long id = strtoul(arg, &end, 10);
	if (*end != '\0') {
		return NULL;
	}
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->id == id) {
			return view;
		}
	}
	return NULL;
}

/* A workspace named by a command: a number, "new", "next" or "prev". */
static struct workspace *workspace_by_arg(struct server *server,
		const char *arg) {
	if (arg == NULL) {
		return NULL;
	}
	int count = workspace_count(server);
	int current = workspace_number(server->active_workspace);
	if (strcmp(arg, "new") == 0) {
		return workspace_create(server);
	}
	if (strcmp(arg, "next") == 0) {
		return workspace_nth(server, current % count + 1);
	}
	if (strcmp(arg, "prev") == 0) {
		return workspace_nth(server, (current + count - 2) % count + 1);
	}
	char *end;
	long n = strtol(arg, &end, 10);
	if (*end != '\0' || n < 1) {
		return NULL;
	}
	return workspace_for_target(server, (int)n);
}

static const char *const ok = "{\"ok\":true}";

static char *reply_error(const char *message) {
	struct strbuf sb = {0};
	sb_append(&sb, "{\"error\":", 9);
	sb_json_string(&sb, message);
	sb_append(&sb, "}", 1);
	return sb.data;
}

/* Runs one command and returns its JSON reply (to be freed). `client` is
 * the socket client, or NULL for key bindings; "subscribe" needs one. */
static char *execute(struct server *server, struct ipc_client *client,
		char *line) {
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	/* exec takes the rest of the line as a shell command. */
	if (strncmp(line, "exec", 4) == 0 && (line[4] == ' ' || line[4] == '\t')) {
		spawn(line + 5);
		return strdup(ok);
	}

	char *argv[4] = {0};
	int argc = 0;
	char *save = NULL;
	for (char *tok = strtok_r(line, " \t\r", &save); tok != NULL && argc < 4;
			tok = strtok_r(NULL, " \t\r", &save)) {
		argv[argc++] = tok;
	}
	if (argc == 0) {
		return reply_error("empty command");
	}
	const char *cmd = argv[0];
	struct strbuf sb = {0};

	if (strcmp(cmd, "workspaces") == 0) {
		json_workspaces(server, &sb, NULL);
		return sb.data;
	}
	if (strcmp(cmd, "windows") == 0) {
		json_windows(server, &sb, NULL);
		return sb.data;
	}
	if (strcmp(cmd, "workspace") == 0) {
		struct workspace *ws = workspace_by_arg(server, argv[1]);
		if (ws == NULL) {
			return reply_error("no such workspace");
		}
		workspace_switch(server, ws);
		return strdup(ok);
	}
	if (strcmp(cmd, "move-window") == 0) {
		struct view *view = view_by_id(server, argv[1]);
		if (view == NULL) {
			return reply_error("no such window");
		}
		struct workspace *ws = workspace_by_arg(server, argv[2]);
		if (ws == NULL) {
			return reply_error("no such workspace");
		}
		view_set_workspace(view, ws);
		workspace_prune(server); /* in case "new" went unused */
		return strdup(ok);
	}
	if (strcmp(cmd, "menu-activate") == 0) {
		struct view *view = view_by_id(server, argv[1]);
		if (view == NULL || argv[2] == NULL) {
			return reply_error("usage: menu-activate <window> <item>");
		}
		view_menu_activate(view, (uint32_t)strtoul(argv[2], NULL, 10));
		return strdup(ok);
	}
	if (strcmp(cmd, "focus-window") == 0) {
		struct view *view = view_by_id(server, argv[1]);
		if (view == NULL) {
			return reply_error("no such window");
		}
		focus_view(view);
		return strdup(ok);
	}
	if (strcmp(cmd, "maximize") == 0) {
		struct view *view = view_by_id(server, argv[1] ? argv[1] : "focused");
		if (view == NULL) {
			return reply_error("no such window");
		}
		view_toggle_maximize_or_zoom(view);
		return strdup(ok);
	}
	if (strcmp(cmd, "close-window") == 0) {
		struct view *view = view_by_id(server, argv[1]);
		if (view == NULL) {
			return reply_error("no such window");
		}
		wlr_xdg_toplevel_send_close(view->xdg_toplevel);
		return strdup(ok);
	}
	if (strcmp(cmd, "cycle-windows") == 0) {
		int direction = argv[1] != NULL && strcmp(argv[1], "prev") == 0 ? -1 : 1;
		/* From a key, the cycle lasts while its modifier is held. */
		view_cycle(server, direction, client == NULL);
		return strdup(ok);
	}
	if (strcmp(cmd, "mode") == 0) {
		if (argv[1] == NULL) {
			json_mode(server, &sb, NULL);
			return sb.data;
		}
		if (strcmp(argv[1], "window") == 0) {
			tile_set_mode(server, MODE_WINDOW);
		} else if (strcmp(argv[1], "tiling") == 0) {
			tile_set_mode(server, MODE_TILING);
		} else if (strcmp(argv[1], "scrolling") == 0) {
			tile_set_mode(server, MODE_SCROLLING);
		} else if (strcmp(argv[1], "toggle") == 0) {
			tile_set_mode(server, (server->mode + 1) % 3);
		} else {
			return reply_error("modes are window, tiling and scrolling");
		}
		return strdup(ok);
	}
	if (strcmp(cmd, "focus-direction") == 0) {
		if (argv[1] == NULL || (strcmp(argv[1], "left") != 0 &&
				strcmp(argv[1], "right") != 0 && strcmp(argv[1], "up") != 0 &&
				strcmp(argv[1], "down") != 0)) {
			return reply_error("directions are left, right, up and down");
		}
		/* In window mode the keys do nothing. */
		if (server->mode == MODE_TILING) {
			tile_focus_direction(server, argv[1]);
		} else if (server->mode == MODE_SCROLLING) {
			scroll_focus_direction(server, argv[1]);
		}
		return strdup(ok);
	}
	if (strcmp(cmd, "move-direction") == 0 ||
			strcmp(cmd, "consume-or-expel") == 0) {
		bool consume = cmd[0] == 'c';
		if (argv[1] == NULL || (strcmp(argv[1], "left") != 0 &&
				strcmp(argv[1], "right") != 0 && (consume ||
				(strcmp(argv[1], "up") != 0 && strcmp(argv[1], "down") != 0)))) {
			return reply_error(consume ? "directions are left and right" :
				"directions are left, right, up and down");
		}
		if (server->mode == MODE_SCROLLING) {
			if (consume) {
				scroll_consume_or_expel(server, argv[1]);
			} else {
				scroll_move(server, argv[1]);
			}
		}
		return strdup(ok);
	}
	if (strcmp(cmd, "column-width") == 0) {
		if (server->mode == MODE_SCROLLING &&
				!scroll_column_width(server, argv[1])) {
			return reply_error("widths are cycle or a fraction like 0.5");
		}
		return strdup(ok);
	}
	if (strcmp(cmd, "reload-config") == 0) {
		config_load(server);
		return strdup(ok);
	}
	if (strcmp(cmd, "quit") == 0) {
		wl_display_terminate(server->wl_display);
		return strdup(ok);
	}
	if (strcmp(cmd, "subscribe") == 0) {
		if (client == NULL) {
			return reply_error("subscribe needs a socket");
		}
		client->subscribed = true;
		client_send(client, ok);
		json_workspaces(server, &sb, "workspaces");
		client_send(client, sb.data);
		sb.len = 0;
		json_windows(server, &sb, "windows");
		client_send(client, sb.data);
		sb.len = 0;
		json_focus(server, &sb, "focus");
		client_send(client, sb.data);
		sb.len = 0;
		json_mode(server, &sb, "mode");
		client_send(client, sb.data);
		free(sb.data);
		return NULL; /* replies already sent */
	}
	return reply_error("unknown command");
}

static void handle_command(struct ipc_client *client, char *line) {
	char *reply = execute(client->server, client, line);
	if (reply != NULL) {
		client_send(client, reply);
		free(reply);
	}
}

/* Runs a command from a key binding; errors go to the log. */
void ipc_run_command(struct server *server, const char *command) {
	char *line = strdup(command);
	char *reply = execute(server, NULL, line);
	if (reply != NULL && strncmp(reply, "{\"error\"", 8) == 0) {
		wlr_log(WLR_ERROR, "\"%s\": %s", command, reply);
	}
	free(reply);
	free(line);
}

static int client_handle(int fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;
	if (mask & WL_EVENT_WRITABLE) {
		client_flush(client);
	}
	if (mask & WL_EVENT_READABLE) {
		ssize_t n = read(fd, client->in + client->in_len,
			sizeof(client->in) - client->in_len);
		if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EINTR))) {
			client->dead = true;
		} else if (n > 0) {
			client->in_len += n;
			char *start = client->in, *nl;
			while ((nl = memchr(start, '\n',
					client->in + client->in_len - start)) != NULL) {
				*nl = '\0';
				handle_command(client, start);
				start = nl + 1;
			}
			client->in_len -= start - client->in;
			memmove(client->in, start, client->in_len);
			if (client->in_len == sizeof(client->in)) {
				client->dead = true; /* a line longer than we accept */
			}
		}
	}
	if (client->dead || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
		client_close(client);
	}
	return 0;
}

static int handle_accept(int fd, uint32_t mask, void *data) {
	struct server *server = data;
	int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (cfd < 0) {
		return 0;
	}
	struct ipc_client *client = calloc(1, sizeof(*client));
	client->server = server;
	client->fd = cfd;
	client->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->wl_display), cfd,
		WL_EVENT_READABLE, client_handle, client);
	wl_list_insert(&server->ipc_clients, &client->link);
	return 0;
}

static bool socket_path(char *path, size_t size, const char *wayland_display) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (dir == NULL || wayland_display == NULL) {
		return false;
	}
	int n = snprintf(path, size, "%s/gemwm.%s.sock", dir, wayland_display);
	return n > 0 && (size_t)n < size;
}

bool ipc_init(struct server *server, const char *wayland_display) {
	wl_list_init(&server->ipc_clients);
	server->ipc_fd = -1;
	if (!socket_path(server->ipc_path, sizeof(server->ipc_path),
			wayland_display)) {
		wlr_log(WLR_ERROR, "ipc: no XDG_RUNTIME_DIR, control socket disabled");
		return false;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strcpy(addr.sun_path, server->ipc_path);
	unlink(server->ipc_path);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			listen(fd, 16) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: can't listen on %s", server->ipc_path);
		if (fd >= 0) {
			close(fd);
		}
		server->ipc_path[0] = '\0';
		return false;
	}
	server->ipc_fd = fd;
	server->ipc_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->wl_display), fd,
		WL_EVENT_READABLE, handle_accept, server);
	setenv("GEMWM_SOCKET", server->ipc_path, true);
	return true;
}

void ipc_finish(struct server *server) {
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		client_close(client);
	}
	if (server->ipc_fd >= 0) {
		wl_event_source_remove(server->ipc_source);
		close(server->ipc_fd);
		unlink(server->ipc_path);
	}
}

/* ---- gemwm msg ---------------------------------------------------------- */

/* Sends argv as one command and prints the reply; with "subscribe", keeps
 * printing events until the compositor goes away. */
int ipc_client_main(int argc, char *argv[]) {
	if (argc < 1) {
		fprintf(stderr, "usage: gemwm msg <command> [args...]\n"
			"commands: workspaces, windows, workspace <n|new|next|prev>,\n"
			"  move-window <id|focused> <n|new>, focus-window <id>,\n"
			"  close-window <id|focused>, cycle-windows <next|prev>,\n"
			"  mode [window|tiling|scrolling|toggle],\n"
			"  focus-direction|move-direction <left|right|up|down>,\n"
			"  consume-or-expel <left|right>, column-width <cycle|fraction>,\n"
			"  maximize [id|focused],\n"
			"  exec <command>, reload-config, quit, subscribe\n");
		return 2;
	}
	char path[108];
	const char *env = getenv("GEMWM_SOCKET");
	if (env != NULL && env[0] != '\0') {
		snprintf(path, sizeof(path), "%s", env);
	} else if (!socket_path(path, sizeof(path), getenv("WAYLAND_DISPLAY"))) {
		fprintf(stderr, "gemwm msg: GEMWM_SOCKET is not set\n");
		return 1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "gemwm msg: can't connect to %s: %s\n",
			path, strerror(errno));
		return 1;
	}

	char line[MAX_LINE];
	size_t len = 0;
	for (int i = 0; i < argc; i++) {
		len += snprintf(line + len, sizeof(line) - len, "%s%s",
			i ? " " : "", argv[i]);
		if (len >= sizeof(line) - 1) {
			fprintf(stderr, "gemwm msg: command too long\n");
			return 2;
		}
	}
	line[len++] = '\n';
	if (write(fd, line, len) != (ssize_t)len) {
		fprintf(stderr, "gemwm msg: write failed\n");
		return 1;
	}

	bool follow = strcmp(argv[0], "subscribe") == 0;
	FILE *in = fdopen(fd, "r");
	char *reply = NULL;
	size_t cap = 0;
	int status = 1;
	while (getline(&reply, &cap, in) != -1) {
		fputs(reply, stdout);
		fflush(stdout);
		status = strncmp(reply, "{\"error\"", 8) == 0 ? 1 : 0;
		if (!follow) {
			break;
		}
	}
	free(reply);
	fclose(in);
	return status;
}
