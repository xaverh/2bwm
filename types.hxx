#include <xcb/randr.h>

///---Types---///
struct Monitor {
	xcb_randr_output_t id;
	int16_t y, x;           // X and Y.
	uint16_t width, height; // Width/Height in pixels.
	struct item* item;      // Pointer to our place in output list.
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

struct Client {                  // Everything we know about a window.
	xcb_drawable_t id;       // ID of this window.
	bool usercoord;          // X,Y was set by -geom.
	int16_t x, y;            // X/Y coordinate.
	uint16_t width, height;  // Width,Height in pixels.
	uint8_t depth;           // pixel depth
	struct Sizepos origsize; // Original size if we're currently maxed.
	uint16_t max_width, max_height, min_width, min_height, width_inc, height_inc, base_width,
		base_height;
	bool fixed, unkillable, vertmaxed, hormaxed, maxed, verthor, ignore_borders, iconic;
	struct Monitor* monitor; // The physical output this window is on.
	struct item* winitem;    // Pointer to our place in global windows list.
	struct item* wsitem;     // Pointer to workspace window list.
	int ws;                  // In which workspace this window belongs to.
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
