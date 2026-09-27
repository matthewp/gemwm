#ifndef GEMWM_SERVER_H
#define GEMWM_SERVER_H

#include <cairo.h>
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>
#include "frame.h"

/* A key binding from the config: the action is a control-socket command. */
struct binding {
	uint32_t mods;    /* WLR_MODIFIER_* */
	xkb_keysym_t sym; /* lower case */
	char *action;
};

enum cursor_mode {
	CURSOR_PASSTHROUGH,
	CURSOR_MOVE,   /* dragging a window outline by its title bar */
	CURSOR_RESIZE, /* dragging a window outline by its sizer */
	CURSOR_SCROLL, /* dragging a scroll bar's slider */
};

struct server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	/* Scene layers, bottom to top: desktop pattern, layer-shell background
	 * and bottom, windows, layer-shell top and overlay, drag outline. */
	struct wlr_scene_tree *layer_desktop;
	struct wlr_scene_tree *layers[4]; /* indexed by zwlr_layer_shell_v1_layer */
	struct wlr_scene_tree *layer_views;
	struct wlr_scene_tree *layer_drag;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list views; /* view.link, front to back, mapped only */
	struct view *focused_view;
	int cascade; /* placement counter for new windows */
	uint32_t next_view_id;

	struct wl_list workspaces; /* workspace.link, in order */
	struct workspace *active_workspace;

	/* How windows are laid out: freely (GEM style), tiled (tile.c) or on a
	 * scrolling strip of columns (scroll.c). */
	enum layout_mode { MODE_WINDOW, MODE_TILING, MODE_SCROLLING } mode;
	int gap;               /* pixels between tiles, from the config */
	double column_width;   /* scrolling: new columns' share of the screen */
	bool mode_configured;  /* [layout] mode applies at startup only */
	struct wl_list tiles;  /* view.tile_link, in the order windows opened */

	/* From the config file. */
	struct binding *bindings;
	int n_bindings;
	float highlight_color[4];
	int highlight_width;
	bool highlight_tiling; /* in tiling mode, always show the focus border */
	enum { DIM_OFF, DIM_TILING, DIM_ALWAYS } dim; /* grey out unfocused windows */
	float dim_opacity;

	/* The desktop background (desktop.c): a colour or the ST's dither,
	 * with an image over it if one is set. */
	uint32_t desktop_color;
	bool desktop_dither;
	char *desktop_image;              /* path, NULL for none */
	enum image_mode { IMAGE_FILL, IMAGE_FIT, IMAGE_CENTER, IMAGE_TILE,
		IMAGE_STRETCH } desktop_image_mode;
	cairo_surface_t *desktop_picture; /* desktop_image, loaded */

	/* Window cycling (Super+Tab): the windows in most-recently-used order
	 * as it started, walked while the modifier stays down. */
	struct view **cycle_views;
	int cycle_count, cycle_index;
	bool super_held; /* shows the focus highlight */

	/* Control socket, see ipc.c. */
	int ipc_fd;
	char ipc_path[108];
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;
	struct wl_list layer_surfaces; /* layer_surface.link */

	struct wl_listener new_xdg_decoration;
	struct wl_listener new_kde_decoration;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;

	/* Interactive move/resize. GEM style: an outline follows the pointer and
	 * the window only moves/resizes when the button is released. */
	enum cursor_mode cursor_mode;
	struct view *grabbed_view;
	bool grab_vertical;   /* CURSOR_SCROLL: which slider */
	int grab_position;    /* CURSOR_SCROLL: its position when grabbed */
	double grab_x, grab_y;
	struct wlr_box grab_box;  /* frame box when the grab started */
	struct wlr_box drag_box;  /* current outline */
	uint32_t resize_edges;
	struct wlr_scene_tree *outline;
	struct wlr_scene_rect *outline_rects[8];

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
	float output_scale;
};

struct output {
	struct wl_list link;
	struct server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_buffer *desktop; /* the background (desktop.c) */
	struct wlr_box usable_area; /* layout coords, minus panels like the menu bar */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

/* Scrolling mode: a column of windows stacked top to bottom. */
struct column {
	struct wl_list link;  /* workspace.columns, left to right */
	struct wl_list views; /* view.column_link, top to bottom */
	double width;         /* share of the screen */
	double saved_width;   /* width before Super+Z made it full; 0 if not */
	struct view *focus;   /* the window last focused in it */
	int x, w;             /* on the strip, from the last arrange */
};

/* A set of windows shown together. Workspaces are numbered by position:
 * removing an empty one renumbers those after it. */
struct workspace {
	struct wl_list link;
	struct server *server;
	struct wlr_scene_tree *tree; /* the windows; disabled while hidden */
	struct view *zoomed; /* tiling: the tile filling the screen, or NULL */
	struct wl_list columns; /* scrolling: column.link, left to right */
	int scroll_x;           /* scrolling: the strip's offset on screen */
};

struct view {
	struct wl_list link;
	struct server *server;
	uint32_t id; /* stable, for gemwm msg */
	struct wl_list tile_link;    /* server.tiles, while mapped */
	struct column *column;       /* scrolling mode */
	struct wl_list column_link;  /* column.views */
	struct wlr_box float_box;    /* frame box to go back to after tiling */
	struct workspace *workspace; /* NULL while unmapped */
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *tree;       /* positioned at the frame's top-left */
	struct wlr_scene_tree *content;    /* the client's surfaces */
	struct wlr_scene_tree *popups;     /* its menus, placed like content */
	struct wlr_scene_buffer *frame;    /* GEM window chrome */
	struct wlr_scene_tree *highlight;  /* focus border, shown while Super is held */
	struct wlr_scene_rect *highlight_rects[4];
	struct wlr_scene_buffer *dim;      /* GEM "disabled" dither when unfocused */
	int dim_w, dim_h;
	float dim_drawn_opacity;
	int x, y;

	bool ssd; /* we draw the frame; off only if the client insists on CSD */
	struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration;

	bool maximized;
	struct wlr_box saved_box; /* frame box before maximizing */

	/* What the frame was last drawn with, to skip redundant redraws. */
	int drawn_w, drawn_h;
	bool drawn_active;
	char *drawn_title;
	struct frame_style drawn_style;

	/* Scroll bars the client drives (scrollbar.c); off unless it does. */
	struct scroll_client *scroll;
	struct frame_axis scroll_v, scroll_h;

	/* Its own menus for the menu bar (appmenu.c), as the bar reads them;
	 * NULL when it has none. */
	struct app_menu *app_menu;
	char *app_menus;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_title;
};

/* A layer-shell surface: panels, the menu bar, wallpapers. */
struct layer_surface {
	struct wl_list link;
	struct server *server;
	struct wlr_layer_surface_v1 *wlr;
	struct wlr_scene_layer_surface_v1 *scene;
	bool mapped;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

/* view.c */
void view_init_shell(struct server *server);
void view_init_decorations(struct server *server);
void focus_view(struct view *view);
struct view *view_at(struct server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy, bool *on_frame);
void view_frame_box(struct view *view, struct wlr_box *box);
void view_frame_style(struct view *view, struct frame_style *style);
void view_extents(struct view *view, int *w, int *h);
void view_move(struct view *view, int x, int y);
void view_update_frame(struct view *view);
void view_begin_interactive(struct view *view, enum cursor_mode mode,
	uint32_t edges);
void view_toggle_maximize(struct view *view);
void view_frame_click(struct view *view, double fx, double fy, uint32_t time);
void process_interactive_motion(struct server *server);
void end_interactive(struct server *server);
void view_cycle(struct server *server, int direction, bool held);
void view_cycle_end(struct server *server);
void highlight_update(struct server *server);

/* scrollbar.c */
void scrollbars_init(struct server *server);
void scrollbars_view_destroyed(struct view *view);
void view_scroll_to(struct view *view, bool vertical, int position);
void app_menus_init(struct server *server);
void app_menus_view_destroyed(struct view *view);
void view_menu_activate(struct view *view, uint32_t id);

/* tile.c */
bool view_is_tiled(struct view *view);
void tile_arrange(struct server *server);
void tile_set_mode(struct server *server, enum layout_mode mode);
void tile_place(struct view *view, int x, int y, int w, int h);
bool tile_focus_direction(struct server *server, const char *dir);
void tile_toggle_zoom(struct view *view);
void tile_unzoom(struct workspace *ws);
void view_toggle_maximize_or_zoom(struct view *view);

/* scroll.c */
void scroll_add_view(struct view *view);
void scroll_remove_view(struct view *view);
void scroll_build(struct server *server);
void scroll_clear(struct server *server);
void scroll_arrange_workspace(struct workspace *ws, struct wlr_box area);
bool scroll_focus_direction(struct server *server, const char *dir);
bool scroll_move(struct server *server, const char *dir);
bool scroll_consume_or_expel(struct server *server, const char *dir);
bool scroll_column_width(struct server *server, const char *arg);
void scroll_toggle_full(struct view *view);

/* config.c */
void config_load(struct server *server);
void config_finish(struct server *server);
bool bindings_handle(struct server *server, uint32_t mods,
	const xkb_keysym_t *base, int nbase, const xkb_keysym_t *syms, int nsyms);

/* main.c */
void spawn(const char *command);
void cursor_rebase(struct server *server);

/* workspace.c */
void workspaces_init(struct server *server);
int workspace_count(struct server *server);
int workspace_number(struct workspace *ws);
struct workspace *workspace_nth(struct server *server, int n);
struct workspace *workspace_create(struct server *server);
struct workspace *workspace_for_target(struct server *server, int n);
void workspace_switch(struct server *server, struct workspace *ws);
void workspace_prune(struct server *server);
void workspace_focus_top(struct server *server);
void view_set_workspace(struct view *view, struct workspace *ws);

/* ipc.c */
bool ipc_init(struct server *server, const char *wayland_display);
void ipc_finish(struct server *server);
void ipc_notify_workspaces(struct server *server);
void ipc_notify_windows(struct server *server);
void ipc_notify_mode(struct server *server);
void ipc_notify_focus(struct server *server);
int ipc_client_main(int argc, char *argv[]);
void ipc_run_command(struct server *server, const char *command);

/* layer.c */
void layers_init(struct server *server);
void layers_arrange(struct output *output);
void layers_output_destroyed(struct output *output);
struct wlr_surface *layers_exclusive_focus(struct server *server);
bool output_usable_area_at(struct server *server, double lx, double ly,
	struct wlr_box *box);

/* desktop.c */
void desktop_update_output(struct output *output);
bool desktop_parse_color(const char *value, uint32_t *argb, bool *dither);
void desktop_configure(struct server *server);

#endif
