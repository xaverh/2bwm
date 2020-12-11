/* 2bwm, a fast floating WM  with the particularity of having 2 borders written
 * over the XCB library and derived from mcwm written by Michael Cardell.
 * Heavily modified version of http://www.hack.org/mc/hacks/mcwm/
 * Copyright (c) 2010, 2011, 2012 Michael Cardell Widerkrantz, mc at the domain hack.org.
 * Copyright (c) 2014, 2020 Patrick Louis, patrick at the domain psychology dot wtf.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

///---Types---///
struct Monitor {
	xcb_randr_output_t id;
	int16_t x, y;           // X and Y.
	uint16_t width, height; // Width/Height in pixels.
	// Constructor ersetzt addmonitor, das das neue Objekt am Anfang einer linked list erstellt.
	Monitor(xcb_randr_output_t id, const int16_t x, const int16_t y, const uint16_t width,
		const uint16_t height)
		: id{id},
		  x{x},
		  y{y},
		  width{width},
		  height{height} {};

	bool operator==(const Monitor& b) const
	{
		return this->id == b.id;
	}
};

union Arg {
	const char** com;
	const uint32_t i;
};

struct Key {
	unsigned int mod;
	xcb_keysym_t keysym;
	void (*func)(const Arg*);
	const Arg arg;
};

struct Button {
	unsigned int mask, button;
	void (*func)(const Arg*);
	const Arg arg;
	const bool root_only;
};

struct Sizepos {
	int16_t x, y;
	uint16_t width, height;
};

struct Client {                       // Everything we know about a window.
	xcb_drawable_t id;            // ID of this window.
	bool usercoord{false};        // X,Y was set by -geom.
	int16_t x{0}, y{0};           // X/Y coordinate.
	uint16_t width{0}, height{0}; // Width,Height in pixels.
	uint8_t depth;                // pixel depth
	Sizepos origsize;             // Original size if we're currently maxed.
	uint16_t max_width, max_height, min_width{0}, min_height{0}, width_inc{1}, height_inc{1},
		base_width{0}, base_height{0};
	bool fixed{false}, unkillable{false}, vertmaxed{false}, hormaxed{false}, maxed{false},
		verthor{false}, ignore_borders{false}, iconic{false};
	Monitor* monitor{nullptr}; // The physical output this window is on.
	// std::list<Client*>* wsitem; // Pointer to workspace window list.
	size_t ws{SIZE_MAX}; // In which workspace this window belongs to.
	bool operator==(const Client& b) const
	{
		return this->id == b.id;
	}
	Client(xcb_drawable_t id, uint16_t max_width, uint16_t max_height)
		: id{id},
		  max_width{max_width},
		  max_height{max_height}
	{
	}
	Client(xcb_drawable_t id, uint16_t max_width, uint16_t max_height, int16_t x, int16_t y,
	       uint16_t width, uint16_t height, uint16_t min_width, uint16_t min_height,
	       uint16_t width_inc, uint16_t height_inc, uint16_t base_width, uint16_t base_height,
	       bool unkillable, bool fixed, Monitor* monitor, bool ignore_borders)
		: id{id},
		  max_width{max_width},
		  max_height{max_height},
		  x{x},
		  y{y},
		  width{width},
		  height{height},
		  min_width{min_width},
		  min_height{min_height},
		  width_inc{width_inc},
		  height_inc{height_inc},
		  base_width{base_width},
		  base_height{base_height},
		  unkillable{unkillable},
		  fixed{fixed},
		  monitor{monitor},
		  ignore_borders{ignore_borders}
	{
	}
};

struct Winconf { // Window configuration data.
	int16_t x, y;
	uint16_t width, height;
	uint8_t stackmode;
	xcb_window_t sibling;
};

struct Config {
	int8_t borderwidth;  // Do we draw borders for non-focused window? If so, how large?
	int8_t outer_border; // The size of the outer border
	uint32_t focuscol, unfocuscol, fixedcol, unkillcol, empty_col, fixed_unkil_col,
		outer_border_col;
	bool inverted_colors;
	bool enable_compton;
};

///---Internal Constants---///
static constexpr size_t WORKSPACES{10};
#define BUTTONMASK XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
#define NET_WM_FIXED                                                                               \
	0xffffffff // Value in WM hint which means this window is fixed on all workspaces.
#define TWOBWM_NOWS     0xfffffffe // This means we didn't get any window hint at all.
#define LENGTH(x)       (sizeof(x) / sizeof(*x))
#define MIN(X, Y)       ((X) < (Y) ? (X) : (Y))
#define CLEANMASK(mask) (mask & ~(numlockmask | XCB_MOD_MASK_LOCK))
#define CONTROL         XCB_MOD_MASK_CONTROL /* Control key */
#define ALT             XCB_MOD_MASK_1       /* ALT key */
#define SHIFT           XCB_MOD_MASK_SHIFT   /* Shift key */

enum { TWOBWM_MOVE, TWOBWM_RESIZE };
// XXX hier könnte man eine flag setzen, die die Richtung automatisch ändert
static constexpr auto TWOBWM_FOCUS_NEXT{true};
static constexpr auto TWOBWM_FOCUS_PREVIOUS{false};
static constexpr auto TWOBWM_RESIZE_LEFT{0};
static constexpr auto TWOBWM_RESIZE_DOWN{1};
static constexpr auto TWOBWM_RESIZE_UP{2};
static constexpr auto TWOBWM_RESIZE_RIGHT{3};
static constexpr auto TWOBWM_RESIZE_LEFT_SLOW{4};
static constexpr auto TWOBWM_RESIZE_DOWN_SLOW{5};
static constexpr auto TWOBWM_RESIZE_UP_SLOW{6};
static constexpr auto TWOBWM_RESIZE_RIGHT_SLOW{7};
enum { TWOBWM_MOVE_LEFT,
       TWOBWM_MOVE_DOWN,
       TWOBWM_MOVE_UP,
       TWOBWM_MOVE_RIGHT,
       TWOBWM_MOVE_LEFT_SLOW,
       TWOBWM_MOVE_DOWN_SLOW,
       TWOBWM_MOVE_UP_SLOW,
       TWOBWM_MOVE_RIGHT_SLOW };
enum { TWOBWM_TELEPORT_CENTER_X,
       TWOBWM_TELEPORT_TOP_RIGHT,
       TWOBWM_TELEPORT_BOTTOM_RIGHT,
       TWOBWM_TELEPORT_CENTER,
       TWOBWM_TELEPORT_BOTTOM_LEFT,
       TWOBWM_TELEPORT_TOP_LEFT,
       TWOBWM_TELEPORT_CENTER_Y };
enum { BOTTOM_RIGHT, BOTTOM_LEFT, TOP_RIGHT, TOP_LEFT, MIDDLE };
enum { wm_delete_window, wm_change_state, NB_ATOMS };
enum { TWOBWM_RESIZE_KEEP_ASPECT_GROW, TWOBWM_RESIZE_KEEP_ASPECT_SHRINK };
enum { TWOBWM_MAXIMIZE_HORIZONTALLY, TWOBWM_MAXIMIZE_VERTICALLY };
enum { TWOBWM_MAXHALF_FOLD_HORIZONTAL,
       TWOBWM_MAXHALF_UNFOLD_HORIZONTAL,
       TWOBWM_MAXHALF_HORIZONTAL_TOP,
       TWOBWM_MAXHALF_HORIZONTAL_BOTTOM,
       MAXHALF_UNUSED,
       TWOBWM_MAXHALF_VERTICAL_RIGHT,
       TWOBWM_MAXHALF_VERTICAL_LEFT,
       TWOBWM_MAXHALF_UNFOLD_VERTICAL,
       TWOBWM_MAXHALF_FOLD_VERTICAL };
enum { TWOBWM_PREVIOUS_SCREEN, TWOBWM_NEXT_SCREEN };
enum { TWOBWM_CURSOR_UP,
       TWOBWM_CURSOR_DOWN,
       TWOBWM_CURSOR_RIGHT,
       TWOBWM_CURSOR_LEFT,
       TWOBWM_CURSOR_UP_SLOW,
       TWOBWM_CURSOR_DOWN_SLOW,
       TWOBWM_CURSOR_RIGHT_SLOW,
       TWOBWM_CURSOR_LEFT_SLOW };

///---Globals---///
static Config conf;
static xcb_generic_event_t* ev = nullptr;
static void (*events[XCB_NO_OPERATION])(xcb_generic_event_t* e);
static unsigned int numlockmask = 0;
static bool is_sloppy = true;     // by default use sloppy focus
int sigcode = 0;                  // Signal code. Non-zero if we've been interruped by a signal.
xcb_connection_t* conn = nullptr; // Connection to X server.
void ewmh_deleter(xcb_ewmh_connection_t* e)
{
	xcb_ewmh_connection_wipe(e);
	free(e);
}
std::unique_ptr<xcb_ewmh_connection_t, decltype(&ewmh_deleter)> ewmh{
	nullptr, &ewmh_deleter};   // Ewmh Connection.
xcb_screen_t* screen = nullptr;    // Our current screen.
int randrbase = 0;                 // Beginning of RANDR extension events.
static uint8_t curws = 0;          // Current workspace.
Client* focuswin = nullptr;        // Current focus window.
static xcb_drawable_t top_win = 0; // Window always on top.
static std::list<Client> winlist;  // Global list of all client windows.
static std::list<Monitor> monlist; // List of all physical monitor outputs.
static std::array<std::list<Client*>, WORKSPACES> wslists;

///---Global configuration.---///
static const char* atomnames[NB_ATOMS][1] = {{"WM_DELETE_WINDOW"}, {"WM_CHANGE_STATE"}};
xcb_atom_t ATOM[NB_ATOMS];

///---Functions prototypes---///
void run();
auto setup(int) -> bool;
void install_sig_handlers();
void start(const Arg*);
void mousemotion(const Arg*);
void cursor_move(const Arg*);
void changeworkspace(const Arg*);
void changeworkspace_helper(size_t const);
void focusnext(const Arg*);
void focusnext_helper(bool);
void sendtoworkspace(const Arg*);
void sendtonextworkspace(const Arg*);
void sendtoprevworkspace(const Arg*);
void resizestep(const Arg*);
void resizestep_aspect(const Arg*);
void movestep(const Arg*);
void maxvert_hor(const Arg*);
void maxhalf(const Arg*);
void teleport(const Arg*);
void changescreen(const Arg*);
void grabkeys();
void twobwm_restart();
void twobwm_exit();
void centerpointer(xcb_drawable_t, Client const*);
void always_on_top();
auto setup_keyboard() -> bool;
auto setupscreen() -> bool;
auto setuprandr() -> int;
void arrangewindows();
void prevworkspace();
void nextworkspace();
void getrandr();
void raise_current_window();
void raiseorlower();
void setunfocus();
void maximize(const Arg*);
void fullscreen(const Arg*);
void unmaxwin(Client*);
void maxwin(Client*, uint8_t);
void maximize_helper(Client*, uint16_t, uint16_t, uint16_t, uint16_t);
void hide();
void clientmessage(xcb_generic_event_t*);
void deletewin();
void unkillable();
void fix();
void check_name(Client*);
void addtoclientlist(const xcb_drawable_t);
void configurerequest(xcb_generic_event_t*);
void buttonpress(xcb_generic_event_t*);
void unmapnotify(xcb_generic_event_t*);
void mapnotify(xcb_generic_event_t*);
void destroynotify(xcb_generic_event_t*);
void circulaterequest(xcb_generic_event_t*);
void newwin(xcb_generic_event_t*);
void handle_keypress(xcb_generic_event_t*);
auto Create_Font_Cursor(xcb_connection_t*, uint16_t) -> xcb_cursor_t;
auto xcb_get_keycodes(xcb_keysym_t) -> xcb_keycode_t*;
auto xcb_screen_of_display(xcb_connection_t*, int) -> xcb_screen_t*;
auto setupwin(xcb_window_t) -> Client*;
void cleanup();
auto getwmdesktop(xcb_drawable_t) -> uint32_t;
void addtoworkspace(Client*, size_t);
void grabbuttons(Client const*);
void delfromworkspace(Client*);
void unkillablewindow(Client*);
void fixwindow(Client*);
auto getcolor(const char*) -> uint32_t;
void forgetclient(Client*);
void forgetwin(xcb_window_t);
void fitonscreen(Client*);
void getoutputs(xcb_randr_output_t*, const int, xcb_timestamp_t);
auto findmonitor(xcb_randr_output_t) -> Monitor*;
auto findclones(xcb_randr_output_t, const int16_t, const int16_t) -> Monitor*;
auto findmonbycoord(const int16_t, const int16_t) -> Monitor*;
// static void delmonitor(Monitor*);
void raisewindow(xcb_drawable_t);
void movelim(Client*);
void movewindow(xcb_drawable_t, const uint32_t, const uint32_t);
auto findclient(const xcb_drawable_t*);
void setfocus(Client const*);
void resizelim(Client*);
void resize(xcb_drawable_t, const uint16_t, const uint16_t);
void moveresize(xcb_drawable_t, const uint16_t, const uint16_t, const uint16_t, const uint16_t);
void mousemove(const int16_t, const int16_t);
void mouseresize(Client*, const int16_t, const int16_t);
void setborders(Client const*, const bool);
void unmax(Client*);
auto getpointer(const xcb_drawable_t*, int16_t*, int16_t*) -> bool;
auto getgeom(const xcb_drawable_t*, int16_t*, int16_t*, uint16_t*, uint16_t*, uint8_t*) -> bool;
void configwin(xcb_window_t, uint16_t, const struct Winconf*);
void sigcatch(const int);
void ewmh_init();
auto getatom(const char*) -> xcb_atom_t;
void getmonsize(int8_t, int16_t*, int16_t*, uint16_t*, uint16_t*, const Client*);
void noborder(int16_t*, Client*, bool);
void movepointerback(const int16_t, const int16_t, const Client*);
void snapwindow(Client*);
#include "config.hxx"

void fix()
{
	fixwindow(focuswin);
}

void unkillable()
{
	unkillablewindow(focuswin);
}
// void delmonitor(Monitor* mon)
// {
// 	freeitem(&monlist, nullptr, mon->item);
// }

void raise_current_window()
{
	raisewindow(focuswin->id);
}

void focusnext(const Arg* arg)
{
	focusnext_helper(arg->i > 0);
}

void delfromworkspace(Client* client)
{
	if (client->ws == SIZE_MAX) return;
	wslists[client->ws].remove(client);
	client->ws = SIZE_MAX;
}

void changeworkspace(const Arg* arg)
{
	changeworkspace_helper(arg->i);
}

void nextworkspace()
{
	curws == WORKSPACES - 1 ? changeworkspace_helper(0) : changeworkspace_helper(curws + 1);
}

void prevworkspace()
{
	curws > 0 ? changeworkspace_helper(curws - 1) : changeworkspace_helper(WORKSPACES - 1);
}

void twobwm_exit()
{
	exit(EXIT_SUCCESS);
}

void saveorigsize(Client* client)
{
	client->origsize.x = client->x;
	client->origsize.y = client->y;
	client->origsize.width = client->width;
	client->origsize.height = client->height;
}

void centerpointer(xcb_drawable_t win, Client const* cl)
{
	int16_t cur_x, cur_y;

	cur_x = cur_y = 0;

	switch (CURSOR_POSITION) {
	case BOTTOM_RIGHT:
		cur_x += cl->width;
		[[fallthrough]];
	case BOTTOM_LEFT:
		cur_y += cl->height;
		break;
	case TOP_RIGHT:
		cur_x += cl->width;
		[[fallthrough]];
	case TOP_LEFT:
		break;
	default:
		cur_x = cl->width / 2;
		cur_y = cl->height / 2;
	}

	xcb_warp_pointer(conn, XCB_NONE, win, 0, 0, 0, 0, cur_x, cur_y);
}

/* Find client with client->id win in global window list or NULL. */
auto findclient(const xcb_drawable_t* win)
{
	auto cl = std::find_if(winlist.cbegin(), winlist.cend(),
			       [win](Client const& c) { return c.id == *win; });

	return (cl == winlist.cend()) ? nullptr : &*cl;
}

void updateclientlist()
{
	uint32_t len, i;
	xcb_window_t* children;
	Client const* cl;

	/* can only be called after the first window has been spawn */
	xcb_query_tree_reply_t* reply =
		xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), nullptr);
	xcb_delete_property(conn, screen->root, ewmh->_NET_CLIENT_LIST);
	xcb_delete_property(conn, screen->root, ewmh->_NET_CLIENT_LIST_STACKING);

	if (reply == nullptr) {
		addtoclientlist(0);
		return;
	}

	len = xcb_query_tree_children_length(reply);
	children = xcb_query_tree_children(reply);

	for (i = 0; i < len; i++) {
		cl = findclient(&children[i]);
		if (cl != nullptr) addtoclientlist(cl->id);
	}

	free(reply);
}

/* get screen of display */
auto xcb_screen_of_display(xcb_connection_t* con, int screen) -> xcb_screen_t*
{
	xcb_screen_iterator_t iter;
	iter = xcb_setup_roots_iterator(xcb_get_setup(con));
	for (; iter.rem; --screen, xcb_screen_next(&iter))
		if (screen == 0) return iter.data;

	return nullptr;
}

void movepointerback(const int16_t startx, const int16_t starty, const Client* client)
{
	if (startx > (0 - conf.borderwidth - 1) &&
	    startx < (client->width + conf.borderwidth + 1) &&
	    starty > (0 - conf.borderwidth - 1) && starty < (client->height + conf.borderwidth + 1))
		xcb_warp_pointer(conn, XCB_NONE, client->id, 0, 0, 0, 0, startx, starty);
}

/* Set keyboard focus to follow mouse pointer. Then exit. We don't need to
 * bother mapping all windows we know about. They should all be in the X
 * server's Save Set and should be mapped automagically. */
void cleanup()
{
	free(ev);
	monlist.clear();
	for (auto& i : wslists) { i.clear(); }
	winlist.clear();
	ewmh = nullptr;
	if (!conn) { return; }
	xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
	xcb_flush(conn);
	xcb_disconnect(conn);
}

/* Rearrange windows to fit new screen size. */
void arrangewindows()
{
	for (auto& item : winlist) { fitonscreen(&item); }
}

auto getwmdesktop(xcb_drawable_t win) -> uint32_t
{ // Get EWWM hint so we might know what workspace window win should be visible on.
  // Returns either workspace, NET_WM_FIXED if this window should be
  // visible on all workspaces or TWOBWM_NOWS if we didn't find any hints.
	xcb_get_property_cookie_t cookie =
		xcb_get_property(conn, false, win, ewmh->_NET_WM_DESKTOP, XCB_GET_PROPERTY_TYPE_ANY,
				 0, sizeof(uint32_t));
	xcb_get_property_reply_t* reply = xcb_get_property_reply(conn, cookie, nullptr);
	if (nullptr == reply ||
	    0 == xcb_get_property_value_length(reply)) { /* 0 if we didn't find it. */
		if (nullptr != reply) free(reply);
		return TWOBWM_NOWS;
	}
	uint32_t wsp = *(uint32_t*)xcb_get_property_value(reply);
	if (nullptr != reply) free(reply);
	return wsp;
}

/* check if the window is unkillable, if yes return true */
auto get_unkil_state(xcb_drawable_t win) -> bool
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t* reply;
	uint8_t wsp;

	cookie = xcb_get_property(conn, false, win, ewmh->_NET_WM_STATE_DEMANDS_ATTENTION,
				  XCB_GET_PROPERTY_TYPE_ANY, 0, sizeof(uint8_t));

	reply = xcb_get_property_reply(conn, cookie, nullptr);

	if (reply == nullptr || xcb_get_property_value_length(reply) == 0) {
		if (reply != nullptr) free(reply);
		return false;
	}

	wsp = *(uint8_t*)xcb_get_property_value(reply);

	if (reply != nullptr) free(reply);

	if (wsp == 1)
		return true;
	else
		return false;
}

void check_name(Client* client)
{
	xcb_get_property_reply_t* reply;
	unsigned int reply_len;
	unsigned int i;
	uint32_t values[1] = {0};

	if (nullptr == client) return;

	reply = xcb_get_property_reply(conn,
				       xcb_get_property(conn, false, client->id, getatom(LOOK_INTO),
							XCB_GET_PROPERTY_TYPE_ANY, 0, 60),
				       nullptr);

	if (reply == nullptr || xcb_get_property_value_length(reply) == 0) {
		if (nullptr != reply) free(reply);
		return;
	}

	reply_len = xcb_get_property_value_length(reply);
	auto wm_name_window = reinterpret_cast<char*>(malloc(sizeof(char*) * (reply_len + 1)));

	memcpy(wm_name_window, xcb_get_property_value(reply), reply_len);
	wm_name_window[reply_len] = '\0';

	if (nullptr != reply) free(reply);

	for (i = 0; i < sizeof(ignore_names) / sizeof(__typeof__(*ignore_names)); i++)
		if (strstr(wm_name_window, ignore_names[i]) != nullptr) {
			client->ignore_borders = true;
			xcb_configure_window(conn, client->id, XCB_CONFIG_WINDOW_BORDER_WIDTH,
					     values);
			break;
		}

	free(wm_name_window);
}

/* Add a window, specified by client, to workspace ws. */
void addtoworkspace(Client* client, size_t ws)
{
	wslists[ws].push_back(client);

	// Remember our new workspace.
	client->ws = ws;
	/* Set window hint property so we can survive a crash. Like "fixed" */
	if (!client->fixed)
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, ewmh->_NET_WM_DESKTOP,
				    XCB_ATOM_CARDINAL, 32, 1, &ws);
}

void addtoclientlist(const xcb_drawable_t id)
{
	xcb_change_property(conn, XCB_PROP_MODE_APPEND, screen->root, ewmh->_NET_CLIENT_LIST,
			    XCB_ATOM_WINDOW, 32, 1, &id);
	xcb_change_property(conn, XCB_PROP_MODE_APPEND, screen->root,
			    ewmh->_NET_CLIENT_LIST_STACKING, XCB_ATOM_WINDOW, 32, 1, &id);
}

/* Change current workspace to ws */
void changeworkspace_helper(size_t const ws)
{
	if (ws == curws) return;
	xcb_ewmh_set_current_desktop(ewmh.get(), 0, ws);
	/* Go through list of current ws.
	 * Unmap everything that isn't fixed. */
	for (auto client : wslists[curws]) {
		setborders(client, false);
		if (!client->fixed) {
			xcb_unmap_window(conn, client->id);
		} else {
			// correct order is delete first add later.
			delfromworkspace(client);
			addtoworkspace(client, ws);
		}
	}
	for (auto client : wslists[ws]) {
		if (!client->fixed && !client->iconic) xcb_map_window(conn, client->id);
	}
	curws = ws;
	auto pointer =
		xcb_query_pointer_reply(conn, xcb_query_pointer(conn, screen->root), nullptr);
	if (pointer == nullptr)
		setfocus(nullptr);
	else {
		setfocus(findclient(&pointer->child));
		free(pointer);
	}
}

void always_on_top()
{
	Client const* cl = nullptr;

	if (focuswin == nullptr) return;

	if (top_win != focuswin->id) {
		if (0 != top_win) cl = findclient(&top_win);

		top_win = focuswin->id;
		if (nullptr != cl) setborders(cl, false);

		raisewindow(top_win);
	} else
		top_win = 0;

	setborders(focuswin, true);
}

/* Fix or unfix a window client from all workspaces. If setcolour is */
void fixwindow(Client* client)
{
	uint32_t ww;

	if (nullptr == client) return;

	if (client->fixed) {
		client->fixed = false;
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, ewmh->_NET_WM_DESKTOP,
				    XCB_ATOM_CARDINAL, 32, 1, &curws);
	} else {
		/* Raise the window, if going to another desktop don't
		 * let the fixed window behind. */
		raisewindow(client->id);
		client->fixed = true;
		ww = NET_WM_FIXED;
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, ewmh->_NET_WM_DESKTOP,
				    XCB_ATOM_CARDINAL, 32, 1, &ww);
	}

	setborders(client, true);
}

/* Make unkillable or killable a window client. If setcolour is */
void unkillablewindow(Client* client)
{
	if (nullptr == client) return;

	if (client->unkillable) {
		client->unkillable = false;
		xcb_delete_property(conn, client->id, ewmh->_NET_WM_STATE_DEMANDS_ATTENTION);
	} else {
		raisewindow(client->id);
		client->unkillable = true;
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id,
				    ewmh->_NET_WM_STATE_DEMANDS_ATTENTION, XCB_ATOM_CARDINAL, 8, 1,
				    &client->unkillable);
	}

	setborders(client, true);
}

void sendtoworkspace(const Arg* arg)
{
	if (nullptr == focuswin || focuswin->fixed || arg->i == curws) return;
	// correct order is delete first add later.
	delfromworkspace(focuswin);
	addtoworkspace(focuswin, arg->i);
	xcb_unmap_window(conn, focuswin->id);
	xcb_flush(conn);
}

void sendtonextworkspace(const Arg* arg)
{
	Arg arg2 = {.i = 0};
	Arg arg3 = {.i = static_cast<uint32_t>(curws + 1)};
	curws == WORKSPACES - 1 ? sendtoworkspace(&arg2) : sendtoworkspace(&arg3);
}

void sendtoprevworkspace(const Arg* arg)
{
	Arg arg2 = {.i = static_cast<uint32_t>(curws - 1)};
	Arg arg3 = {.i = WORKSPACES - 1};
	curws > 0 ? sendtoworkspace(&arg2) : sendtoworkspace(&arg3);
}

/* Get the pixel values of a named colour colstr. Returns pixel values. */
auto getcolor(const char* hex) -> uint32_t
{
	uint32_t rgb48;
	char strgroups[7] = {hex[1], hex[2], hex[3], hex[4], hex[5], hex[6], '\0'};

	rgb48 = strtol(strgroups, nullptr, 16);
	return rgb48 | 0xff000000;
}

/* Forget everything about client client. */
void forgetclient(Client* client)
{
	if (nullptr == client) return;
	if (client->id == top_win) top_win = 0;
	/* Delete client from the workspace list it belongs to. */
	delfromworkspace(client);

	// Remove from global window list.
	winlist.remove(*client);
}

/* Forget everything about a client with client->id win. */
void forgetwin(xcb_window_t win)
{
	// Find this window in the global window list. Forget it and free allocated data, it might
	// already be freed by handling an UnmapNotify.
	forgetclient(&*std::find_if(winlist.begin(), winlist.end(),
				    [win](Client tofind) { return win == tofind.id; }));
	return;
}

void getmonsize(int8_t with_offsets, int16_t* mon_x, int16_t* mon_y, uint16_t* mon_width,
		uint16_t* mon_height, const Client* client)
{
	if (nullptr == client || nullptr == client->monitor) {
		/* Window isn't attached to any monitor, so we use
		 * the root window size. */
		*mon_x = *mon_y = 0;
		*mon_width = screen->width_in_pixels;
		*mon_height = screen->height_in_pixels;
	} else {
		*mon_x = client->monitor->x;
		*mon_y = client->monitor->y;
		*mon_width = client->monitor->width;
		*mon_height = client->monitor->height;
	}

	if (with_offsets) {
		*mon_x += offsets[0];
		*mon_y += offsets[1];
		*mon_width -= offsets[2];
		*mon_height -= offsets[3];
	}
}

void noborder(int16_t* temp, Client* client, bool set_unset)
{
	if (client->ignore_borders) {
		if (set_unset) {
			*temp = conf.borderwidth;
			conf.borderwidth = 0;
		} else
			conf.borderwidth = *temp;
	}
}
void maximize_helper(Client* client, uint16_t mon_x, uint16_t mon_y, uint16_t mon_width,
		     uint16_t mon_height)
{
	uint32_t values[4];

	values[0] = 0;
	saveorigsize(client);
	xcb_configure_window(conn, client->id, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);

	client->x = mon_x;
	client->y = mon_y;
	client->width = mon_width;
	client->height = mon_height;

	moveresize(client->id, client->x, client->y, client->width, client->height);
	client->maxed = true;
}

/* Fit client on physical screen, moving and resizing as necessary. */
void fitonscreen(Client* client)
{
	int16_t mon_x, mon_y, temp = 0;
	uint16_t mon_width, mon_height;
	bool willmove, willresize;

	willmove = willresize = client->vertmaxed = client->hormaxed = false;

	getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, client);

	if (client->maxed) {
		client->maxed = false;
		setborders(client, false);
	} else {
		/* not maxed but look as if it was maxed, then make it maxed */
		if (client->width == mon_width && client->height == mon_height)
			willresize = true;
		else {
			getmonsize(0, &mon_x, &mon_y, &mon_width, &mon_height, client);
			if (client->width == mon_width && client->height == mon_height)
				willresize = true;
		}
		if (willresize) {
			client->x = mon_x;
			client->y = mon_y;
			client->width -= conf.borderwidth * 2;
			client->height -= conf.borderwidth * 2;
			maximize_helper(client, mon_x, mon_y, mon_width, mon_height);
			return;
		} else {
			getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, client);
		}
	}

	if (client->x > mon_x + mon_width || client->y > mon_y + mon_height || client->x < mon_x ||
	    client->y < mon_y) {
		willmove = true;
		/* Is it outside the physical monitor? */
		if (client->x > mon_x + mon_width)
			client->x = mon_x + mon_width - client->width - conf.borderwidth * 2;
		if (client->y > mon_y + mon_height)
			client->y = mon_y + mon_height - client->height - conf.borderwidth * 2;
		if (client->x < mon_x) client->x = mon_x;
		if (client->y < mon_y) client->y = mon_y;
	}

	/* Is it smaller than it wants to  be? */
	if (0 != client->min_height && client->height < client->min_height) {
		client->height = client->min_height;

		willresize = true;
	}

	if (0 != client->min_width && client->width < client->min_width) {
		client->width = client->min_width;
		willresize = true;
	}

	noborder(&temp, client, true);
	/* If the window is larger than our screen, just place it in
	 * the corner and resize. */
	if (client->width + conf.borderwidth * 2 > mon_width) {
		client->x = mon_x;
		client->width = mon_width - conf.borderwidth * 2;
		;
		willmove = willresize = true;
	} else if (client->x + client->width + conf.borderwidth * 2 > mon_x + mon_width) {
		client->x = mon_x + mon_width - (client->width + conf.borderwidth * 2);
		willmove = true;
	}
	if (client->height + conf.borderwidth * 2 > mon_height) {
		client->y = mon_y;
		client->height = mon_height - conf.borderwidth * 2;
		willmove = willresize = true;
	} else if (client->y + client->height + conf.borderwidth * 2 > mon_y + mon_height) {
		client->y = mon_y + mon_height - (client->height + conf.borderwidth * 2);
		willmove = true;
	}

	if (willmove) movewindow(client->id, client->x, client->y);

	if (willresize) resize(client->id, client->width, client->height);

	noborder(&temp, client, false);
}

/* Set position, geometry and attributes of a new window and show it on
 * the screen.*/
void newwin(xcb_generic_event_t* ev)
{
	auto* e = (xcb_map_request_event_t*)ev;
	long data[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};

	/* The window is trying to map itself on the current workspace,
	 * but since it's unmapped it probably belongs on another workspace.*/
	if (nullptr != findclient(&e->window)) return;

	auto client = setupwin(e->window);

	if (nullptr == client) return;

	/* Add this window to the current workspace. */
	addtoworkspace(client, curws);

	/* If we don't have specific coord map it where the pointer is.*/
	if (!client->usercoord) {
		if (!getpointer(&screen->root, &client->x, &client->y)) client->x = client->y = 0;

		client->x -= client->width / 2;
		client->y -= client->height / 2;
		movewindow(client->id, client->x, client->y);
	}

	/* Find the physical output this window will be on if RANDR is active */
	if (-1 != randrbase) {
		client->monitor = findmonbycoord(client->x, client->y);
		if (nullptr == client->monitor && !monlist.empty())
			/* Window coordinates are outside all physical monitors.
			 * Choose the first screen.*/
			client->monitor = &monlist.front();
	}

	fitonscreen(client);

	/* Show window on screen. */
	xcb_map_window(conn, client->id);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, ewmh->_NET_WM_STATE,
			    ewmh->_NET_WM_STATE, 32, 2, data);

	centerpointer(e->window, client);
	updateclientlist();

	if (!client->maxed) setborders(client, true);
	// always focus new window
	setfocus(client);
}

/* Set border colour, width and event mask for window. */
auto setupwin(xcb_window_t win) -> Client*
{
	unsigned int i;
	uint8_t result;
	uint32_t values[2];
	xcb_atom_t a;
	xcb_size_hints_t hints;
	xcb_ewmh_get_atoms_reply_t win_type;
	xcb_window_t prop;
	xcb_get_property_cookie_t cookie;

	if (xcb_ewmh_get_wm_window_type_reply(ewmh.get(),
					      xcb_ewmh_get_wm_window_type(ewmh.get(), win),
					      &win_type, nullptr) == 1) {
		for (i = 0; i < win_type.atoms_len; i++) {
			a = win_type.atoms[i];
			if (a == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR ||
			    a == ewmh->_NET_WM_WINDOW_TYPE_DOCK ||
			    a == ewmh->_NET_WM_WINDOW_TYPE_DESKTOP) {
				xcb_map_window(conn, win);
				return nullptr;
			}
		}
		xcb_ewmh_get_atoms_reply_wipe(&win_type);
	}
	values[0] = XCB_EVENT_MASK_ENTER_WINDOW;
	xcb_change_window_attributes(conn, win, XCB_CW_BACK_PIXEL, &conf.empty_col);
	xcb_change_window_attributes_checked(conn, win, XCB_CW_EVENT_MASK, values);

	/* Add this window to the X Save Set. */
	xcb_change_save_set(conn, XCB_SET_MODE_INSERT, win);

	/* Remember window and store a few things about it. */
	auto client = &winlist.emplace_front(
		Client{win, screen->width_in_pixels, screen->height_in_pixels});

	/* Get window geometry. */
	getgeom(&client->id, &client->x, &client->y, &client->width, &client->height,
		&client->depth);

	/* Get the window's incremental size step, if any.*/
	xcb_icccm_get_wm_normal_hints_reply(
		conn, xcb_icccm_get_wm_normal_hints_unchecked(conn, win), &hints, nullptr);

	/* The user specified the position coordinates.
	 * Remember that so we can use geometry later. */
	if (hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION) client->usercoord = true;

	if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
		client->min_width = hints.min_width;
		client->min_height = hints.min_height;
	}

	if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
		client->max_width = hints.max_width;
		client->max_height = hints.max_height;
	}

	if (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
		client->width_inc = hints.width_inc;
		client->height_inc = hints.height_inc;
	}

	if (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
		client->base_width = hints.base_width;
		client->base_height = hints.base_height;
	}
	cookie = xcb_icccm_get_wm_transient_for_unchecked(conn, win);
	if (cookie.sequence > 0) {
		result = xcb_icccm_get_wm_transient_for_reply(conn, cookie, &prop, nullptr);
		if (result) {
			Client const* parent = findclient(&prop);
			if (parent) {
				client->usercoord = true;
				client->x =
					parent->x + (parent->width / 2.0) - (client->width / 2.0);
				client->y =
					parent->y + (parent->height / 2.0) - (client->height / 2.0);
			}
		}
	}

	check_name(client);
	return client;
}

/* wrapper to get xcb keycodes from keysymbol */
auto xcb_get_keycodes(xcb_keysym_t keysym) -> xcb_keycode_t*
{
	xcb_key_symbols_t* keysyms;
	xcb_keycode_t* keycode;

	if (!(keysyms = xcb_key_symbols_alloc(conn))) return nullptr;

	keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
	xcb_key_symbols_free(keysyms);

	return keycode;
}

/* the wm should listen to key presses */
void grabkeys()
{
	xcb_keycode_t* keycode;
	int i, k, m;
	unsigned int modifiers[] = {0, XCB_MOD_MASK_LOCK, numlockmask,
				    numlockmask | XCB_MOD_MASK_LOCK};

	xcb_ungrab_key(conn, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);

	for (i = 0; i < LENGTH(keys); i++) {
		keycode = xcb_get_keycodes(keys[i].keysym);

		for (k = 0; keycode[k] != XCB_NO_SYMBOL; k++)
			for (m = 0; m < LENGTH(modifiers); m++)
				xcb_grab_key(conn, 1, screen->root, keys[i].mod | modifiers[m],
					     keycode[k],
					     XCB_GRAB_MODE_ASYNC, // pointer mode
					     XCB_GRAB_MODE_ASYNC  // keyboard mode
				);
		free(keycode); // allocated in xcb_get_keycodes()
	}
}

auto setup_keyboard() -> bool
{
	xcb_get_modifier_mapping_reply_t* reply;
	xcb_keycode_t *modmap, *numlock;
	unsigned int i, j, n;

	reply = xcb_get_modifier_mapping_reply(conn, xcb_get_modifier_mapping_unchecked(conn),
					       nullptr);

	if (!reply) return false;

	modmap = xcb_get_modifier_mapping_keycodes(reply);

	if (!modmap) return false;

	numlock = xcb_get_keycodes(XK_Num_Lock);

	for (i = 4; i < 8; i++) {
		for (j = 0; j < reply->keycodes_per_modifier; j++) {
			xcb_keycode_t keycode = modmap[i * reply->keycodes_per_modifier + j];

			if (keycode == XCB_NO_SYMBOL) continue;

			if (numlock != nullptr)
				for (n = 0; numlock[n] != XCB_NO_SYMBOL; n++)
					if (numlock[n] == keycode) {
						numlockmask = 1 << i;
						break;
					}
		}
	}

	free(reply);
	free(numlock);

	return true;
}

/* Walk through all existing windows and set them up. returns true on success */
auto setupscreen() -> bool
{
	xcb_get_window_attributes_reply_t* attr;
	Client* client;
	uint32_t ws;
	uint32_t len;
	xcb_window_t* children;
	uint32_t i;

	/* Get all children. */
	xcb_query_tree_reply_t* reply =
		xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), nullptr);

	if (nullptr == reply) return false;

	len = xcb_query_tree_children_length(reply);
	children = xcb_query_tree_children(reply);

	/* Set up all windows on this root. */
	for (i = 0; i < len; i++) {
		attr = xcb_get_window_attributes_reply(
			conn, xcb_get_window_attributes(conn, children[i]), nullptr);

		if (!attr) continue;

		/* Don't set up or even bother windows in override redirect mode.
		 * This mode means they wouldn't have been reported to us with a
		 * MapRequest if we had been running, so in the normal case we wouldn't
		 * have seen them. Only handle visible windows. */
		if (!attr->override_redirect && attr->map_state == XCB_MAP_STATE_VIEWABLE) {
			client = setupwin(children[i]);

			if (nullptr != client) {
				/* Find the physical output this window will be on if
				 * RANDR is active. */
				if (-1 != randrbase)
					client->monitor = findmonbycoord(client->x, client->y);
				/* Fit window on physical screen. */
				fitonscreen(client);
				setborders(client, false);

				/* Check if this window has a workspace set already
				 * as a WM hint. */
				ws = getwmdesktop(children[i]);

				if (get_unkil_state(children[i])) unkillablewindow(client);

				if (ws == NET_WM_FIXED) {
					/* Add to current workspace. */
					addtoworkspace(client, curws);
					/* Add to all other workspaces. */
					fixwindow(client);
				} else {
					if (TWOBWM_NOWS != ws && ws < WORKSPACES) {
						addtoworkspace(client, ws);
						if (ws != curws)
							/* If it's not our current works
							 * pace, hide it. */
							xcb_unmap_window(conn, client->id);
					} else {
						addtoworkspace(client, curws);
						addtoclientlist(children[i]);
					}
				}
			}
		}

		if (nullptr != attr) free(attr);
	}
	changeworkspace_helper(0);

	if (nullptr != reply) free(reply);

	return true;
}

/* Set up RANDR extension. Get the extension base and subscribe to events */
auto setuprandr() -> int
{
	int base;
	const xcb_query_extension_reply_t* extension = xcb_get_extension_data(conn, &xcb_randr_id);

	if (!extension->present)
		return -1;
	else
		getrandr();

	base = extension->first_event;
	xcb_randr_select_input(
		conn, screen->root,
		XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
			XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);

	return base;
}

/* Get RANDR resources and figure out how many outputs there are. */
void getrandr()
{
	int len;
	xcb_randr_get_screen_resources_current_cookie_t rcookie =
		xcb_randr_get_screen_resources_current(conn, screen->root);
	xcb_randr_get_screen_resources_current_reply_t* res =
		xcb_randr_get_screen_resources_current_reply(conn, rcookie, nullptr);

	if (nullptr == res) return;

	xcb_timestamp_t timestamp = res->config_timestamp;
	len = xcb_randr_get_screen_resources_current_outputs_length(res);
	xcb_randr_output_t* outputs = xcb_randr_get_screen_resources_current_outputs(res);

	/* Request information for all outputs. */
	getoutputs(outputs, len, timestamp);
	free(res);
}

/* Walk through all the RANDR outputs (number of outputs == len) there */
void getoutputs(xcb_randr_output_t* outputs, const int len, xcb_timestamp_t timestamp)
{
	/* was at time timestamp. */
	xcb_randr_get_crtc_info_cookie_t icookie;
	xcb_randr_get_crtc_info_reply_t* crtc = nullptr;
	// xcb_randr_get_output_info_reply_t* output;
	Monitor* clonemon;
	xcb_randr_get_output_info_cookie_t ocookie[len];

	for (auto i = 0; i < len; i++)
		ocookie[i] = xcb_randr_get_output_info(conn, outputs[i], timestamp);

	/* Loop through all outputs. */
	for (auto i = 0; i < len; i++) {
		std::unique_ptr<xcb_randr_get_output_info_reply_t, decltype(&std::free)> output{
			xcb_randr_get_output_info_reply(conn, ocookie[i], nullptr), std::free};
		if (output == nullptr) continue;

		if (XCB_NONE != output->crtc) {
			icookie = xcb_randr_get_crtc_info(conn, output->crtc, timestamp);
			crtc = xcb_randr_get_crtc_info_reply(conn, icookie, nullptr);

			if (nullptr == crtc) return;

			/* Check if it's a clone. */
			// TODO maybe they are not cloned, one might be bigger
			// than the other after closing the lid
			clonemon = findclones(outputs[i], crtc->x, crtc->y);

			if (nullptr != clonemon) continue;

			/* Do we know this monitor already? */
			if (auto mon = findmonitor(outputs[i]); mon == nullptr)
				monlist.emplace_front(outputs[i], crtc->x, crtc->y, crtc->width,
						      crtc->height);
			else
				/* We know this monitor. Update information.
				 * If it's smaller than before, rearrange windows. */
				if (crtc->x != mon->x || crtc->y != mon->y ||
				    crtc->width != mon->width || crtc->height != mon->height) {
				if (crtc->x != mon->x) mon->x = crtc->x;
				if (crtc->y != mon->y) mon->y = crtc->y;
				if (crtc->width != mon->width) mon->width = crtc->width;
				if (crtc->height != mon->height) mon->height = crtc->height;

				// TODO when lid closed, one screen
				for (auto& item : winlist) {
					if (item.monitor == mon) fitonscreen(&item);
				}
			}
			free(crtc);
		} else {
			/* Check if it was used before. If it was, do something. */
			auto id = outputs[i];
			if (auto mon =
				    std::find_if(monlist.begin(), monlist.end(),
						 [id](Monitor const& mon) { return mon.id == id; });
			    mon != monlist.end()) {
				for (auto& client : winlist) {
					// Check all windows on this monitor and move them to the
					// next or to the first monitor if there is no next.
					if (client.monitor == &*mon) {
						if (mon == std::prev(monlist.end())) {
							client.monitor = &monlist.front();
						} else {
							client.monitor = &*std::next(mon);
						};
						fitonscreen(&client);
					}
				}
			}
		}
	}
}

// XXX die folgenden drei Funktionen können zusammengefaßt werden
auto findmonitor(xcb_randr_output_t id) -> Monitor*
{
	if (auto found = std::find_if(std::begin(monlist), std::end(monlist),
				      [id](const Monitor& mon) { return mon.id == id; });
	    found != std::end(monlist))
		return &*found;
	return nullptr;
}

// XXX vielleicht kann man hier stattdessen operator== bzw. operator<=> überladen
auto findclones(xcb_randr_output_t id, const int16_t x, const int16_t y) -> Monitor*
{
	if (auto found = std::find_if(std::begin(monlist), std::end(monlist),
				      [id, x, y](const Monitor& mon) {
					      return mon.id == id && mon.x == x && mon.y == y;
				      });
	    found != std::end(monlist))
		return &*found;
	return nullptr;
}

auto findmonbycoord(const int16_t x, const int16_t y) -> Monitor*
{
	if (auto found =
		    std::find_if(std::begin(monlist), std::end(monlist),
				 [x, y](const Monitor& mon) { return mon.x == x && mon.y == y; });
	    found != std::end(monlist))
		return &*found;
	return nullptr;
}

/* Raise window win to top of stack. */
void raisewindow(xcb_drawable_t win)
{
	uint32_t values[] = {XCB_STACK_MODE_ABOVE};

	if (screen->root == win || 0 == win) return;

	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, values);
	xcb_flush(conn);
}

/* Set window client to either top or bottom of stack depending on
 * where it is now. */
void raiseorlower()
{
	uint32_t values[] = {XCB_STACK_MODE_OPPOSITE};

	if (nullptr == focuswin) return;

	xcb_configure_window(conn, focuswin->id, XCB_CONFIG_WINDOW_STACK_MODE, values);

	xcb_flush(conn);
}

/* Keep the window inside the screen */
void movelim(Client* client)
{
	int16_t mon_y, mon_x, temp = 0;
	uint16_t mon_height, mon_width;

	getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, client);
	noborder(&temp, client, true);

	/* Is it outside the physical monitor or close to the side? */
	if (client->y - conf.borderwidth < mon_y || client->y < borders[2] + mon_y)
		client->y = mon_y;
	else if (client->y + client->height + (conf.borderwidth * 2) >
		 mon_y + mon_height - borders[2])
		client->y = mon_y + mon_height - client->height - conf.borderwidth * 2;

	if (client->x < borders[2] + mon_x)
		client->x = mon_x;
	else if (client->x + client->width + (conf.borderwidth * 2) >
		 mon_x + mon_width - borders[2])
		client->x = mon_x + mon_width - client->width - conf.borderwidth * 2;

	movewindow(client->id, client->x, client->y);
	noborder(&temp, client, false);
}

void movewindow(xcb_drawable_t win, const uint32_t x, const uint32_t y)
{ // Move window win to root coordinates x,y.
	uint32_t values[2] = {x, y};

	if (screen->root == win || 0 == win) return;

	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);

	xcb_flush(conn);
}

void focusnext_helper(bool arg)
{
	Client* cl = nullptr;
	if (wslists[curws].empty()) return;
	// if no focus on current workspace, find first valid item on list.
	if (nullptr == focuswin || focuswin->ws != curws) {
		cl = *std::find_if_not(wslists[curws].cbegin(), wslists[curws].cend(),
				       [](auto const& c) { return c->iconic; });
	} else {
		if (arg) {
			// start from focus next and find first valid item on circular list.
			// do {
			cl = *std::rotate(wslists[curws].begin(), std::next(wslists[curws].begin()),
					  wslists[curws].end());
			// } while (!cl->iconic);
		} else {
			// start from focus previous and find first valid on circular list.
			// do {
			cl = *std::rotate(wslists[curws].rbegin(),
					  std::next(wslists[curws].rbegin()),
					  wslists[curws].rend());
			//} while (!cl->iconic);
		}
	}
	raisewindow(cl->id);
	centerpointer(cl->id, cl);
	setfocus(cl);
}
/* Mark window win as unfocused. */
void setunfocus()
{
	//    xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_NONE,XCB_CURRENT_TIME);
	if (nullptr == focuswin || focuswin->id == screen->root) return;

	setborders(focuswin, false);
}

void setfocus(Client const* client) // Set focus on window client.
{
	long data[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};

	/* If client is NULL, we focus on whatever the pointer is on.
	 * This is a pathological case, but it will make the poor user able
	 * to focus on windows anyway, even though this windowmanager might
	 * be buggy. */
	if (nullptr == client) {
		focuswin = nullptr;
		xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
		xcb_window_t not_win = 0;
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
				    ewmh->_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 32, 1, &not_win);

		xcb_flush(conn);
		return;
	}

	/* Don't bother focusing on the root window or on the same window
	 * that already has focus. */
	if (client->id == screen->root) return;

	if (nullptr != focuswin) setunfocus(); /* Unset last focus. */

	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, ewmh->_NET_WM_STATE,
			    ewmh->_NET_WM_STATE, 32, 2, data);
	xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, client->id,
			    XCB_CURRENT_TIME); /* Set new input focus. */
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, ewmh->_NET_ACTIVE_WINDOW,
			    XCB_ATOM_WINDOW, 32, 1, &client->id);

	/* Remember the new window as the current focused window. */
	focuswin = const_cast<Client*>(client);

	grabbuttons(client);
	setborders(client, true);
}

void start(const Arg* arg)
{
	if (fork()) return;

	//	if (conn)
	//		close(screen->root);

	setsid();
	execvp((char*)arg->com[0], (char**)arg->com);
}

/* Resize with limit. */
void resizelim(Client* client)
{
	int16_t mon_x, mon_y, temp = 0;
	uint16_t mon_width, mon_height;

	getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, client);
	noborder(&temp, client, true);

	/* Is it smaller than it wants to  be? */
	if (0 != client->min_height && client->height < client->min_height)
		client->height = client->min_height;
	if (0 != client->min_width && client->width < client->min_width)
		client->width = client->min_width;
	if (client->x + client->width + conf.borderwidth > mon_x + mon_width)
		client->width = mon_width - ((client->x - mon_x) + conf.borderwidth * 2);
	if (client->y + client->height + conf.borderwidth > mon_y + mon_height)
		client->height = mon_height - ((client->y - mon_y) + conf.borderwidth * 2);

	resize(client->id, client->width, client->height);
	noborder(&temp, client, false);
}

void moveresize(xcb_drawable_t win, const uint16_t x, const uint16_t y, const uint16_t width,
		const uint16_t height)
{
	uint32_t values[4] = {x, y, width, height};

	if (screen->root == win || 0 == win) return;

	xcb_configure_window(conn, win,
			     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
				     XCB_CONFIG_WINDOW_HEIGHT,
			     values);

	xcb_flush(conn);
}

/* Resize window win to width,height. */
void resize(xcb_drawable_t win, const uint16_t width, const uint16_t height)
{
	uint32_t values[2] = {width, height};

	if (screen->root == win || 0 == win) return;

	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);

	xcb_flush(conn);
}

/* Resize window client in direction. */
void resizestep(const Arg* arg)
{
	uint8_t stepx, stepy, cases = arg->i % 4;

	if (nullptr == focuswin || focuswin->maxed) return;

	arg->i < 4 ? (stepx = stepy = movements[1]) : (stepx = stepy = movements[0]);

	if (focuswin->width_inc > 7 && focuswin->height_inc > 7) {
		/* we were given a resize hint by the window so use it */
		stepx = focuswin->width_inc;
		stepy = focuswin->height_inc;
	}

	if (cases == TWOBWM_RESIZE_LEFT)
		focuswin->width = focuswin->width - stepx;
	else if (cases == TWOBWM_RESIZE_DOWN)
		focuswin->height = focuswin->height + stepy;
	else if (cases == TWOBWM_RESIZE_UP)
		focuswin->height = focuswin->height - stepy;
	else if (cases == TWOBWM_RESIZE_RIGHT)
		focuswin->width = focuswin->width + stepx;

	if (focuswin->vertmaxed) focuswin->vertmaxed = false;
	if (focuswin->hormaxed) focuswin->hormaxed = false;

	resizelim(focuswin);
	centerpointer(focuswin->id, focuswin);
	raise_current_window();
	setborders(focuswin, true);
}

/* Resize window and keep it's aspect ratio (exponentially grow),
 * and round the result (+0.5) */
void resizestep_aspect(const Arg* arg)
{
	if (nullptr == focuswin || focuswin->maxed) return;

	if (arg->i == TWOBWM_RESIZE_KEEP_ASPECT_SHRINK) {
		focuswin->width = (focuswin->width / resize_keep_aspect_ratio) + 0.5;
		focuswin->height = (focuswin->height / resize_keep_aspect_ratio) + 0.5;
	} else {
		focuswin->height = (focuswin->height * resize_keep_aspect_ratio) + 0.5;
		focuswin->width = (focuswin->width * resize_keep_aspect_ratio) + 0.5;
	}

	if (focuswin->vertmaxed) focuswin->vertmaxed = false;
	if (focuswin->hormaxed) focuswin->hormaxed = false;

	resizelim(focuswin);
	centerpointer(focuswin->id, focuswin);
	raise_current_window();
	setborders(focuswin, true);
}

/* Try to snap to other windows and monitor border */
void snapwindow(Client* client)
{
	int16_t mon_x, mon_y;
	uint16_t mon_width, mon_height;

	getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, focuswin);

	for (auto const& win : wslists[curws]) {
		if (client != win) {
			if (abs((win->x + win->width) - client->x + conf.borderwidth) < borders[2])
				if (client->y + client->height > win->y &&
				    client->y < win->y + win->height)
					client->x = (win->x + win->width) + (2 * conf.borderwidth);

			if (abs((win->y + win->height) - client->y + conf.borderwidth) < borders[2])
				if (client->x + client->width > win->x &&
				    client->x < win->x + win->width)
					client->y = (win->y + win->height) + (2 * conf.borderwidth);

			if (abs((client->x + client->width) - win->x + conf.borderwidth) <
			    borders[2])
				if (client->y + client->height > win->y &&
				    client->y < win->y + win->height)
					client->x =
						(win->x - client->width) - (2 * conf.borderwidth);

			if (abs((client->y + client->height) - win->y + conf.borderwidth) <
			    borders[2])
				if (client->x + client->width > win->x &&
				    client->x < win->x + win->width)
					client->y =
						(win->y - client->height) - (2 * conf.borderwidth);
		}
	}
}

/* Move window win as a result of pointer motion to coordinates rel_x,rel_y. */
void mousemove(const int16_t rel_x, const int16_t rel_y)
{
	if (focuswin == nullptr || focuswin->ws != curws) return;

	focuswin->x = rel_x;
	focuswin->y = rel_y;

	if (borders[2] > 0) snapwindow(focuswin);

	movelim(focuswin);
}

void mouseresize(Client* client, const int16_t rel_x, const int16_t rel_y)
{
	if (focuswin->id == screen->root || focuswin->maxed) return;

	client->width = abs(rel_x);
	client->height = abs(rel_y);

	if (resize_by_line) {
		client->width -= (client->width - client->base_width) % client->width_inc;
		client->height -= (client->height - client->base_height) % client->height_inc;
	}

	resizelim(client);
	client->vertmaxed = false;
	client->hormaxed = false;
}

void movestep(const Arg* arg)
{
	int16_t start_x, start_y;
	uint8_t step, cases = arg->i;

	if (nullptr == focuswin || focuswin->maxed) return;

	/* Save pointer position so we can warp pointer here later. */
	if (!getpointer(&focuswin->id, &start_x, &start_y)) return;

	cases = cases % 4;
	arg->i < 4 ? (step = movements[1]) : (step = movements[0]);

	if (cases == TWOBWM_MOVE_LEFT)
		focuswin->x = focuswin->x - step;
	else if (cases == TWOBWM_MOVE_DOWN)
		focuswin->y = focuswin->y + step;
	else if (cases == TWOBWM_MOVE_UP)
		focuswin->y = focuswin->y - step;
	else if (cases == TWOBWM_MOVE_RIGHT)
		focuswin->x = focuswin->x + step;

	raise_current_window();
	movelim(focuswin);
	movepointerback(start_x, start_y, focuswin);
	xcb_flush(conn);
}

void setborders(Client const* client, const bool isitfocused)
{
	uint32_t values[1]; /* this is the color maintainer */
	uint16_t half = 0;
	bool inv = conf.inverted_colors;

	if (client->maxed || client->ignore_borders) return;

	/* Set border width. */
	values[0] = conf.borderwidth;
	xcb_configure_window(conn, client->id, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);

	if (top_win != 0 && client->id == top_win) inv = !inv;

	half = conf.outer_border;

	xcb_rectangle_t rect_inner[] = {
		{static_cast<int16_t>(client->width), 0,
		 static_cast<uint16_t>(conf.borderwidth - half),
		 static_cast<uint16_t>(client->height + conf.borderwidth - half)},
		{static_cast<int16_t>(client->width + conf.borderwidth + half), 0,
		 static_cast<uint16_t>(conf.borderwidth - half),
		 static_cast<uint16_t>(client->height + conf.borderwidth - half)},
		{0, static_cast<int16_t>(client->height),
		 static_cast<uint16_t>(client->width + conf.borderwidth - half),
		 static_cast<uint16_t>(conf.borderwidth - half)},
		{0, static_cast<int16_t>(client->height + conf.borderwidth + half),
		 static_cast<uint16_t>(client->width + conf.borderwidth - half),
		 static_cast<uint16_t>(conf.borderwidth - half)},
		{static_cast<int16_t>(client->width + conf.borderwidth + half),
		 static_cast<int16_t>(conf.borderwidth + client->height + half),
		 static_cast<uint16_t>(conf.borderwidth), static_cast<uint16_t>(conf.borderwidth)}};

	xcb_rectangle_t rect_outer[] = {
		{static_cast<int16_t>(client->width + conf.borderwidth - half), 0, half,
		 static_cast<uint16_t>(client->height + conf.borderwidth * 2)},
		{static_cast<int16_t>(client->width + conf.borderwidth), 0, half,
		 static_cast<uint16_t>(client->height + conf.borderwidth * 2)},
		{0, static_cast<int16_t>(client->height + conf.borderwidth - half),
		 static_cast<uint16_t>(client->width + conf.borderwidth * 2), half},
		{0, static_cast<int16_t>(client->height + conf.borderwidth),
		 static_cast<uint16_t>(client->width + conf.borderwidth * 2), half},
		{1, 1, 1, 1}};

	xcb_pixmap_t pmap = xcb_generate_id(conn);
	xcb_create_pixmap(conn, client->depth, pmap, client->id,
			  client->width + (conf.borderwidth * 2),
			  client->height + (conf.borderwidth * 2));

	xcb_gcontext_t gc = xcb_generate_id(conn);
	xcb_create_gc(conn, gc, pmap, 0, nullptr);

	if (inv) {
		xcb_rectangle_t fake_rect[5];

		for (uint8_t i = 0; i < 5; i++) {
			fake_rect[i] = rect_outer[i];
			rect_outer[i] = rect_inner[i];
			rect_inner[i] = fake_rect[i];
		}
	}

	values[0] = conf.outer_border_col;

	if (client->unkillable || client->fixed) {
		if (client->unkillable && client->fixed)
			values[0] = conf.fixed_unkil_col;
		else if (client->fixed)
			values[0] = conf.fixedcol;
		else
			values[0] = conf.unkillcol;
	}

	xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &values[0]);
	xcb_poly_fill_rectangle(conn, pmap, gc, 5, rect_outer);

	values[0] = conf.focuscol;

	if (!isitfocused) values[0] = conf.unfocuscol;

	xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &values[0]);
	xcb_poly_fill_rectangle(conn, pmap, gc, 5, rect_inner);
	values[0] = pmap;
	xcb_change_window_attributes(conn, client->id, XCB_CW_BORDER_PIXMAP, &values[0]);

	/* free the memory we allocated for the pixmap */
	xcb_free_pixmap(conn, pmap);
	xcb_free_gc(conn, gc);
	xcb_flush(conn);
}

void unmax(Client* client)
{
	if (nullptr == client) return;

	client->x = client->origsize.x;
	client->y = client->origsize.y;
	client->width = client->origsize.width;
	client->height = client->origsize.height;

	client->maxed = client->hormaxed = false;
	moveresize(client->id, client->x, client->y, client->width, client->height);

	centerpointer(client->id, client);
	setborders(client, true);
}

void maximize(const Arg* arg)
{
	maxwin(focuswin, 1);
}

void fullscreen(const Arg* arg)
{
	maxwin(focuswin, 0);
}

void unmaxwin(Client* client)
{
	unmax(client);
	client->maxed = false;
	setborders(client, true);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, ewmh->_NET_WM_STATE,
			    XCB_ATOM_ATOM, 32, 0, nullptr);
}

void maxwin(Client* client, uint8_t with_offsets)
{
	int16_t mon_x, mon_y;
	uint16_t mon_width, mon_height;

	if (nullptr == focuswin) return;

	/* Check if maximized already. If so, revert to stored geometry. */
	if (focuswin->maxed) {
		unmaxwin(focuswin);
		return;
	}

	getmonsize(with_offsets, &mon_x, &mon_y, &mon_width, &mon_height, client);
	maximize_helper(client, mon_x, mon_y, mon_width, mon_height);
	raise_current_window();
	if (!with_offsets) {
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, ewmh->_NET_WM_STATE,
				    XCB_ATOM_ATOM, 32, 1, &ewmh->_NET_WM_STATE_FULLSCREEN);
	}
	xcb_flush(conn);
}

void maxvert_hor(const Arg* arg)
{
	uint32_t values[2];
	int16_t mon_y, mon_x, temp = 0;
	uint16_t mon_height, mon_width;

	if (nullptr == focuswin) return;

	if (focuswin->vertmaxed || focuswin->hormaxed) {
		unmax(focuswin);
		focuswin->vertmaxed = focuswin->hormaxed = false;
		fitonscreen(focuswin);
		setborders(focuswin, true);
		return;
	}

	getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, focuswin);
	saveorigsize(focuswin);
	noborder(&temp, focuswin, true);

	if (arg->i == TWOBWM_MAXIMIZE_VERTICALLY) {
		focuswin->y = mon_y;
		/* Compute new height considering height increments
		 * and screen height. */
		focuswin->height = mon_height - (conf.borderwidth * 2);

		/* Move to top of screen and resize. */
		values[0] = focuswin->y;
		values[1] = focuswin->height;

		xcb_configure_window(conn, focuswin->id,
				     XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT, values);

		focuswin->vertmaxed = true;
	} else if (arg->i == TWOBWM_MAXIMIZE_HORIZONTALLY) {
		focuswin->x = mon_x;
		focuswin->width = mon_width - (conf.borderwidth * 2);
		values[0] = focuswin->x;
		values[1] = focuswin->width;

		xcb_configure_window(conn, focuswin->id,
				     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_WIDTH, values);

		focuswin->hormaxed = true;
	}

	noborder(&temp, focuswin, false);
	raise_current_window();
	centerpointer(focuswin->id, focuswin);
	setborders(focuswin, true);
}

void maxhalf(const Arg* arg)
{
	uint32_t values[4];
	int16_t mon_x, mon_y, temp = 0;
	uint16_t mon_width, mon_height;

	if (nullptr == focuswin || focuswin->maxed) return;

	getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, focuswin);
	noborder(&temp, focuswin, true);

	if (arg->i > 4) {
		if (arg->i > 6) {
			/* in folding mode */
			if (arg->i == TWOBWM_MAXHALF_FOLD_VERTICAL)
				focuswin->height = focuswin->height / 2 - (conf.borderwidth);
			else
				focuswin->height = focuswin->height * 2 + (2 * conf.borderwidth);
		} else {
			focuswin->y = mon_y;
			focuswin->height = mon_height - (conf.borderwidth * 2);
			focuswin->width = ((float)(mon_width) / 2) - (conf.borderwidth * 2);

			if (arg->i == TWOBWM_MAXHALF_VERTICAL_LEFT)
				focuswin->x = mon_x;
			else
				focuswin->x = mon_x + mon_width -
					      (focuswin->width + conf.borderwidth * 2);
		}
	} else {
		if (arg->i < 2) {
			/* in folding mode */
			if (arg->i == TWOBWM_MAXHALF_FOLD_HORIZONTAL)
				focuswin->width = focuswin->width / 2 - conf.borderwidth;
			else
				focuswin->width =
					focuswin->width * 2 + (2 * conf.borderwidth); // unfold
		} else {
			focuswin->x = mon_x;
			focuswin->width = mon_width - (conf.borderwidth * 2);
			focuswin->height = ((float)(mon_height) / 2) - (conf.borderwidth * 2);

			if (arg->i == TWOBWM_MAXHALF_HORIZONTAL_TOP)
				focuswin->y = mon_y;
			else
				focuswin->y = mon_y + mon_height -
					      (focuswin->height + conf.borderwidth * 2);
		}
	}

	moveresize(focuswin->id, focuswin->x, focuswin->y, focuswin->width, focuswin->height);

	focuswin->verthor = true;
	noborder(&temp, focuswin, false);
	raise_current_window();
	fitonscreen(focuswin);
	centerpointer(focuswin->id, focuswin);
	setborders(focuswin, true);
}

void hide()
{
	if (focuswin == nullptr) return;

	long data[] = {XCB_ICCCM_WM_STATE_ICONIC, ewmh->_NET_WM_STATE_HIDDEN, XCB_NONE};

	/* Unmap window and declare iconic. Unmapping will generate an
	 * UnmapNotify event so we can forget about the window later. */
	focuswin->iconic = true;

	xcb_unmap_window(conn, focuswin->id);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, focuswin->id, ewmh->_NET_WM_STATE,
			    ewmh->_NET_WM_STATE, 32, 3, data);

	xcb_flush(conn);
}

auto getpointer(const xcb_drawable_t* win, int16_t* x, int16_t* y) -> bool
{
	xcb_query_pointer_reply_t* pointer;

	pointer = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, *win), nullptr);
	if (nullptr == pointer) return false;
	*x = pointer->win_x;
	*y = pointer->win_y;

	free(pointer);
	return true;
}

auto getgeom(const xcb_drawable_t* win, int16_t* x, int16_t* y, uint16_t* width, uint16_t* height,
	     uint8_t* depth) -> bool
{
	xcb_get_geometry_reply_t* geom =
		xcb_get_geometry_reply(conn, xcb_get_geometry(conn, *win), nullptr);

	if (nullptr == geom) return false;

	*x = geom->x;
	*y = geom->y;
	*width = geom->width;
	*height = geom->height;
	*depth = geom->depth;

	free(geom);
	return true;
}

void teleport(const Arg* arg)
{
	int16_t pointx, pointy, mon_x, mon_y, temp = 0;
	uint16_t mon_width, mon_height;

	if (nullptr == focuswin || wslists[curws].empty() || focuswin->maxed) return;

	if (!getpointer(&focuswin->id, &pointx, &pointy)) return;
	uint16_t tmp_x = focuswin->x;
	uint16_t tmp_y = focuswin->y;

	getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, focuswin);
	noborder(&temp, focuswin, true);
	focuswin->x = mon_x;
	focuswin->y = mon_y;

	if (arg->i == TWOBWM_TELEPORT_CENTER) { /* center */
		focuswin->x += mon_width - (focuswin->width + conf.borderwidth * 2) + mon_x;
		focuswin->y += mon_height - (focuswin->height + conf.borderwidth * 2) + mon_y;
		focuswin->y = focuswin->y / 2;
		focuswin->x = focuswin->x / 2;
	} else {
		/* top-left */
		if (arg->i > 3) {
			/* bottom-left */
			if (arg->i == TWOBWM_TELEPORT_BOTTOM_LEFT)
				focuswin->y +=
					mon_height - (focuswin->height + conf.borderwidth * 2);
			/* center y */
			else if (arg->i == TWOBWM_TELEPORT_CENTER_Y) {
				focuswin->x = tmp_x;
				focuswin->y += mon_height -
					       (focuswin->height + conf.borderwidth * 2) + mon_y;
				focuswin->y = focuswin->y / 2;
			}
		} else {
			/* top-right */
			if (arg->i < 2) /* center x */
				if (arg->i == TWOBWM_TELEPORT_CENTER_X) {
					focuswin->y = tmp_y;
					focuswin->x += mon_width -
						       (focuswin->width + conf.borderwidth * 2) +
						       mon_x;
					focuswin->x = focuswin->x / 2;
				} else
					focuswin->x += mon_width -
						       (focuswin->width + conf.borderwidth * 2);
			else {
				/* bottom-right */
				focuswin->x += mon_width - (focuswin->width + conf.borderwidth * 2);
				focuswin->y +=
					mon_height - (focuswin->height + conf.borderwidth * 2);
			}
		}
	}

	movewindow(focuswin->id, focuswin->x, focuswin->y);
	movepointerback(pointx, pointy, focuswin);
	noborder(&temp, focuswin, false);
	raise_current_window();
	xcb_flush(conn);
}

void deletewin()
{
	bool use_delete = false;
	xcb_icccm_get_wm_protocols_reply_t protocols;
	xcb_get_property_cookie_t cookie;

	if (nullptr == focuswin || focuswin->unkillable == true) return;

	/* Check if WM_DELETE is supported.  */
	cookie = xcb_icccm_get_wm_protocols_unchecked(conn, focuswin->id, ewmh->WM_PROTOCOLS);

	if (focuswin->id == top_win) top_win = 0;

	if (xcb_icccm_get_wm_protocols_reply(conn, cookie, &protocols, nullptr) == 1) {
		for (uint32_t i = 0; i < protocols.atoms_len; i++)
			if (protocols.atoms[i] == ATOM[wm_delete_window]) {
				xcb_client_message_event_t ev = {
					.response_type = XCB_CLIENT_MESSAGE,
					.format = 32,
					.sequence = 0,
					.window = focuswin->id,
					.type = ewmh->WM_PROTOCOLS,
					.data = {.data32 = {ATOM[wm_delete_window],
							    XCB_CURRENT_TIME}}};

				xcb_send_event(conn, false, focuswin->id, XCB_EVENT_MASK_NO_EVENT,
					       (char*)&ev);

				use_delete = true;
				break;
			}

		xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
	}
	if (!use_delete) xcb_kill_client(conn, focuswin->id);
}

void changescreen(const Arg* arg)
{
	if (nullptr == focuswin || nullptr == focuswin->monitor) return;

	Monitor const* new_monitor = nullptr;
	if (arg->i == TWOBWM_NEXT_SCREEN) {
		auto monit =
			std::find(std::cbegin(monlist), std::cend(monlist), *focuswin->monitor);
		if (monit == monlist.end())
			new_monitor = &monlist.front();
		else
			new_monitor = &*std::next(std::find(
				std::cbegin(monlist), std::cend(monlist), *focuswin->monitor));
	} else {
		auto monit =
			std::find(std::cbegin(monlist), std::cend(monlist), *focuswin->monitor);
		if (monit == monlist.begin())
			new_monitor = &monlist.back();
		else
			new_monitor = &*std::prev(std::find(
				std::cbegin(monlist), std::cend(monlist), *focuswin->monitor));
	}

	auto xpercentage =
		(float)((focuswin->x - focuswin->monitor->x) / (focuswin->monitor->width));
	auto ypercentage =
		(float)((focuswin->y - focuswin->monitor->y) / (focuswin->monitor->height));

	focuswin->monitor = const_cast<Monitor*>(new_monitor);

	focuswin->x = focuswin->monitor->width * xpercentage + focuswin->monitor->x + 0.5;
	focuswin->y = focuswin->monitor->height * ypercentage + focuswin->monitor->y + 0.5;

	raise_current_window();
	fitonscreen(focuswin);
	movelim(focuswin);
	setborders(focuswin, true);
	centerpointer(focuswin->id, focuswin);
}

/* Function to make the cursor move with the keyboard */
void cursor_move(const Arg* arg)
{
	uint16_t speed;
	uint8_t cases = arg->i % 4;
	arg->i < 4 ? (speed = movements[3]) : (speed = movements[2]);

	if (cases == TWOBWM_CURSOR_UP)
		xcb_warp_pointer(conn, XCB_NONE, XCB_NONE, 0, 0, 0, 0, 0, -speed);
	else if (cases == TWOBWM_CURSOR_DOWN)
		xcb_warp_pointer(conn, XCB_NONE, XCB_NONE, 0, 0, 0, 0, 0, speed);
	else if (cases == TWOBWM_CURSOR_RIGHT)
		xcb_warp_pointer(conn, XCB_NONE, XCB_NONE, 0, 0, 0, 0, speed, 0);
	else if (cases == TWOBWM_CURSOR_LEFT)
		xcb_warp_pointer(conn, XCB_NONE, XCB_NONE, 0, 0, 0, 0, -speed, 0);

	xcb_flush(conn);
}

/* wrapper to get xcb keysymbol from keycode */
static auto xcb_get_keysym(xcb_keycode_t keycode) -> xcb_keysym_t
{
	xcb_key_symbols_t* keysyms;

	if (!(keysyms = xcb_key_symbols_alloc(conn))) return 0;

	xcb_keysym_t keysym = xcb_key_symbols_get_keysym(keysyms, keycode, 0);
	xcb_key_symbols_free(keysyms);

	return keysym;
}

void circulaterequest(xcb_generic_event_t* ev)
{
	auto* e = (xcb_circulate_request_event_t*)ev;

	/*
	 * Subwindow e->window to parent e->event is about to be restacked.
	 * Just do what was requested, e->place is either
	 * XCB_PLACE_ON_TOP or _ON_BOTTOM.
	 */
	xcb_circulate_window(conn, e->window, e->place);
}

void handle_keypress(xcb_generic_event_t* e)
{
	auto* ev = (xcb_key_press_event_t*)e;
	xcb_keysym_t keysym = xcb_get_keysym(ev->detail);

	for (unsigned int i = 0; i < LENGTH(keys); i++) {
		if (keysym == keys[i].keysym && CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) &&
		    keys[i].func) {
			keys[i].func(&keys[i].arg);
			break;
		}
	}
}

/* Helper function to configure a window. */
void configwin(xcb_window_t win, uint16_t mask, const struct Winconf* wc)
{
	uint32_t values[7];
	int8_t i = -1;

	if (mask & XCB_CONFIG_WINDOW_X) {
		mask |= XCB_CONFIG_WINDOW_X;
		i++;
		values[i] = wc->x;
	}

	if (mask & XCB_CONFIG_WINDOW_Y) {
		mask |= XCB_CONFIG_WINDOW_Y;
		i++;
		values[i] = wc->y;
	}

	if (mask & XCB_CONFIG_WINDOW_WIDTH) {
		mask |= XCB_CONFIG_WINDOW_WIDTH;
		i++;
		values[i] = wc->width;
	}

	if (mask & XCB_CONFIG_WINDOW_HEIGHT) {
		mask |= XCB_CONFIG_WINDOW_HEIGHT;
		i++;
		values[i] = wc->height;
	}

	if (mask & XCB_CONFIG_WINDOW_SIBLING) {
		mask |= XCB_CONFIG_WINDOW_SIBLING;
		i++;
		values[i] = wc->sibling;
	}

	if (mask & XCB_CONFIG_WINDOW_STACK_MODE) {
		mask |= XCB_CONFIG_WINDOW_STACK_MODE;
		i++;
		values[i] = wc->stackmode;
	}

	if (i == -1) return;

	xcb_configure_window(conn, win, mask, values);
	xcb_flush(conn);
}

void configurerequest(xcb_generic_event_t* ev)
{
	auto* e = (xcb_configure_request_event_t*)ev;
	Client* client;
	struct Winconf wc;
	int16_t mon_x, mon_y;
	uint16_t mon_width, mon_height;
	uint32_t values[1];

	if ((client = const_cast<Client*>(findclient(&e->window)))) { /* Find the client. */
		getmonsize(1, &mon_x, &mon_y, &mon_width, &mon_height, client);

		if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
			if (!client->maxed && !client->hormaxed) client->width = e->width;

		if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
			if (!client->maxed && !client->vertmaxed) client->height = e->height;

		if (e->value_mask & XCB_CONFIG_WINDOW_X)
			if (!client->maxed && !client->hormaxed) client->x = e->x;

		if (e->value_mask & XCB_CONFIG_WINDOW_Y)
			if (!client->maxed && !client->vertmaxed) client->y = e->y;

		/* XXX Do we really need to pass on sibling and stack mode
		 * configuration? Do we want to? */
		if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
			values[0] = e->sibling;
			xcb_configure_window(conn, e->window, XCB_CONFIG_WINDOW_SIBLING, values);
		}

		if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
			values[0] = e->stack_mode;
			xcb_configure_window(conn, e->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
		}

		/* Check if window fits on screen after resizing. */
		if (!client->maxed) {
			resizelim(client);
			movelim(client);
			fitonscreen(client);
		}

		setborders(client, true);
	} else {
		/* Unmapped window, pass all options except border width. */
		wc.x = e->x;
		wc.y = e->y;
		wc.width = e->width;
		wc.height = e->height;
		wc.sibling = e->sibling;
		wc.stackmode = e->stack_mode;

		configwin(e->window, e->value_mask, &wc);
	}
}

auto Create_Font_Cursor(xcb_connection_t* conn, uint16_t glyph) -> xcb_cursor_t
{
	static xcb_font_t cursor_font;

	cursor_font = xcb_generate_id(conn);
	xcb_open_font(conn, cursor_font, strlen("cursor"), "cursor");
	xcb_cursor_t cursor = xcb_generate_id(conn);
	xcb_create_glyph_cursor(conn, cursor, cursor_font, cursor_font, glyph, glyph + 1, 0x3232,
				0x3232, 0x3232, 0xeeee, 0xeeee, 0xeeec);

	return cursor;
}

auto create_back_win() -> std::unique_ptr<Client>
{
	uint32_t values[1] = {conf.focuscol};

	auto id = xcb_generate_id(conn);
	xcb_create_window(conn,
			  /* depth */
			  XCB_COPY_FROM_PARENT,
			  /* window Id */
			  id,
			  /* parent window */
			  screen->root,
			  /* x, y */
			  focuswin->x, focuswin->y,
			  /* width, height */
			  focuswin->width, focuswin->height,
			  /* border width */
			  borders[3],
			  /* class */
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  /* visual */
			  screen->root_visual, XCB_CW_BORDER_PIXEL, values);

	if (conf.enable_compton) {
		values[0] = 1;
		xcb_change_window_attributes(conn, id, XCB_BACK_PIXMAP_PARENT_RELATIVE, values);
	} else {
		values[0] = conf.unfocuscol;
		xcb_change_window_attributes(conn, id, XCB_CW_BACK_PIXEL, values);
	}

	return std::make_unique<Client>(
		id, screen->width_in_pixels, screen->height_in_pixels, focuswin->x, focuswin->y,
		focuswin->width, focuswin->height, focuswin->min_width, focuswin->min_height,
		focuswin->width_inc, focuswin->height_inc, focuswin->base_width,
		focuswin->base_height, focuswin->unkillable, focuswin->fixed, focuswin->monitor,
		focuswin->ignore_borders);
}

void mousemotion(const Arg* arg)
{
	int16_t mx, my, winx, winy, winw, winh;
	xcb_query_pointer_reply_t* pointer;
	xcb_grab_pointer_reply_t* grab_reply;
	xcb_motion_notify_event_t* ev = nullptr;
	xcb_generic_event_t* e = nullptr;
	bool ungrab;

	pointer = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, screen->root), nullptr);

	if (!pointer || focuswin->maxed) {
		free(pointer);
		return;
	}

	mx = pointer->root_x;
	my = pointer->root_y;
	winx = focuswin->x;
	winy = focuswin->y;
	winw = focuswin->width;
	winh = focuswin->height;

	xcb_cursor_t cursor;
	std::unique_ptr<Client> example;
	raise_current_window();

	if (arg->i == TWOBWM_MOVE)
		cursor = Create_Font_Cursor(conn, 52); /* fleur */
	else {
		cursor = Create_Font_Cursor(conn, 120); /* sizing */
		example = create_back_win();
		xcb_map_window(conn, example->id);
	}

	grab_reply =
		xcb_grab_pointer_reply(conn,
				       xcb_grab_pointer(conn, 0, screen->root,
							BUTTONMASK | XCB_EVENT_MASK_BUTTON_MOTION |
								XCB_EVENT_MASK_POINTER_MOTION,
							XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
							XCB_NONE, cursor, XCB_CURRENT_TIME),
				       nullptr);

	if (grab_reply->status != XCB_GRAB_STATUS_SUCCESS) {
		free(grab_reply);

		if (arg->i == TWOBWM_RESIZE) xcb_unmap_window(conn, example->id);

		return;
	}

	free(grab_reply);
	ungrab = false;

	do {
		if (nullptr != e) free(e);

		while (!(e = xcb_wait_for_event(conn))) xcb_flush(conn);

		switch (e->response_type & ~0x80) {
		case XCB_CONFIGURE_REQUEST:
		case XCB_MAP_REQUEST:
			events[e->response_type & ~0x80](e);
			break;
		case XCB_MOTION_NOTIFY:
			ev = (xcb_motion_notify_event_t*)e;
			if (arg->i == TWOBWM_MOVE)
				mousemove(winx + ev->root_x - mx, winy + ev->root_y - my);
			else
				mouseresize(example.get(), winw + ev->root_x - mx,
					    winh + ev->root_y - my);

			xcb_flush(conn);
			break;
		case XCB_KEY_PRESS:
		case XCB_KEY_RELEASE:
		case XCB_BUTTON_PRESS:
		case XCB_BUTTON_RELEASE:
			if (arg->i == TWOBWM_RESIZE) {
				ev = (xcb_motion_notify_event_t*)e;

				mouseresize(focuswin, winw + ev->root_x - mx,
					    winh + ev->root_y - my);

				setborders(focuswin, true);
			}

			ungrab = true;
			break;
		}
	} while (!ungrab && focuswin != nullptr);

	free(pointer);
	free(e);
	xcb_free_cursor(conn, cursor);
	xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);

	if (arg->i == TWOBWM_RESIZE) xcb_unmap_window(conn, example->id);

	xcb_flush(conn);
}

void buttonpress(xcb_generic_event_t* ev)
{
	auto* e = (xcb_button_press_event_t*)ev;
	Client const* client;
	unsigned int i;

	if (!is_sloppy && e->detail == XCB_BUTTON_INDEX_1 && CLEANMASK(e->state) == 0) {
		// skip if already focused
		if (nullptr != focuswin && e->event == focuswin->id) { return; }
		client = findclient(&e->event);
		if (nullptr != client) {
			setfocus(client);
			raisewindow(client->id);
			setborders(client, true);
		}
		return;
	}

	for (i = 0; i < LENGTH(buttons); i++)
		if (buttons[i].func && buttons[i].button == e->detail &&
		    CLEANMASK(buttons[i].mask) == CLEANMASK(e->state)) {
			if ((focuswin == nullptr) && buttons[i].func == mousemotion) return;
			if (buttons[i].root_only) {
				if (e->event == e->root && e->child == 0)
					buttons[i].func(&(buttons[i].arg));
			} else {
				buttons[i].func(&(buttons[i].arg));
			}
		}
}

void clientmessage(xcb_generic_event_t* ev)
{
	auto* e = (xcb_client_message_event_t*)ev;
	Client* cl;

	if ((e->type == ATOM[wm_change_state] && e->format == 32 &&
	     e->data.data32[0] == XCB_ICCCM_WM_STATE_ICONIC) ||
	    e->type == ewmh->_NET_ACTIVE_WINDOW) {
		cl = const_cast<Client*>(findclient(&e->window));

		if (nullptr == cl) return;

		if (false == cl->iconic) {
			if (e->type == ewmh->_NET_ACTIVE_WINDOW) {
				setfocus(cl);
				raisewindow(cl->id);
			} else {
				hide();
			}

			return;
		}

		cl->iconic = false;
		xcb_map_window(conn, cl->id);
		setfocus(cl);
	} else if (e->type == ewmh->_NET_CURRENT_DESKTOP)
		changeworkspace_helper(e->data.data32[0]);
	else if (e->type == ewmh->_NET_WM_STATE && e->format == 32) {
		cl = const_cast<Client*>(findclient(&e->window));
		if (nullptr == cl) return;
		if (e->data.data32[1] == ewmh->_NET_WM_STATE_FULLSCREEN ||
		    e->data.data32[2] == ewmh->_NET_WM_STATE_FULLSCREEN) {
			switch (e->data.data32[0]) {
			case XCB_EWMH_WM_STATE_REMOVE:
				unmaxwin(cl);
				break;
			case XCB_EWMH_WM_STATE_ADD:
				maxwin(cl, false);
				break;
			case XCB_EWMH_WM_STATE_TOGGLE:
				if (cl->maxed)
					unmaxwin(cl);
				else
					maxwin(cl, false);
				break;

			default:
				break;
			}
		}
	} else if (e->type == ewmh->_NET_WM_DESKTOP && e->format == 32) {
		cl = const_cast<Client*>(findclient(&e->window));
		if (nullptr == cl) return;
		/*
		 * e->data.data32[1] Source indication
		 * 0: backward compat
		 * 1: normal
		 * 2: pager/bars
		 *
		 * e->data.data32[0] new workspace
		 */
		delfromworkspace(cl);
		addtoworkspace(cl, e->data.data32[0]);
		xcb_unmap_window(conn, cl->id);
		xcb_flush(conn);
	}
}

void destroynotify(xcb_generic_event_t* ev)
{
	Client const* cl;

	auto* e = (xcb_destroy_notify_event_t*)ev;
	if (nullptr != focuswin && focuswin->id == e->window) focuswin = nullptr;

	cl = findclient(&e->window);

	/* Find this window in list of clients and forget about it. */
	if (nullptr != cl) forgetwin(cl->id);

	updateclientlist();
}

void enternotify(xcb_generic_event_t* ev)
{
	auto* e = (xcb_enter_notify_event_t*)ev;
	Client* client;
	unsigned int modifiers[] = {0, XCB_MOD_MASK_LOCK, numlockmask,
				    numlockmask | XCB_MOD_MASK_LOCK};

	/*
	 * If this isn't a normal enter notify, don't bother. We also need
	 * ungrab events, since these will be generated on button and key
	 * grabs and if the user for some reason presses a button on the
	 * root and then moves the pointer to our window and releases the
	 * button, we get an Ungrab EnterNotify. The other cases means the
	 * pointer is grabbed and that either means someone is using it for
	 * menu selections or that we're moving or resizing. We don't want
	 * to change focus in those cases.
	 */

	if (e->mode == XCB_NOTIFY_MODE_NORMAL || e->mode == XCB_NOTIFY_MODE_UNGRAB) {
		/* If we're entering the same window we focus now,
		 * then don't bother focusing. */

		if (nullptr != focuswin && e->event == focuswin->id) return;

		/* Otherwise, set focus to the window we just entered if we
		 * can find it among the windows we know about.
		 * If not, just keep focus in the old window. */

		client = const_cast<Client*>(findclient(&e->event));
		if (nullptr == client) return;

		/* skip this if not is_sloppy
		 * we'll focus on click instead (see buttonpress function)
		 * thus we have to grab left click button on that window
		 * the grab is removed at the end of the setfocus function,
		 * in the grabbuttons when not in sloppy mode
		 */
		if (!is_sloppy) {
			for (unsigned int modifier : modifiers) {
				xcb_grab_button(conn,
						0, // owner_events => 0 means
						   // the grab_window won't
						   // receive this event
						client->id, XCB_EVENT_MASK_BUTTON_PRESS,
						XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
						screen->root, XCB_NONE, XCB_BUTTON_INDEX_1,
						modifier);
			}
			return;
		}

		setfocus(client);
		setborders(client, true);
	}
}

void unmapnotify(xcb_generic_event_t* ev)
{
	auto* e = (xcb_unmap_notify_event_t*)ev;
	/*
	 * Find the window in our current workspace list, then forget about it.
	 * Note that we might not know about the window we got the UnmapNotify
	 * event for.
	 * It might be a window we just unmapped on *another* workspace when
	 * changing workspaces, for instance, or it might be a window with
	 * override redirect set.
	 * This is not an error.
	 * XXX We might need to look in the global window list, after all.
	 * Consider if a window is unmapped on our last workspace while
	 * changing workspaces.
	 * If we do this, we need to keep track of our own windows and
	 * ignore UnmapNotify on them.
	 */
	auto client = const_cast<Client*>(findclient(&e->window));
	if (nullptr == client || client->ws != curws) return;
	if (focuswin != nullptr && client->id == focuswin->id) focuswin = nullptr;
	if (client->iconic == false) forgetclient(client);

	updateclientlist();
}

void mapnotify(xcb_generic_event_t* ev)
{
	auto* e = (xcb_mapping_notify_event_t*)ev;
	xcb_key_symbols_t* keysyms;
	if (!(keysyms = xcb_key_symbols_alloc(conn))) return;
	xcb_refresh_keyboard_mapping(keysyms, e);
	xcb_key_symbols_free(keysyms);

	setup_keyboard();
	grabkeys();
}

void confignotify(xcb_generic_event_t* ev)
{
	auto* e = (xcb_configure_notify_event_t*)ev;

	if (e->window == screen->root) {
		/*
		 * When using RANDR or Xinerama, the root can change geometry
		 * when the user adds a new screen, tilts their screen 90
		 * degrees or whatnot. We might need to rearrange windows to
		 * be visible.
		 * We might get notified for several reasons, not just if the
		 * geometry changed.
		 * If the geometry is unchanged we do nothing.
		 */

		if (e->width != screen->width_in_pixels || e->height != screen->height_in_pixels) {
			screen->width_in_pixels = e->width;
			screen->height_in_pixels = e->height;

			if (-1 == randrbase) arrangewindows();
		}
	}
}

void run()
{
	sigcode = 0;

	while (0 == sigcode) {
		/* the WM is running */
		xcb_flush(conn);

		if (xcb_connection_has_error(conn)) {
			cleanup();
			abort();
		}
		if ((ev = xcb_wait_for_event(conn))) {
			if (ev->response_type == randrbase + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
				getrandr();

			if (events[ev->response_type & ~0x80])
				events[ev->response_type & ~0x80](ev);

			if (top_win != 0) raisewindow(top_win);

			free(ev);
		}
	}
	if (sigcode == SIGHUP) {
		sigcode = 0;
		twobwm_restart();
	}
}

/* Get a defined atom from the X server. */
auto getatom(const char* atom_name) -> xcb_atom_t
{
	xcb_intern_atom_cookie_t atom_cookie =
		xcb_intern_atom(conn, 0, strlen(atom_name), atom_name);

	xcb_intern_atom_reply_t* rep = xcb_intern_atom_reply(conn, atom_cookie, nullptr);

	/* XXX Note that we return 0 as an atom if anything goes wrong.
	 * Might become interesting.*/

	if (nullptr == rep) return 0;

	xcb_atom_t atom = rep->atom;

	free(rep);
	return atom;
}

/* set the given client to listen to button events (presses / releases) */
void grabbuttons(Client const* c)
{
	unsigned int modifiers[] = {0, XCB_MOD_MASK_LOCK, numlockmask,
				    numlockmask | XCB_MOD_MASK_LOCK};

	for (auto& button : buttons)
		if (!button.root_only) {
			for (unsigned int modifier : modifiers)
				xcb_grab_button(conn, 1, c->id, XCB_EVENT_MASK_BUTTON_PRESS,
						XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
						screen->root, XCB_NONE, button.button,
						button.mask | modifier);
		}

	/* ungrab the left click, otherwise we can't use it
	 * we've previously grabbed the left click in the enternotify function
	 * when not in sloppy mode
	 * though the name is counter-intuitive to the method
	 */
	for (unsigned int modifier : modifiers) {
		xcb_ungrab_button(conn, XCB_BUTTON_INDEX_1, c->id, modifier);
	}
}

void ewmh_init()
{
	ewmh = std::unique_ptr<xcb_ewmh_connection_t, decltype(&ewmh_deleter)>{
		new xcb_ewmh_connection_t, ewmh_deleter};
	xcb_intern_atom_cookie_t* cookie = xcb_ewmh_init_atoms(conn, ewmh.get());
	if (!xcb_ewmh_init_atoms_replies(ewmh.get(), cookie, nullptr)) {
		fprintf(stderr, "%s\n", "xcb_ewmh_init_atoms_replies:faild.");
		exit(1);
	}
}

auto setup(int scrno) -> bool
{
	unsigned int i;
	uint32_t event_mask_pointer[] = {XCB_EVENT_MASK_POINTER_MOTION};

	unsigned int values[1] = {XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
				  XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
				  XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS};

	screen = xcb_screen_of_display(conn, scrno);

	if (!screen) return false;

	ewmh_init();
	xcb_ewmh_set_wm_pid(ewmh.get(), screen->root, getpid());
	xcb_ewmh_set_wm_name(ewmh.get(), screen->root, 4, "2bwm");

	xcb_atom_t net_atoms[] = {ewmh->_NET_SUPPORTED,
				  ewmh->_NET_WM_DESKTOP,
				  ewmh->_NET_NUMBER_OF_DESKTOPS,
				  ewmh->_NET_CURRENT_DESKTOP,
				  ewmh->_NET_ACTIVE_WINDOW,
				  ewmh->_NET_WM_ICON,
				  ewmh->_NET_WM_STATE,
				  ewmh->_NET_WM_NAME,
				  ewmh->_NET_SUPPORTING_WM_CHECK,
				  ewmh->_NET_WM_STATE_HIDDEN,
				  ewmh->_NET_WM_ICON_NAME,
				  ewmh->_NET_WM_WINDOW_TYPE,
				  ewmh->_NET_WM_WINDOW_TYPE_DOCK,
				  ewmh->_NET_WM_WINDOW_TYPE_DESKTOP,
				  ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR,
				  ewmh->_NET_WM_PID,
				  ewmh->_NET_CLIENT_LIST,
				  ewmh->_NET_CLIENT_LIST_STACKING,
				  ewmh->WM_PROTOCOLS,
				  ewmh->_NET_WM_STATE,
				  ewmh->_NET_WM_STATE_DEMANDS_ATTENTION,
				  ewmh->_NET_WM_STATE_FULLSCREEN};

	xcb_ewmh_set_supported(ewmh.get(), scrno, LENGTH(net_atoms), net_atoms);

	// Load the default config anyway.
	conf.borderwidth = borders[1];
	conf.outer_border = borders[0];
	conf.focuscol = getcolor(colors[0]);
	conf.unfocuscol = getcolor(colors[1]);
	conf.fixedcol = getcolor(colors[2]);
	conf.unkillcol = getcolor(colors[3]);
	conf.outer_border_col = getcolor(colors[5]);
	conf.fixed_unkil_col = getcolor(colors[4]);
	conf.empty_col = getcolor(colors[6]);
	conf.inverted_colors = inverted_colors;
	conf.enable_compton = false;

	for (i = 0; i < NB_ATOMS; i++) ATOM[i] = getatom(atomnames[i][0]);

	randrbase = setuprandr();

	if (!setupscreen()) return false;

	if (!setup_keyboard()) return false;

	xcb_generic_error_t* error =
		xcb_request_check(conn, xcb_change_window_attributes_checked(
						conn, screen->root, XCB_CW_EVENT_MASK, values));
	xcb_flush(conn);

	if (error) {
		fprintf(stderr, "%s\n", "xcb_request_check:faild.");
		free(error);
		return false;
	}
	xcb_ewmh_set_current_desktop(ewmh.get(), scrno, curws);
	xcb_ewmh_set_number_of_desktops(ewmh.get(), scrno, WORKSPACES);

	grabkeys();
	/* set events */
	for (i = 0; i < XCB_NO_OPERATION; i++) events[i] = nullptr;

	events[XCB_CONFIGURE_REQUEST] = configurerequest;
	events[XCB_DESTROY_NOTIFY] = destroynotify;
	events[XCB_ENTER_NOTIFY] = enternotify;
	events[XCB_KEY_PRESS] = handle_keypress;
	events[XCB_MAP_REQUEST] = newwin;
	events[XCB_UNMAP_NOTIFY] = unmapnotify;
	events[XCB_MAPPING_NOTIFY] = mapnotify;
	events[XCB_CONFIGURE_NOTIFY] = confignotify;
	events[XCB_CIRCULATE_REQUEST] = circulaterequest;
	events[XCB_BUTTON_PRESS] = buttonpress;
	events[XCB_CLIENT_MESSAGE] = clientmessage;

	return true;
}

void twobwm_restart()
{
	xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
	xcb_disconnect(conn);
	execvp(TWOBWM_PATH, nullptr);
}

void sigcatch(const int sig)
{
	sigcode = sig;
}

void install_sig_handlers()
{
	struct sigaction sa;
	struct sigaction sa_chld;
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;
	// could not initialize signal handler
	if (sigaction(SIGCHLD, &sa, nullptr) == -1) exit(-1);
	sa.sa_handler = sigcatch;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART; /* Restart if interrupted by handler */
	if (sigaction(SIGINT, &sa, nullptr) == -1 || sigaction(SIGHUP, &sa, nullptr) == -1 ||
	    sigaction(SIGTERM, &sa, nullptr) == -1)
		exit(-1);
}

auto main() -> int
{
	int scrno = 0;
	atexit(cleanup);
	install_sig_handlers();
	if (!xcb_connection_has_error(conn = xcb_connect(nullptr, &scrno)))
		if (setup(scrno)) run();
	/* the WM has stopped running, because sigcode is not 0 */
	exit(sigcode);
}
