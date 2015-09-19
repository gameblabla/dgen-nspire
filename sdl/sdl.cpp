/**
 * SDL interface
 */

#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <SDL.h>
#include <SDL_audio.h>

#ifdef HAVE_MEMCPY_H
#include "memcpy.h"
#endif
#include "md.h"
#include "rc-vars.h"
#include "pd.h"
#include "system.h"
#include "romload.h"

/// Number of microseconds to sustain messages
#define MESSAGE_LIFE 3000000

int pressed;

/// Generic type for supported colour depths.
typedef union {
	uint8_t *u8;
	uint32_t *u32;
	uint24_t *u24;
	uint16_t *u16;
	uint16_t *u15;
} bpp_t;

struct screen {
	unsigned int window_width; ///< window width
	unsigned int window_height; ///< window height
	unsigned int width; ///< buffer width
	unsigned int height; ///< buffer height
	unsigned int bpp; ///< bits per pixel
	unsigned int Bpp; ///< bytes per pixel
	unsigned int x_scale; ///< horizontal scale factor
	unsigned int y_scale; ///< vertical scale factor
	unsigned int info_height; ///< message bar height (included in height)
	bpp_t buf; ///< generic pointer to pixel data
	unsigned int pitch; ///< number of bytes per line in buf
	SDL_Surface *surface; ///< SDL surface
	unsigned int want_fullscreen:1; ///< want fullscreen
	unsigned int is_fullscreen:1; ///< fullscreen enabled
	SDL_Color color[64]; ///< SDL colors for 8bpp modes
};

static struct screen screen;

static struct {
	const unsigned int width; ///< 320
	unsigned int height; ///< 224 or 240 (NTSC_VBLANK or PAL_VBLANK)
	unsigned int hz; ///< refresh rate
	unsigned int is_pal: 1; ///< PAL enabled
	uint8_t palette[256]; ///< palette for 8bpp modes (mdpal)
} video = {
	320, ///< width is always 320
	NTSC_VBLANK, ///< NTSC height by default
	NTSC_HZ, ///< 60Hz
	0, ///< NTSC is enabled
	{ 0 }
};

/**
 * Call this before accessing screen.buf.
 * No syscalls allowed before screen_unlock().
 */
static int screen_lock()
{
	if (SDL_MUSTLOCK(screen.surface) == 0)
		return 0;
	return SDL_LockSurface(screen.surface);
}

/**
 * Call this after accessing screen.buf.
 */
static void screen_unlock()
{
	if (SDL_MUSTLOCK(screen.surface) == 0)
		return;
	SDL_UnlockSurface(screen.surface);
}

/**
 * Do not call this directly, use screen_update() instead.
 */
static void screen_update_once()
{
	SDL_Flip(screen.surface);
}

/**
 * Call this after writing into screen.buf.
 */
static void screen_update()
{
		screen_update_once();
}

/**
 * Clear screen.
 */
static void screen_clear()
{
	if ((screen.buf.u8 == NULL) || (screen_lock()))
		return;
	memset(screen.buf.u8, 0, (screen.pitch * screen.height));
	screen_unlock();
}

// Bad hack- extern slot etc. from main.cpp so we can save/load states
extern int slot;
void md_save(md &megad);
void md_load(md &megad);

// Define externed variables
struct bmap mdscr;
unsigned char *mdpal = NULL;
struct sndinfo sndi;
const char *pd_options =
	"fX:Y:S:G:";

/// Circular buffer and related functions.
typedef struct {
	size_t i; ///< data start index
	size_t s; ///< data size
	size_t size; ///< buffer size
	union {
		uint8_t *u8;
		int16_t *i16;
	} data; ///< storage
} cbuf_t;

/**
 * Write/copy data into a circular buffer.
 * @param[in,out] cbuf Destination buffer.
 * @param[in] src Buffer to copy from.
 * @param size Size of src.
 * @return Number of bytes copied.
 */
size_t cbuf_write(cbuf_t *cbuf, uint8_t *src, size_t size)
{
	size_t j;
	size_t k;

	if (size > cbuf->size) {
		src += (size - cbuf->size);
		size = cbuf->size;
	}
	k = (cbuf->size - cbuf->s);
	j = ((cbuf->i + cbuf->s) % cbuf->size);
	if (size > k) {
		cbuf->i = ((cbuf->i + (size - k)) % cbuf->size);
		cbuf->s = cbuf->size;
	}
	else
		cbuf->s += size;
	k = (cbuf->size - j);
	if (k >= size) {
		memcpy(&cbuf->data.u8[j], src, size);
	}
	else {
		memcpy(&cbuf->data.u8[j], src, k);
		memcpy(&cbuf->data.u8[0], &src[k], (size - k));
	}
	return size;
}

/**
 * Read bytes out of a circular buffer.
 * @param[out] dst Destination buffer.
 * @param[in,out] cbuf Circular buffer to read from.
 * @param size Maximum number of bytes to copy to dst.
 * @return Number of bytes copied.
 */
size_t cbuf_read(uint8_t *dst, cbuf_t *cbuf, size_t size)
{
	if (size > cbuf->s)
		size = cbuf->s;
	if ((cbuf->i + size) > cbuf->size) {
		size_t k = (cbuf->size - cbuf->i);

		memcpy(&dst[0], &cbuf->data.u8[(cbuf->i)], k);
		memcpy(&dst[k], &cbuf->data.u8[0], (size - k));
	}
	else
		memcpy(&dst[0], &cbuf->data.u8[(cbuf->i)], size);
	cbuf->i = ((cbuf->i + size) % cbuf->size);
	cbuf->s -= size;
	return size;
}

/// Sound
static struct {
	unsigned int rate; ///< samples rate
	unsigned int samples; ///< number of samples required by the callback
	cbuf_t cbuf; ///< circular buffer
} sound;

/// Messages
static struct {
	unsigned int displayed:1; ///< whether message is currently displayed
	unsigned long since; ///< since this number of microseconds
	size_t length; ///< remaining length to display
	char message[2048]; ///< message
} info;

/// Prompt return values
#define PROMPT_RET_CONT 0x01 ///< waiting for more input
#define PROMPT_RET_EXIT 0x02 ///< leave prompt normally
#define PROMPT_RET_ERROR 0x04 ///< leave prompt with error
#define PROMPT_RET_ENTER 0x10 ///< previous line entered
#define PROMPT_RET_MSG 0x80 ///< pd_message() has been used

/// Extra commands return values.
#define CMD_OK 0x00 ///< command successful
#define CMD_EINVAL 0x01 ///< invalid argument
#define CMD_FAIL 0x02 ///< command failed
#define CMD_ERROR 0x03 ///< fatal error, DGen should exit
#define CMD_MSG 0x80 ///< pd_message() has been used

/// Stopped flag used by pd_stopped()
static int stopped = 0;

/// Events handling status.
static enum events {
	STARTED,
	STOPPED,
	STOPPED_PROMPT,
	STOPPED_GAME_GENIE,
	PROMPT,
	GAME_GENIE
} events = STARTED;

static int stop_events(md& megad, enum events status);
static void restart_events(md &megad);

/// Messages shown whenever events are stopped.
static const char stopped_str[] = "STOPPED.";
static const char prompt_str[] = ":";
static const char game_genie_str[] = "Enter Game Genie/Hex code: ";

/// Enable emulation by default.
bool pd_freeze = false;
static unsigned int pd_freeze_ref = 0;

static void freeze(bool toggle)
{
	if (toggle == true) {
		if (!pd_freeze_ref) {
			assert(pd_freeze == false);
			pd_freeze = true;
		}
		pd_freeze_ref++;
	}
	else {
		if (pd_freeze_ref) {
			assert(pd_freeze == true);
			pd_freeze_ref--;
			if (!pd_freeze_ref)
				pd_freeze = false;
		}
		else
			assert(pd_freeze == false);
	}
}


/**
 * Prompt "exit" command handler.
 * @return Error status to make DGen exit.
 */
static int prompt_cmd_exit(class md&, unsigned int, const char**)
{
	return (CMD_ERROR | CMD_MSG);
}

/**
 * Prompt "load" command handler.
 * @param md Context.
 * @param ac Number of arguments in av.
 * @param av Arguments.
 * @return Status code.
 */
static int prompt_cmd_load(class md& md, unsigned int ac, const char** av)
{
	extern int slot;
	extern void ram_save(class md&);
	extern void ram_load(class md&);
	char *s;

	if (ac != 2)
		return CMD_EINVAL;
	s = backslashify((const uint8_t *)av[1], strlen(av[1]), 0, NULL);
	if (s == NULL)
		return CMD_FAIL;
	ram_save(md);
	if (dgen_autosave) {
		slot = 0;
		md_save(md);
	}
	md.unplug();
	if (md.load(av[1])) {
		free(s);
		return (CMD_FAIL | CMD_MSG);
	}
	free(s);
	if (dgen_show_carthead)
		pd_show_carthead(md);
	// Initialize like main() does.
	md.reset();

	if (!dgen_region) {
		uint8_t c = md.region_guess();
		int hz;
		int pal;

		md::region_info(c, &pal, &hz, 0, 0, 0);
		if ((hz != dgen_hz) || (pal != dgen_pal) || (c != md.region)) {
			md.region = c;
			dgen_hz = hz;
			dgen_pal = pal;
			printf("sdl: reconfiguring for region \"%c\": "
			       "%dHz (%s)\n", c, hz, (pal ? "PAL" : "NTSC"));
			pd_graphics_reinit(dgen_sound, dgen_pal, dgen_hz);
			if (dgen_sound) {
				long rate = dgen_soundrate;
				unsigned int samples;

				pd_sound_deinit();
				samples = (dgen_soundsegs * (rate / dgen_hz));
				pd_sound_init(rate, samples);
			}
			md.pal = pal;
			md.init_pal();
			md.init_sound();
		}
	}

	ram_load(md);
	if (dgen_autoload) {
		slot = 0;
		md_load(md);
	}
	return (CMD_OK | CMD_MSG);
}

struct filter_data {
	bpp_t buf; ///< Input or output buffer.
	unsigned int width; ///< Buffer width.
	unsigned int height; ///< Buffer height.
	unsigned int pitch; ///< Number of bytes per line in buffer.
	void *data; ///< Filter-specific data.
	bool updated:1; ///< Filter updated data to match its output.
	bool failed:1; ///< Filter failed.
};

typedef void filter_func_t(const struct filter_data *in,
			   struct filter_data *out);

struct filter {
	const char *name; ///< Filter name.
	filter_func_t *func; ///< Filtering function.
	bool safe:1; ///< Output buffer can be the same as input.
	bool ctv:1; ///< Part of the CTV filters set.
	bool resize:1; ///< Filter resizes input.
};

static filter_func_t filter_scale;
static filter_func_t filter_off;
static filter_func_t filter_stretch;

static const struct filter filters_available[] = {
	{ "stretch", filter_stretch, false, false, true },
	{ "scale", filter_scale, false, false, true },
};

static unsigned int filters_stack_size;
static bool filters_stack_default;
static const struct filter *filters_stack[64];
static bpp_t filters_stack_data_buf[2];
static struct filter_data filters_stack_data[1 + elemof(filters_stack)];

/**
 * Return filter structure associated with name.
 * @param name Name of filter.
 * @return Pointer to filter or NULL if not found.
 */
static const struct filter *filters_find(const char *name)
{
	size_t i;

	for (i = 0; (i != elemof(filters_available)); ++i)
		if (!strcasecmp(name, filters_available[i].name))
			return &filters_available[i];
	return NULL;
}

/**
 * Update filters data, reallocate extra buffers if necessary.
 */
static void filters_stack_update()
{
	size_t i;
	const struct filter *f;
	struct filter_data *fd;
	unsigned int buffers;
	bpp_t buf[2];
	struct filter_data in_fd = {
		// Use the same formula as draw_scanline() in ras.cpp to avoid
		// the messy border for any supported depth.
		{ ((uint8_t *)mdscr.data + (mdscr.pitch * 8) + 16) },
		video.width,
		video.height,
		(unsigned int)mdscr.pitch,
		NULL,
		false,
		false,
	};
	struct filter_data out_fd = {
		{ screen.buf.u8 },
		screen.width,
		(screen.height - screen.info_height),
		screen.pitch,
		NULL,
		false,
		false,
	};
	struct filter_data *prev_fd;

	DEBUG(("updating filters data"));
retry:
	assert(filters_stack_size <= elemof(filters_stack));
	buffers = 0;
	buf[0].u8 = filters_stack_data_buf[0].u8;
	buf[1].u8 = filters_stack_data_buf[1].u8;
	// Get the number of defined filters and count how many of them cannot
	// use the same buffer for both input and output.
	// Unless they are on top, "unsafe" filters require extra buffers.
	assert(filters_stack_data[0].data == NULL);
	for (i = 0; (i != elemof(filters_stack)); ++i) {
		if (i == filters_stack_size)
			break;
		f = filters_stack[i];
		assert(f != NULL);
		if ((f->safe == false) && (i != (filters_stack_size - 1)))
			++buffers;
		// Clear filters stack output data.
		free(filters_stack_data[i + 1].data);
	}
	memset(filters_stack_data, 0, sizeof(filters_stack_data));
	// Add a valid default filter if stack is empty.
	if (i == 0) {
		assert(filters_stack_size == 0);
		filters_stack[0] = &filters_available[0];
		++filters_stack_size;
		filters_stack_default = true;
		goto retry;
	}
	// Remove default filter if there is one and stack is not empty.
	else if ((i > 1) && (filters_stack_default == true)) {
		assert(filters_stack_size > 1);
		--filters_stack_size;
		memmove(&filters_stack[0], &filters_stack[1],
			(sizeof(filters_stack[0]) * filters_stack_size));
		filters_stack_default = false;
		goto retry;
	}
	// Check if extra buffers are required.
	if (buffers) {
		if (buffers > 2)
			buffers = 2;
		else {
			// Remove unnecessary buffer.
			free(buf[1].u8);
			buf[1].u8 = NULL;
			filters_stack_data_buf[1].u8 = NULL;
		}
		DEBUG(("requiring %u extra buffer(s)", buffers));
		// Reallocate them.
		for (i = 0; (i != buffers); ++i) {
			size_t size = (screen.pitch * screen.height);

			DEBUG(("temporary buffer %u size: %zu", i, size));
			buf[i].u8 =
				(uint8_t *)realloc((void *)buf[i].u8, size);
			if (size == 0) {
				assert(buf[i].u8 == NULL);
				DEBUG(("freed zero-sized buffer"));
				filters_stack_data_buf[i].u8 = NULL;
				continue;
			}
			if (buf[i].u8 == NULL) {
				// Not good, remove one of the filters that
				// require an extra buffer and try again.
				free(filters_stack_data_buf[i].u8);
				filters_stack_data_buf[i].u8 = NULL;
				for (i = 0;
				     (i < filters_stack_size);
				     ++i) {
					if (filters_stack[i]->safe == true)
						continue;
					--filters_stack_size;
					memmove(&filters_stack[i],
						&filters_stack[i + 1],
						(sizeof(filters_stack[i]) *
						 (filters_stack_size - i)));
					break;
				}
				goto retry;
			}
			filters_stack_data_buf[i].u8 = buf[i].u8;
		}
	}
	else {
		// No extra buffer required, deallocate them.
		DEBUG(("removing temporary buffers"));
		for (i = 0; (i != elemof(buf)); ++i) {
			free(buf[i].u8);
			buf[i].u8 = NULL;
			filters_stack_data_buf[i].u8 = NULL;
		}
	}
	// Update I/O buffers.
	buffers = 0;
	prev_fd = &filters_stack_data[0];
	memcpy(prev_fd, &in_fd, sizeof(*prev_fd));
	for (i = 0; (i != elemof(filters_stack)); ++i) {
		if (i == filters_stack_size)
			break;
		f = filters_stack[i];
		fd = &filters_stack_data[i + 1];
		// The last filter uses screen output.
		if (i == (filters_stack_size - 1))
			memcpy(fd, &out_fd, sizeof(*fd));
		// Safe filters have the same input as their output.
		else if (f->safe == true)
			memcpy(fd, prev_fd, sizeof(*fd));
		// Other filters output to a temporary buffer.
		else {
			fd->buf.u8 = buf[buffers].u8;
			fd->width = screen.width;
			fd->height = (screen.height - screen.info_height);
			fd->pitch = screen.pitch;
			fd->data = NULL;
			fd->updated = false;
			fd->failed = false;
			buffers ^= 1;
		}
		prev_fd = fd;
	}
#ifndef NDEBUG
	DEBUG(("filters stack:"));
	for (i = 0; (i != filters_stack_size); ++i)
		DEBUG(("- %s (input: %p output: %p)",
		       filters_stack[i]->name,
		       (void *)filters_stack_data[i].buf.u8,
		       (void *)filters_stack_data[i + 1].buf.u8));
#endif
	screen_clear();
}

/**
 * Add filter to stack.
 * @param f Filter to add.
 */
static void filters_push(const struct filter *f)
{
	assert(filters_stack_size <= elemof(filters_stack));
	if ((f == NULL) || (filters_stack_size == elemof(filters_stack)))
		return;
	DEBUG(("%s", f->name));
	filters_stack[filters_stack_size] = f;
	filters_stack_data[filters_stack_size + 1].data = NULL;
	++filters_stack_size;
	filters_stack_update();
}

/**
 * Insert filter at the bottom of the stack.
 * @param f Filter to insert.
 */
static void filters_insert(const struct filter *f)
{
	assert(filters_stack_size <= elemof(filters_stack));
	if ((f == NULL) ||
	    (filters_stack_size == elemof(filters_stack)))
		return;
	DEBUG(("%s", f->name));
	memmove(&filters_stack[1], &filters_stack[0],
		(filters_stack_size * sizeof(filters_stack[0])));
	filters_stack[0] = f;
	filters_stack_data[0 + 1].data = NULL;
	++filters_stack_size;
	filters_stack_update();
}

// Currently unused.
#if 0

/**
 * Add filter to stack if not already in it.
 * @param f Filter to add.
 */
static void filters_push_once(const struct filter *f)
{
	size_t i;

	assert(filters_stack_size <= elemof(filters_stack));
	if (f == NULL)
		return;
	DEBUG(("%s", f->name));
	for (i = 0; (i != filters_stack_size); ++i)
		if (filters_stack[i] == f)
			return;
	filters_push(f);
}

#endif

/**
 * Remove a filter from anywhere in the stack.
 * @param index Filters stack index.
 */
static void filters_remove(unsigned int index)
{
	assert(filters_stack_size <= elemof(filters_stack));
	if (index >= filters_stack_size)
		return;
	--filters_stack_size;
	DEBUG(("%s", filters_stack[index]->name));
	free(filters_stack_data[index + 1].data);
#ifndef NDEBUG
	memset(&filters_stack[index], 0xf2, sizeof(filters_stack[index]));
	memset(&filters_stack_data[index + 1], 0xf3,
	       sizeof(filters_stack_data[index + 1]));
#endif
	memmove(&filters_stack[index], &filters_stack[index + 1],
		(sizeof(filters_stack[index]) * (filters_stack_size - index)));
	memmove(&filters_stack_data[index + 1], &filters_stack_data[index + 2],
		(sizeof(filters_stack_data[index + 1]) *
		 (filters_stack_size - index)));
	filters_stack_update();
}

/**
 * Remove all occurences of filter from the stack.
 * @param f Filter to remove.
 */
static void filters_pluck(const struct filter *f)
{
	size_t i;

	assert(filters_stack_size <= elemof(filters_stack));
	if (f == NULL)
		return;
	DEBUG(("%s", f->name));
	for (i = 0; (i < filters_stack_size); ++i) {
		if (filters_stack[i] != f)
			continue;
		--filters_stack_size;
		DEBUG(("%s", filters_stack[i]->name));
		free(filters_stack_data[i + 1].data);
#ifndef NDEBUG
		memset(&filters_stack[i], 0xf4, sizeof(filters_stack[i]));
		memset(&filters_stack_data[i + 1], 0xf5,
		       sizeof(filters_stack_data[i + 1]));
#endif
		memmove(&filters_stack[i], &filters_stack[i + 1],
			(sizeof(filters_stack[i]) * (filters_stack_size - i)));
		memmove(&filters_stack_data[i + 1], &filters_stack_data[i + 2],
			(sizeof(filters_stack_data[i + 1]) *
			 (filters_stack_size - i)));
		--i;
	}
	filters_stack_update();
}

/**
 * Take a screenshot.
 */
static void do_screenshot(md& megad)
{
	static unsigned int n = 0;
	static char romname_old[sizeof(megad.romname)];
	FILE *fp;
#ifdef HAVE_FTELLO
	off_t pos;
#else
	long pos;
#endif
	bpp_t line;
	unsigned int width;
	unsigned int height;
	unsigned int pitch;
	unsigned int bpp = mdscr.bpp;
	uint8_t (*out)[3]; // 24 bpp
	char name[(sizeof(megad.romname) + 32)];

	if (dgen_raw_screenshots) {
		width = video.width;
		height = video.height;
		pitch = mdscr.pitch;
		line.u8 = ((uint8_t *)mdscr.data + (pitch * 8) + 16);
	}
	else {
		width = screen.width;
		height = screen.height;
		pitch = screen.pitch;
		line = screen.buf;
	}
	switch (bpp) {
	case 15:
	case 16:
	case 24:
	case 32:
		break;
	default:
		return;
	}
	// Make take a long time, let the main loop know about it.
	stopped = 1;
	// If megad.romname is different from last time, reset n.
	if (memcmp(romname_old, megad.romname, sizeof(romname_old))) {
		memcpy(romname_old, megad.romname, sizeof(romname_old));
		n = 0;
	}
retry:
	snprintf(name, sizeof(name), "%s-%06u.tga",
		 ((megad.romname[0] == '\0') ? "unknown" : megad.romname), n);
	fp = dgen_fopen("screenshots", name, DGEN_APPEND);
	if (fp == NULL) {
		return;
	}
	fseek(fp, 0, SEEK_END);
#ifdef HAVE_FTELLO
	pos = ftello(fp);
#else
	pos = ftell(fp);
#endif
	if (((off_t)pos == (off_t)-1) || ((off_t)pos != (off_t)0)) {
		fclose(fp);
		n = ((n + 1) % 1000000);
		goto retry;
	}
	// Allocate line buffer.
	if ((out = (uint8_t (*)[3])malloc(sizeof(*out) * width)) == NULL)
		goto error;
	// Header
	{
		uint8_t tmp[(3 + 5)] = {
			0x00, // length of the image ID field
			0x00, // whether a color map is included
			0x02 // image type: uncompressed, true-color image
			// 5 bytes of color map specification
		};

		if (!fwrite(tmp, sizeof(tmp), 1, fp))
			goto error;
	}
	{
		uint16_t tmp[4] = {
			0, // x-origin
			0, // y-origin
			h2le16(width), // width
			h2le16(height) // height
		};

		if (!fwrite(tmp, sizeof(tmp), 1, fp))
			goto error;
	}
	{
		uint8_t tmp[2] = {
			24, // always output 24 bits per pixel
			(1 << 5) // top-left origin
		};

		if (!fwrite(tmp, sizeof(tmp), 1, fp))
			goto error;
	}
	// Data
	switch (bpp) {
		unsigned int y;
		unsigned int x;

	case 15:
		for (y = 0; (y < height); ++y) {
			if (screen_lock())
				goto error;
			for (x = 0; (x < width); ++x) {
				uint16_t v = line.u16[x];

				out[x][0] = ((v << 3) & 0xf8);
				out[x][1] = ((v >> 2) & 0xf8);
				out[x][2] = ((v >> 7) & 0xf8);
			}
			screen_unlock();
			if (!fwrite(out, (sizeof(*out) * width), 1, fp))
				goto error;
			line.u8 += pitch;
		}
		break;
	case 16:
		for (y = 0; (y < height); ++y) {
			if (screen_lock())
				goto error;
			for (x = 0; (x < width); ++x) {
				uint16_t v = line.u16[x];

				out[x][0] = ((v << 3) & 0xf8);
				out[x][1] = ((v >> 3) & 0xfc);
				out[x][2] = ((v >> 8) & 0xf8);
			}
			screen_unlock();
			if (!fwrite(out, (sizeof(*out) * width), 1, fp))
				goto error;
			line.u8 += pitch;
		}
		break;
	case 24:
		for (y = 0; (y < height); ++y) {
			if (screen_lock())
				goto error;
#ifdef WORDS_BIGENDIAN
			for (x = 0; (x < width); ++x) {
				out[x][0] = line.u24[x][2];
				out[x][1] = line.u24[x][1];
				out[x][2] = line.u24[x][0];
			}
#else
			memcpy(out, line.u24, (sizeof(*out) * width));
#endif
			screen_unlock();
			if (!fwrite(out, (sizeof(*out) * width), 1, fp))
				goto error;
			line.u8 += pitch;
		}
		break;
	case 32:
		for (y = 0; (y < height); ++y) {
			if (screen_lock())
				goto error;
			for (x = 0; (x < width); ++x) {
#ifdef WORDS_BIGENDIAN
				uint32_t rgb = h2le32(line.u32[x]);

				memcpy(&(out[x]), &rgb, 3);
#else
				memcpy(&(out[x]), &(line.u32[x]), 3);
#endif
			}
			screen_unlock();
			if (!fwrite(out, (sizeof(*out) * width), 1, fp))
				goto error;
			line.u8 += pitch;
		}
		break;
	}
	free(out);
	fclose(fp);
	return;
error:
	free(out);
	fclose(fp);
}

/**
 * SDL flags help.
 */
void pd_help()
{
  printf(
  "    -f              Attempt to run fullscreen.\n"
  "    -X scale        Scale the screen in the X direction.\n"
  "    -Y scale        Scale the screen in the Y direction.\n"
  "    -S scale        Scale the screen by the same amount in both directions.\n"
  "    -G WxH          Desired window size.\n"
  );
}

/**
 * Handle rc variables
 */
void pd_rc()
{
	// Set stuff up from the rcfile first, so we can override it with
	// command-line options
	if (dgen_scale >= 1) {
		dgen_x_scale = dgen_scale;
		dgen_y_scale = dgen_scale;
	}
}

/**
 * Handle the switches.
 * @param c Switch's value.
 */
void pd_option(char c, const char *)
{
	int xs, ys;

	switch (c) {
	case 'f':
		dgen_fullscreen = 1;
		break;
	case 'X':
		if ((xs = atoi(optarg)) <= 0)
			break;
		dgen_x_scale = xs;
		break;
	case 'Y':
		if ((ys = atoi(optarg)) <= 0)
			break;
		dgen_y_scale = ys;
		break;
	case 'S':
		if ((xs = atoi(optarg)) <= 0)
			break;
		dgen_x_scale = xs;
		dgen_y_scale = xs;
		break;
	case 'G':
		if ((sscanf(optarg, " %d x %d ", &xs, &ys) != 2) ||
		    (xs < 0) || (ys < 0))
			break;
		dgen_width = xs;
		dgen_height = ys;
		break;
	}
}

/**
 * This filter passes input to output unchanged, only centered or truncated
 * if necessary. Doesn't have any fallback, thus cannot fail.
 * @param in Input buffer data.
 * @param out Output buffer data.
 */
static void filter_off(const struct filter_data *in, struct filter_data *out)
{
	unsigned int line;
	unsigned int height;
	uint8_t *in_buf;
	uint8_t *out_buf;

	// Check if copying is necessary.
	if (in->buf.u8 == out->buf.u8)
		return;
	// Copy line by line and center.
	if (in->height > out->height)
		height = out->height;
	else
		height = in->height;
	if (out->updated == false) {
		if (in->width <= out->width) {
			unsigned int x_off = ((out->width - in->width) / 2);
			unsigned int y_off = ((out->height - height) / 2);

			out->buf.u8 += (x_off * screen.Bpp);
			out->buf.u8 += (out->pitch * y_off);
			out->width = in->width;
		}
		out->height = height;
		out->updated = true;
	}
	in_buf = in->buf.u8;
	out_buf = out->buf.u8;
	for (line = 0; (line != height); ++line) {
		memcpy(out_buf, in_buf, (out->width * screen.Bpp));
		in_buf += in->pitch;
		out_buf += out->pitch;
	}
}

// Copy/rescale functions.

struct filter_scale_data {
	unsigned int x_scale;
	unsigned int y_scale;
	filter_func_t *filter;
};

template <typename uintX_t>
static void filter_scale_X(const struct filter_data *in,
			   struct filter_data *out)
{
	struct filter_scale_data *data =
		(struct filter_scale_data *)out->data;
	uintX_t *dst = (uintX_t *)out->buf.u32;
	unsigned int dst_pitch = out->pitch;
	uintX_t *src = (uintX_t *)in->buf.u32;
	unsigned int src_pitch = in->pitch;
	unsigned int width = in->width;
	unsigned int x_scale = data->x_scale;
	unsigned int y_scale = data->y_scale;
	unsigned int height = in->height;
	unsigned int y;

	for (y = 0; (y != height); ++y) {
		uintX_t *out = dst;
		unsigned int i;
		unsigned int x;

		for (x = 0; (x != width); ++x) {
			uintX_t tmp = src[x];

			for (i = 0; (i != x_scale); ++i)
				*(out++) = tmp;
		}
		out = dst;
		dst = (uintX_t *)((uint8_t *)dst + dst_pitch);
		for (i = 1; (i < y_scale); ++i) {
			memcpy(dst, out, (width * sizeof(*dst) * x_scale));
			out = dst;
			dst = (uintX_t *)((uint8_t *)dst + dst_pitch);
		}
		src = (uintX_t *)((uint8_t *)src + src_pitch);
	}
}

static void filter_scale_3(const struct filter_data *in,
			   struct filter_data *out)
{
	struct filter_scale_data *data =
		(struct filter_scale_data *)out->data;
	uint24_t *dst = out->buf.u24;
	unsigned int dst_pitch = out->pitch;
	uint24_t *src = in->buf.u24;
	unsigned int src_pitch = in->pitch;
	unsigned int width = in->width;
	unsigned int x_scale = data->x_scale;
	unsigned int y_scale = data->y_scale;
	unsigned int height = in->height;
	unsigned int y;

	for (y = 0; (y != height); ++y) {
		uint24_t *out = dst;
		unsigned int i;
		unsigned int x;

		for (x = 0; (x != width); ++x) {
			uint24_t tmp;

			u24cpy(&tmp, &src[x]);
			for (i = 0; (i != x_scale); ++i)
				u24cpy((out++), &tmp);
		}
		out = dst;
		dst = (uint24_t *)((uint8_t *)dst + dst_pitch);
		for (i = 1; (i < y_scale); ++i) {
			memcpy(dst, out, (width * sizeof(*dst) * x_scale));
			out = dst;
			dst = (uint24_t *)((uint8_t *)dst + dst_pitch);
		}
		src = (uint24_t *)((uint8_t *)src + src_pitch);
	}
}

/**
 * This filter attempts to rescale according to screen X/Y factors.
 * @param in Input buffer data.
 * @param out Output buffer data.
 */
static void filter_scale(const struct filter_data *in,
			 struct filter_data *out)
{
	static const struct {
		unsigned int Bpp;
		filter_func_t *func;
	} scale_mode[] = {
		{ 1, filter_scale_X<uint8_t> },
		{ 2, filter_scale_X<uint16_t> },
		{ 3, filter_scale_3 },
		{ 4, filter_scale_X<uint32_t> },
	};
	struct filter_scale_data *data;
	unsigned int width;
	unsigned int height;
	unsigned int x_off;
	unsigned int y_off;
	unsigned int x_scale;
	unsigned int y_scale;
	filter_func_t *filter;
	unsigned int i;

	if (out->failed == true) {
	failed:
		filter_off(in, out);
		return;
	}
	if (out->updated == true) {
		data = (struct filter_scale_data *)out->data;
		filter = data->filter;
	process:
		// Feed this to the basic scaler.
		(*filter)(in, out);
		return;
	}
	// Initialize filter.
	assert(out->data == NULL);
	x_scale = screen.x_scale;
	y_scale = screen.y_scale;
	while ((width = (in->width * x_scale)) > out->width)
		--x_scale;
	while ((height = (in->height * y_scale)) > out->height)
		--y_scale;
	// Check whether output is large enough.
	if ((x_scale == 0) || (y_scale == 0)) {
		DEBUG(("cannot rescale by %ux%u", x_scale, y_scale));
		out->failed = true;
		goto failed;
	}
	// Not rescaling is faster through filter_off().
	if ((x_scale == 1) && (y_scale == 1)) {
		DEBUG(("using faster fallback for %ux%u", x_scale, y_scale));
		out->failed = true;
		goto failed;
	}
	// Find a suitable filter.
	for (i = 0; (i != elemof(scale_mode)); ++i)
		if (scale_mode[i].Bpp == screen.Bpp)
			break;
	if (i == elemof(scale_mode)) {
		DEBUG(("%u Bpp depth is not supported", screen.Bpp));
		out->failed = true;
		goto failed;
	}
	DEBUG(("using %u Bpp function to scale by %ux%u",
	       screen.Bpp, x_scale, y_scale));
	data = (struct filter_scale_data *)malloc(sizeof(*data));
	if (data == NULL) {
		DEBUG(("allocation failure"));
		out->failed = true;
		goto failed;
	}
	filter = scale_mode[i].func;
	data->filter = filter;
	data->x_scale = x_scale;
	data->y_scale = y_scale;
	// Center output.
	x_off = ((out->width - width) / 2);
	y_off = ((out->height - height) / 2);
	out->buf.u8 += (x_off * screen.Bpp);
	out->buf.u8 += (out->pitch * y_off);
	out->width = width;
	out->height = height;
	out->data = (void *)data;
	out->updated = true;
	goto process;
}

struct filter_stretch_data {
	uint8_t *h_table;
	uint8_t *v_table;
	filter_func_t *filter;
};

template <typename uintX_t>
static void filter_stretch_X(const struct filter_data *in,
			     struct filter_data *out)
{
	struct filter_stretch_data *data =
		(struct filter_stretch_data *)out->data;
	uint8_t *h_table = data->h_table;
	uint8_t *v_table = data->v_table;
	uintX_t *dst = (uintX_t *)out->buf.u8;
	unsigned int dst_pitch = out->pitch;
	unsigned int dst_w = out->width;
	uintX_t *src = (uintX_t *)in->buf.u8;
	unsigned int src_pitch = in->pitch;
	unsigned int src_w = in->width;
	unsigned int src_h = in->height;
	unsigned int src_y;

	dst_pitch /= sizeof(*dst);
	src_pitch /= sizeof(*src);
	for (src_y = 0; (src_y != src_h); ++src_y) {
		uint8_t v_repeat = v_table[src_y];
		unsigned int src_x;
		unsigned int dst_x;

		if (!v_repeat) {
			src += src_pitch;
			continue;
		}
		for (src_x = 0, dst_x = 0; (src_x != src_w); ++src_x) {
			uint8_t h_repeat = h_table[src_x];

			if (!h_repeat)
				continue;
			while (h_repeat--)
				dst[dst_x++] = src[src_x];
		}
		dst += dst_pitch;
		while (--v_repeat) {
			memcpy(dst, (dst - dst_pitch), (dst_w * sizeof(*dst)));
			dst += dst_pitch;
		}
		src += src_pitch;
	}
}

static void filter_stretch_3(const struct filter_data *in,
			     struct filter_data *out)
{
	struct filter_stretch_data *data =
		(struct filter_stretch_data *)out->data;
	uint8_t *h_table = data->h_table;
	uint8_t *v_table = data->v_table;
	uint24_t *dst = out->buf.u24;
	unsigned int dst_pitch = out->pitch;
	unsigned int dst_w = out->width;
	uint24_t *src = in->buf.u24;
	unsigned int src_pitch = in->pitch;
	unsigned int src_w = in->width;
	unsigned int src_h = in->height;
	unsigned int src_y;

	dst_pitch /= sizeof(*dst);
	src_pitch /= sizeof(*src);
	for (src_y = 0; (src_y != src_h); ++src_y) {
		uint8_t v_repeat = v_table[src_y];
		unsigned int src_x;
		unsigned int dst_x;

		if (!v_repeat) {
			src += src_pitch;
			continue;
		}
		for (src_x = 0, dst_x = 0; (src_x != src_w); ++src_x) {
			uint8_t h_repeat = h_table[src_x];

			if (!h_repeat)
				continue;
			while (h_repeat--)
				u24cpy(&dst[dst_x++], &src[src_x]);
		}
		dst += dst_pitch;
		while (--v_repeat) {
			memcpy(dst, (dst - dst_pitch), (dst_w * sizeof(*dst)));
			dst += dst_pitch;
		}
		src += src_pitch;
	}
}

/**
 * This filter stretches the input buffer to fill the entire output.
 * @param in Input buffer data.
 * @param out Output buffer data.
 */
static void filter_stretch(const struct filter_data *in,
			   struct filter_data *out)
{
	static const struct {
		unsigned int Bpp;
		filter_func_t *func;
	} stretch_mode[] = {
		{ 1, filter_stretch_X<uint8_t> },
		{ 2, filter_stretch_X<uint16_t> },
		{ 3, filter_stretch_3 },
		{ 4, filter_stretch_X<uint32_t> },
	};
	struct filter_stretch_data *data;
	unsigned int dst_w;
	unsigned int dst_h;
	unsigned int src_w;
	unsigned int src_h;
	unsigned int h_ratio;
	unsigned int v_ratio;
	unsigned int dst_x;
	unsigned int dst_y;
	unsigned int src_x;
	unsigned int src_y;
	filter_func_t *filter;
	unsigned int i;

	if (out->failed == true) {
	failed:
		filter_off(in, out);
		return;
	}
	if (out->updated == true) {
		data = (struct filter_stretch_data *)out->data;
		filter = data->filter;
	process:
		(*filter)(in, out);
		return;
	}
	// Initialize filter.
	assert(out->data == NULL);
	dst_w = out->width;
	dst_h = out->height;
	src_w = in->width;
	src_h = in->height;
	if ((src_h == 0) || (src_w == 0)) {
		DEBUG(("invalid input size: %ux%u", src_h, src_w));
		out->failed = true;
		goto failed;
	}
	// Make sure input and output pitches are multiples of pixel size
	// at the current depth.
	if ((in->pitch % screen.Bpp) || (out->pitch % screen.Bpp)) {
		DEBUG(("Bpp: %u, in->pitch: %u, out->pitch: %u",
		       screen.Bpp, in->pitch, out->pitch));
		out->failed = true;
		goto failed;
	}
	// Find a suitable filter.
	for (i = 0; (i != elemof(stretch_mode)); ++i)
		if (stretch_mode[i].Bpp == screen.Bpp)
			break;
	if (i == elemof(stretch_mode)) {
		DEBUG(("%u Bpp depth is not supported", screen.Bpp));
		out->failed = true;
		goto failed;
	}
	filter = stretch_mode[i].func;
	// Fix output if original aspect ratio must be kept.
	if (dgen_aspect) {
		unsigned int w = ((dst_h * src_w) / src_h);
		unsigned int h = ((dst_w * src_h) / src_w);

		if (w >= dst_w) {
			w = dst_w;
			if (h == 0)
				++h;
		}
		else {
			h = dst_h;
			if (w == 0)
				++w;
		}
		dst_w = w;
		dst_h = h;
	}
	// Precompute H and V pixel ratios.
	h_ratio = ((dst_w << 10) / src_w);
	v_ratio = ((dst_h << 10) / src_h);
	data = (struct filter_stretch_data *)
		calloc(1, sizeof(*data) + src_w + src_h);
	if (data == NULL) {
		DEBUG(("allocation failure"));
		out->failed = true;
		goto failed;
	}
	DEBUG(("stretching %ux%u to %ux%u/%ux%u (aspect ratio %s)",
	       src_w, src_h, dst_w, dst_h, out->width, out->height,
	       (dgen_aspect ? "must be kept" : "is free")));
	data->h_table = (uint8_t *)(data + 1);
	data->v_table = (data->h_table + src_w);
	data->filter = filter;
	for (dst_x = 0; (dst_x != dst_w); ++dst_x) {
		src_x = ((dst_x << 10) / h_ratio);
		if (src_x < src_w)
			++data->h_table[src_x];
	}
	for (dst_y = 0; (dst_y != dst_h); ++dst_y) {
		src_y = ((dst_y << 10) / v_ratio);
		if (src_y < src_h)
			++data->v_table[src_y];
	}
	// Center output.
	dst_x = ((out->width - dst_w) / 2);
	dst_y = ((out->height - dst_h) / 2);
	out->buf.u8 += (dst_x * screen.Bpp);
	out->buf.u8 += (out->pitch * dst_y);
	out->width = dst_w;
	out->height = dst_h;
	out->data = (void *)data;
	out->updated = true;
	goto process;
}


static bool calibrating = false; //< True during calibration.
static unsigned int calibrating_controller; ///< Controller being calibrated.

static void manage_calibration(enum rc_binding_type type, intptr_t code);

/**
 * Interactively calibrate a controller.
 * If n_args == 1, controller 0 will be configured.
 * If n_args == 2, configure controller in string args[1].
 * @param n_args Number of arguments.
 * @param[in] args List of arguments.
 * @return Status code.
 */
static int
prompt_cmd_calibrate(class md&, unsigned int n_args, const char** args)
{
	/* check args first */
	if (n_args == 1)
		calibrating_controller = 0;
	else if (n_args == 2) {
		calibrating_controller = (atoi(args[1]) - 1);
		if (calibrating_controller > 1)
			return CMD_EINVAL;
	}
	else
		return CMD_EINVAL;
	manage_calibration(RCB_NUM, -1);
	return (CMD_OK | CMD_MSG);
}

/**
 * Initialize screen.
 *
 * @param width Width of display.
 * @param height Height of display.
 * @return 0 on success, -1 if screen could not be initialized with current
 * options but remains in its previous state, -2 if screen is unusable.
 */
static int screen_init(unsigned int width, unsigned int height)
{
	static bool once = true;
	uint32_t flags = SDL_SWSURFACE;
	struct screen scrtmp;

	DEBUG(("want width=%u height=%u", width, height));
	stopped = 1;
	// Copy current screen data.
	memcpy(&scrtmp, &screen, sizeof(scrtmp));
	if (once) 
	{
		unsigned int info_height = 0;

		// Force defaults once.
		scrtmp.window_width = 0;
		scrtmp.window_height = 0;
		scrtmp.width = (video.width );
		scrtmp.height = ((video.height) + info_height);
		scrtmp.x_scale = (scrtmp.width / video.width);
		scrtmp.y_scale = (scrtmp.height / video.height);
		scrtmp.bpp = 16;
		scrtmp.Bpp = 16;
		scrtmp.info_height = info_height;
		scrtmp.buf.u8 = 0;
		scrtmp.pitch = 0;
		scrtmp.surface = 0;
		scrtmp.want_fullscreen = 0;
		scrtmp.is_fullscreen = 0;
		memset(scrtmp.color, 0, sizeof(scrtmp.color));
		once = false;
	}

	// Configure SDL_SetVideoMode().

	// Set video mode.
	DEBUG(("SDL_SetVideoMode(%u, %u, %d, 0x%08x)",
	       scrtmp.width, scrtmp.height, scrtmp.bpp, flags));
	scrtmp.surface = SDL_SetVideoMode(scrtmp.width, scrtmp.height,
					  scrtmp.bpp, flags);
	if (scrtmp.surface == NULL) {
		return -1;
	}
	DEBUG(("SDL_SetVideoMode succeeded"));
	// Update with current values.
	scrtmp.window_width = scrtmp.surface->w;
	scrtmp.window_height = scrtmp.surface->h;
	scrtmp.width = scrtmp.window_width;
	scrtmp.height = scrtmp.window_height;
	
	scrtmp.info_height = 0;

	assert(scrtmp.info_height <= scrtmp.height); // Do not forget.
	// Determine default X and Y scale values from what remains.
	if (dgen_x_scale >= 0)
		scrtmp.x_scale = dgen_x_scale;
	else
		scrtmp.x_scale = (scrtmp.width / video.width);
	if (dgen_y_scale >= 0)
		scrtmp.y_scale = dgen_y_scale;
	else
		scrtmp.y_scale = ((scrtmp.height - scrtmp.info_height) /
				  video.height);
	if (dgen_aspect) {
		if (scrtmp.x_scale >= scrtmp.y_scale)
			scrtmp.x_scale = scrtmp.y_scale;
		else
			scrtmp.y_scale = scrtmp.x_scale;
	}
	
	// Fix bpp.
	assert(scrtmp.surface->format != NULL);
	scrtmp.bpp = scrtmp.surface->format->BitsPerPixel;
	
	// 15 bpp has be forced if it was required. SDL does not return the
	// right value.
	
	if ((dgen_depth == 15) && (scrtmp.bpp == 16))
		scrtmp.bpp = 15;
		
	scrtmp.Bpp = scrtmp.surface->format->BytesPerPixel;
	scrtmp.buf.u8 = (uint8_t *)scrtmp.surface->pixels;
	scrtmp.pitch = scrtmp.surface->pitch;
	scrtmp.is_fullscreen = scrtmp.want_fullscreen;
	DEBUG(("video configuration: x_scale=%u y_scale=%u",
	       scrtmp.x_scale, scrtmp.y_scale));
	DEBUG(("screen configuration: width=%u height=%u bpp=%u Bpp=%u"
	       " info_height=%u"
	       " buf.u8=%p pitch=%u surface=%p want_fullscreen=%u"
	       " is_fullscreen=%u",
	       scrtmp.width, scrtmp.height, scrtmp.bpp, scrtmp.Bpp,
	       scrtmp.info_height,
	       (void *)scrtmp.buf.u8, scrtmp.pitch, (void *)scrtmp.surface,
	       scrtmp.want_fullscreen, scrtmp.is_fullscreen));
	// Screen is now initialized, update data.
	screen = scrtmp;
	// Set up the Mega Drive screen.
	// Could not be done earlier because bpp was unknown.
	if ((mdscr.data == NULL) ||
	    ((unsigned int)mdscr.bpp != screen.bpp) ||
	    ((unsigned int)mdscr.w != (video.width + 16)) ||
	    ((unsigned int)mdscr.h != (video.height + 16))) {
		mdscr.w = (video.width + 16);
		mdscr.h = (video.height + 16);
		mdscr.pitch = (mdscr.w * screen.Bpp);
		mdscr.bpp = screen.bpp;
		free(mdscr.data);
		mdscr.data = (uint8_t *)calloc(mdscr.h, mdscr.pitch);
		if (mdscr.data == NULL) {
			// Cannot recover. Clean up and bail out.
			memset(&mdscr, 0, sizeof(mdscr));
			return -2;
		}
	}
	DEBUG(("md screen configuration: w=%d h=%d bpp=%d pitch=%d data=%p",
	       mdscr.w, mdscr.h, mdscr.bpp, mdscr.pitch, (void *)mdscr.data));
	// If we're in 8 bit mode, set color 0xff to white for the text,
	// and make a palette buffer.
	if (screen.bpp == 8) {
		SDL_Color color = { 0xff, 0xff, 0xff, 0x00 };

		SDL_SetColors(screen.surface, &color, 0xff, 1);
		memset(video.palette, 0x00, sizeof(video.palette));
		mdpal = video.palette;
	}
	else
		mdpal = NULL;
	// Rehash filters.
	filters_stack_update();
	// Update screen.
	pd_graphics_update(true);
	return 0;
}

/**
 * Initialize SDL, and the graphics.
 * @param want_sound Nonzero if we want sound.
 * @param want_pal Nonzero for PAL mode.
 * @param hz Requested frame rate (between 0 and 1000).
 * @return Nonzero if successful.
 */
int pd_graphics_init(int want_sound, int want_pal, int hz)
{
	SDL_Event event;

	if ((hz <= 0) || (hz > 1000)) {
		// You may as well disable bool_frameskip.
		fprintf(stderr, "sdl: invalid frame rate (%d)\n", hz);
		return 0;
	}
	video.hz = hz;
	if (want_pal) {
		// PAL
		video.is_pal = 1;
		video.height = 240;
	}
	else {
		// NTSC
		video.is_pal = 0;
		video.height = 224;
	}
#ifndef __MINGW32__
	// [fbcon workaround]
	// Disable SDL_FBACCEL (if unset) before calling SDL_Init() in case
	// fbcon is to be used. Prevents SDL_FillRect() and SDL_SetVideoMode()
	// from hanging when hardware acceleration is available.
	setenv("SDL_FBACCEL", "0", 0);
	// [fbcon workaround]
	// A mouse is never required.
	setenv("SDL_NOMOUSE", "1", 0);
#endif
	if (SDL_Init(SDL_INIT_VIDEO)) 
	{
		fprintf(stderr, "sdl: can't init SDL: %s\n", SDL_GetError());
		return 0;
	}
#ifndef __MINGW32__
	{
		char buf[32];

		// [fbcon workaround]
		// Double buffering usually makes screen blink during refresh.
		if ((SDL_VideoDriverName(buf, sizeof(buf))) &&
		    (!strcmp(buf, "fbcon")))
			dgen_doublebuffer = 0;
	}
#endif
	// Hide the cursor.
	SDL_ShowCursor(0);
	// Initialize screen.
	if (screen_init(0, 0))
		goto fail;
		
	DEBUG(("screen initialized"));/*
#ifndef __MINGW32__
	// We don't need setuid privileges anymore
	if (getuid() != geteuid())
		setuid(getuid());
	DEBUG(("setuid privileges dropped"));
#endif*/
	DEBUG(("ret=1"));
	fprintf(stderr, "video: %dx%d, %u bpp (%u Bpp), %uHz\n",
		screen.surface->w, screen.surface->h, screen.bpp,
		screen.Bpp, video.hz);
	return 1;
fail:
	fprintf(stderr, "sdl: can't initialize graphics.\n");
	return 0;
}

/**
 * Reinitialize graphics.
 * @param want_pal Nonzero for PAL mode.
 * @param hz Requested frame rate (between 0 and 1000).
 * @return Nonzero if successful.
 */
int pd_graphics_reinit(int, int want_pal, int hz)
{
	if ((hz <= 0) || (hz > 1000)) {
		// You may as well disable bool_frameskip.
		fprintf(stderr, "sdl: invalid frame rate (%d)\n", hz);
		return 0;
	}
	video.hz = hz;
	if (want_pal) {
		// PAL
		video.is_pal = 1;
		video.height = 240;
	}
	else {
		// NTSC
		video.is_pal = 0;
		video.height = 224;
	}
	// Reinitialize screen.
	if (screen_init(screen.window_width, screen.window_height))
		goto fail;
	DEBUG(("screen reinitialized"));
	return 1;
fail:
	fprintf(stderr, "sdl: can't reinitialize graphics.\n");
	return 0;
}

/**
 * Update palette.
 */
void pd_graphics_palette_update()
{
	unsigned int i;

	for (i = 0; (i < 64); ++i) {
		screen.color[i].r = mdpal[(i << 2)];
		screen.color[i].g = mdpal[((i << 2) + 1)];
		screen.color[i].b = mdpal[((i << 2) + 2)];
	}
		SDL_SetColors(screen.surface, screen.color, 0, 64);
}

/**
 * Display screen.
 * @param update False if screen buffer is garbage and must be updated first.
 */
void pd_graphics_update(bool update)
{
	static unsigned long fps_since = 0;
	static unsigned long frames_old = 0;
	static unsigned long frames = 0;
	const struct filter *f;
	struct filter_data *fd;
	size_t i;

	++frames;

	// Process output through filters.
	for (i = 0; (i != elemof(filters_stack)); ++i) 
	{
		f = filters_stack[i];
		fd = &filters_stack_data[i];
		if ((filters_stack_size == 0) ||
		    (i == (filters_stack_size - 1)))
			break;
		f->func(fd, (fd + 1));
	}
	
	// Lock screen.
	screen_lock();
	// Generate screen output with the last filter.
	f->func(fd, (fd + 1));
	// Unlock screen.
	screen_unlock();
	// Update the screen.
	screen_update();
}

/**
 * Callback for sound.
 * @param stream Sound destination buffer.
 * @param len Length of destination buffer.
 */
static void snd_callback(void *, Uint8 *stream, int len)
{
	size_t wrote;

	// Slurp off the play buffer
	wrote = cbuf_read(stream, &sound.cbuf, len);
	if (wrote == (size_t)len)
		return;
	// Not enough data, fill remaining space with silence.
	memset(&stream[wrote], 0, ((size_t)len - wrote));
}

/**
 * Initialize the sound.
 * @param freq Sound samples rate.
 * @param[in,out] samples Minimum buffer size in samples.
 * @return Nonzero on success.
 */
int pd_sound_init(long &freq, unsigned int &samples)
{
#ifndef NOSOUND
	SDL_AudioSpec wanted;
	SDL_AudioSpec spec;

	// Clean up first.
	pd_sound_deinit();

	// Set the desired format
	wanted.freq = freq;
#ifdef WORDS_BIGENDIAN
	wanted.format = AUDIO_S16MSB;
#else
	wanted.format = AUDIO_S16LSB;
#endif
	wanted.channels = 2;
	wanted.samples = dgen_soundsamples;
	wanted.callback = snd_callback;
	wanted.userdata = NULL;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		fprintf(stderr, "sdl: unable to initialize audio\n");
		return 0;
	}

	// Open audio, and get the real spec
	if (SDL_OpenAudio(&wanted, &spec) < 0) {
		fprintf(stderr,
			"sdl: couldn't open audio: %s\n",
			SDL_GetError());
		return 0;
	}

	// Check everything
	if (spec.channels != 2) {
		fprintf(stderr, "sdl: couldn't get stereo audio format.\n");
		goto snd_error;
	}
	if (spec.format != wanted.format) {
		fprintf(stderr, "sdl: unable to get 16-bit audio.\n");
		goto snd_error;
	}

	// Set things as they really are
	sound.rate = freq = spec.freq;
	sndi.len = (spec.freq / video.hz);
	sound.samples = spec.samples;
	samples += sound.samples;

	// Calculate buffer size (sample size = (channels * (bits / 8))).
	sound.cbuf.size = (samples * (2 * (16 / 8)));
	sound.cbuf.i = 0;
	sound.cbuf.s = 0;

	fprintf(stderr, "sound: %uHz, %d samples, buffer: %u bytes\n",
		sound.rate, spec.samples, (unsigned int)sound.cbuf.size);

	// Allocate zero-filled play buffer.
	sndi.lr = (int16_t *)calloc(2, (sndi.len * sizeof(sndi.lr[0])));

	sound.cbuf.data.i16 = (int16_t *)calloc(1, sound.cbuf.size);
	if ((sndi.lr == NULL) || (sound.cbuf.data.i16 == NULL)) {
		fprintf(stderr, "sdl: couldn't allocate sound buffers.\n");
		goto snd_error;
	}

	// Start sound output.
	SDL_PauseAudio(0);

	// It's all good!
	return 1;

snd_error:
	// Oops! Something bad happened, cleanup.
	SDL_CloseAudio();
	free((void *)sndi.lr);
	sndi.lr = NULL;
	sndi.len = 0;
	free((void *)sound.cbuf.data.i16);
	sound.cbuf.data.i16 = NULL;
	memset(&sound, 0, sizeof(sound));
	return 0;
#else
	return 0;
#endif
}

/**
 * Deinitialize sound subsystem.
 */
void pd_sound_deinit()
{
	if (sound.cbuf.data.i16 != NULL) {
		SDL_PauseAudio(1);
		SDL_CloseAudio();
		free((void *)sound.cbuf.data.i16);
	}
	memset(&sound, 0, sizeof(sound));
	free((void*)sndi.lr);
	sndi.lr = NULL;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/**
 * Return samples read/write indices in the buffer.
 */
unsigned int pd_sound_rp()
{
	unsigned int ret;

	if (!sound.cbuf.size)
		return 0;
	SDL_LockAudio();
	ret = sound.cbuf.i;
	SDL_UnlockAudio();
	return (ret >> 2);
}

unsigned int pd_sound_wp()
{
	unsigned int ret;

	if (!sound.cbuf.size)
		return 0;
	SDL_LockAudio();
	ret = ((sound.cbuf.i + sound.cbuf.s) % sound.cbuf.size);
	SDL_UnlockAudio();
	return (ret >> 2);
}

/**
 * Write contents of sndi to sound.cbuf.
 */
void pd_sound_write()
{
	if (!sound.cbuf.size)
		return;
	SDL_LockAudio();
	cbuf_write(&sound.cbuf, (uint8_t *)sndi.lr, (sndi.len * 4));
	SDL_UnlockAudio();
}

/**
 * Tells whether DGen stopped intentionally so emulation can resume without
 * skipping frames.
 */
int pd_stopped()
{
	int ret = stopped;

	stopped = 0;
	return ret;
}

/**
 * Keyboard input.
 */
typedef struct {
	char *buf;
	size_t pos;
	size_t size;
} kb_input_t;

/**
 * Keyboard input results.
 */
enum kb_input {
	KB_INPUT_ABORTED,
	KB_INPUT_ENTERED,
	KB_INPUT_CONSUMED,
	KB_INPUT_IGNORED
};

/**
 * Manage text input with some rudimentary history.
 * @param input Input buffer.
 * @param ksym Keyboard symbol.
 * @param ksym_uni Unicode translation for keyboard symbol.
 * @return Input result.
 */
static enum kb_input kb_input(kb_input_t *input, uint32_t ksym,
			      uint16_t ksym_uni)
{
#define HISTORY_LEN 32
	static char history[HISTORY_LEN][64];
	static int history_pos = -1;
	static int history_len = 0;
	char c;

	if (ksym & KEYSYM_MOD_CTRL)
		return KB_INPUT_IGNORED;
	if (isprint((c = ksym_uni))) {
		if (input->pos >= (input->size - 1))
			return KB_INPUT_CONSUMED;
		if (input->buf[input->pos] == '\0')
			input->buf[(input->pos + 1)] = '\0';
		input->buf[input->pos] = c;
		++input->pos;
		return KB_INPUT_CONSUMED;
	}
	else if (ksym == SDLK_DELETE) {
		size_t tail;

		if (input->buf[input->pos] == '\0')
			return KB_INPUT_CONSUMED;
		tail = ((input->size - input->pos) + 1);
		memmove(&input->buf[input->pos],
			&input->buf[(input->pos + 1)],
			tail);
		return KB_INPUT_CONSUMED;
	}
	else if (ksym == SDLK_BACKSPACE) {
		size_t tail;

		if (input->pos == 0)
			return KB_INPUT_CONSUMED;
		--input->pos;
		tail = ((input->size - input->pos) + 1);
		memmove(&input->buf[input->pos],
			&input->buf[(input->pos + 1)],
			tail);
		return KB_INPUT_CONSUMED;
	}
	else if (ksym == SDLK_LEFT) {
		if (input->pos != 0)
			--input->pos;
		return KB_INPUT_CONSUMED;
	}
	else if (ksym == SDLK_RIGHT) {
		if (input->buf[input->pos] != '\0')
			++input->pos;
		return KB_INPUT_CONSUMED;
	}
	else if ((ksym == SDLK_RETURN) || (ksym == SDLK_KP_ENTER)) {
		history_pos = -1;
		if (input->pos == 0)
			return KB_INPUT_ABORTED;
		if (history_len < HISTORY_LEN)
			++history_len;
		memmove(&history[1], &history[0],
			((history_len - 1) * sizeof(history[0])));
		strncpy(history[0], input->buf, sizeof(history[0]));
		return KB_INPUT_ENTERED;
	}
	else if (ksym == SDLK_ESCAPE) {
		history_pos = 0;
		return KB_INPUT_ABORTED;
	}
	else if (ksym == SDLK_UP) {
		if (input->size == 0)
			return KB_INPUT_CONSUMED;
		if (history_pos < (history_len - 1))
			++history_pos;
		strncpy(input->buf, history[history_pos], input->size);
		input->buf[(input->size - 1)] = '\0';
		input->pos = strlen(input->buf);
		return KB_INPUT_CONSUMED;
	}
	else if (ksym == SDLK_DOWN) {
		if ((input->size == 0) || (history_pos < 0))
			return KB_INPUT_CONSUMED;
		if (history_pos > 0)
			--history_pos;
		strncpy(input->buf, history[history_pos], input->size);
		input->buf[(input->size - 1)] = '\0';
		input->pos = strlen(input->buf);
		return KB_INPUT_CONSUMED;
	}
	return KB_INPUT_IGNORED;
}


// Controls enum. You must add new entries at the end. Do not change the order.
enum ctl_e {
	CTL_PAD1_UP,
	CTL_PAD1_DOWN,
	CTL_PAD1_LEFT,
	CTL_PAD1_RIGHT,
	CTL_PAD1_A,
	CTL_PAD1_B,
	CTL_PAD1_C,
	CTL_PAD1_X,
	CTL_PAD1_Y,
	CTL_PAD1_Z,
	CTL_PAD1_MODE,
	CTL_PAD1_START,
	CTL_PAD2_UP,
	CTL_PAD2_DOWN,
	CTL_PAD2_LEFT,
	CTL_PAD2_RIGHT,
	CTL_PAD2_A,
	CTL_PAD2_B,
	CTL_PAD2_C,
	CTL_PAD2_X,
	CTL_PAD2_Y,
	CTL_PAD2_Z,
	CTL_PAD2_MODE,
	CTL_PAD2_START,
#ifdef WITH_PICO
	CTL_PICO_PEN_UP,
	CTL_PICO_PEN_DOWN,
	CTL_PICO_PEN_LEFT,
	CTL_PICO_PEN_RIGHT,
	CTL_PICO_PEN_BUTTON,
#endif
	CTL_DGEN_QUIT,
	CTL_DGEN_CRAPTV_TOGGLE,
	CTL_DGEN_SCALING_TOGGLE,
	CTL_DGEN_RESET,
	CTL_DGEN_SLOT0,
	CTL_DGEN_SLOT1,
	CTL_DGEN_SLOT2,
	CTL_DGEN_SLOT3,
	CTL_DGEN_SLOT4,
	CTL_DGEN_SLOT5,
	CTL_DGEN_SLOT6,
	CTL_DGEN_SLOT7,
	CTL_DGEN_SLOT8,
	CTL_DGEN_SLOT9,
	CTL_DGEN_SLOT_NEXT,
	CTL_DGEN_SLOT_PREV,
	CTL_DGEN_SAVE,
	CTL_DGEN_LOAD,
	CTL_DGEN_Z80_TOGGLE,
	CTL_DGEN_CPU_TOGGLE,
	CTL_DGEN_STOP,
	CTL_DGEN_PROMPT,
	CTL_DGEN_GAME_GENIE,
	CTL_DGEN_VOLUME_INC,
	CTL_DGEN_VOLUME_DEC,
	CTL_DGEN_FULLSCREEN_TOGGLE,
	CTL_DGEN_FIX_CHECKSUM,
	CTL_DGEN_SCREENSHOT,
	CTL_DGEN_DEBUG_ENTER,
	CTL_
};

// Controls definitions.
struct ctl {
	const enum ctl_e type;
	intptr_t (*const rc)[RCB_NUM];
	int (*const press)(struct ctl&, md&);
	int (*const release)(struct ctl&, md&);
#define DEF 0, 0, 0, 0
	unsigned int pressed:1;
	unsigned int coord:1;
	unsigned int x:10;
	unsigned int y:10;
};

static int ctl_pad1(struct ctl& ctl, md& megad)
{
	switch (ctl.type) {
	case CTL_PAD1_UP:
		megad.pad[0] &= ~MD_UP_MASK;
		break;
	case CTL_PAD1_DOWN:
		megad.pad[0] &= ~MD_DOWN_MASK;
		break;
	case CTL_PAD1_LEFT:
		megad.pad[0] &= ~MD_LEFT_MASK;
		break;
	case CTL_PAD1_RIGHT:
		megad.pad[0] &= ~MD_RIGHT_MASK;
		break;
	case CTL_PAD1_A:
		megad.pad[0] &= ~MD_A_MASK;
		break;
	case CTL_PAD1_B:
		megad.pad[0] &= ~MD_B_MASK;
		break;
	case CTL_PAD1_C:
		megad.pad[0] &= ~MD_C_MASK;
		break;
	case CTL_PAD1_X:
		megad.pad[0] &= ~MD_X_MASK;
		break;
	case CTL_PAD1_Y:
		megad.pad[0] &= ~MD_Y_MASK;
		break;
	case CTL_PAD1_Z:
		megad.pad[0] &= ~MD_Z_MASK;
		break;
	case CTL_PAD1_MODE:
		megad.pad[0] &= ~MD_MODE_MASK;
		break;
	case CTL_PAD1_START:
		megad.pad[0] &= ~MD_START_MASK;
		break;
	default:
		break;
	}
	return 1;
}

static int ctl_pad1_release(struct ctl& ctl, md& megad)
{
	switch (ctl.type) {
	case CTL_PAD1_UP:
		megad.pad[0] |= MD_UP_MASK;
		break;
	case CTL_PAD1_DOWN:
		megad.pad[0] |= MD_DOWN_MASK;
		break;
	case CTL_PAD1_LEFT:
		megad.pad[0] |= MD_LEFT_MASK;
		break;
	case CTL_PAD1_RIGHT:
		megad.pad[0] |= MD_RIGHT_MASK;
		break;
	case CTL_PAD1_A:
		megad.pad[0] |= MD_A_MASK;
		break;
	case CTL_PAD1_B:
		megad.pad[0] |= MD_B_MASK;
		break;
	case CTL_PAD1_C:
		megad.pad[0] |= MD_C_MASK;
		break;
	case CTL_PAD1_X:
		megad.pad[0] |= MD_X_MASK;
		break;
	case CTL_PAD1_Y:
		megad.pad[0] |= MD_Y_MASK;
		break;
	case CTL_PAD1_Z:
		megad.pad[0] |= MD_Z_MASK;
		break;
	case CTL_PAD1_MODE:
		megad.pad[0] |= MD_MODE_MASK;
		break;
	case CTL_PAD1_START:
		megad.pad[0] |= MD_START_MASK;
		break;
	default:
		break;
	}
	return 1;
}

static int ctl_pad2(struct ctl& ctl, md& megad)
{
	switch (ctl.type) {
	case CTL_PAD2_UP:
		megad.pad[1] &= ~MD_UP_MASK;
		break;
	case CTL_PAD2_DOWN:
		megad.pad[1] &= ~MD_DOWN_MASK;
		break;
	case CTL_PAD2_LEFT:
		megad.pad[1] &= ~MD_LEFT_MASK;
		break;
	case CTL_PAD2_RIGHT:
		megad.pad[1] &= ~MD_RIGHT_MASK;
		break;
	case CTL_PAD2_A:
		megad.pad[1] &= ~MD_A_MASK;
		break;
	case CTL_PAD2_B:
		megad.pad[1] &= ~MD_B_MASK;
		break;
	case CTL_PAD2_C:
		megad.pad[1] &= ~MD_C_MASK;
		break;
	case CTL_PAD2_X:
		megad.pad[1] &= ~MD_X_MASK;
		break;
	case CTL_PAD2_Y:
		megad.pad[1] &= ~MD_Y_MASK;
		break;
	case CTL_PAD2_Z:
		megad.pad[1] &= ~MD_Z_MASK;
		break;
	case CTL_PAD2_MODE:
		megad.pad[1] &= ~MD_MODE_MASK;
		break;
	case CTL_PAD2_START:
		megad.pad[1] &= ~MD_START_MASK;
		break;
	default:
		break;
	}
	return 1;
}

static int ctl_pad2_release(struct ctl& ctl, md& megad)
{
	switch (ctl.type) {
	case CTL_PAD2_UP:
		megad.pad[1] |= MD_UP_MASK;
		break;
	case CTL_PAD2_DOWN:
		megad.pad[1] |= MD_DOWN_MASK;
		break;
	case CTL_PAD2_LEFT:
		megad.pad[1] |= MD_LEFT_MASK;
		break;
	case CTL_PAD2_RIGHT:
		megad.pad[1] |= MD_RIGHT_MASK;
		break;
	case CTL_PAD2_A:
		megad.pad[1] |= MD_A_MASK;
		break;
	case CTL_PAD2_B:
		megad.pad[1] |= MD_B_MASK;
		break;
	case CTL_PAD2_C:
		megad.pad[1] |= MD_C_MASK;
		break;
	case CTL_PAD2_X:
		megad.pad[1] |= MD_X_MASK;
		break;
	case CTL_PAD2_Y:
		megad.pad[1] |= MD_Y_MASK;
		break;
	case CTL_PAD2_Z:
		megad.pad[1] |= MD_Z_MASK;
		break;
	case CTL_PAD2_MODE:
		megad.pad[1] |= MD_MODE_MASK;
		break;
	case CTL_PAD2_START:
		megad.pad[1] |= MD_START_MASK;
		break;
	default:
		break;
	}
	return 1;
}

#ifdef WITH_PICO

static int ctl_pico_pen(struct ctl& ctl, md& megad)
{
	static unsigned int min_y = 0x1fc;
	static unsigned int max_y = 0x2f7;
	static unsigned int min_x = 0x3c;
	static unsigned int max_x = 0x17c;
	static const struct {
		enum ctl_e type;
		unsigned int coords:1;
		unsigned int dir:1;
		unsigned int lim[2];
	} motion[] = {
		{ CTL_PICO_PEN_UP, 1, 0, { min_y, max_y } },
		{ CTL_PICO_PEN_DOWN, 1, 1, { min_y, max_y } },
		{ CTL_PICO_PEN_LEFT, 0, 0, { min_x, max_x } },
		{ CTL_PICO_PEN_RIGHT, 0, 1, { min_x, max_x } }
	};
	unsigned int i;

	if (ctl.type == CTL_PICO_PEN_BUTTON) {
		megad.pad[0] &= ~MD_PICO_PENBTN_MASK;
		return 1;
	}
	// Use coordinates if available.
	if ((ctl.coord) &&
	    (screen.window_width != 0) &&
	    (screen.window_height != 0)) {
		megad.pico_pen_coords[1] =
			(min_y + ((ctl.y * (max_y - min_y)) /
				  screen.window_height));
		megad.pico_pen_coords[0] =
			(min_x + ((ctl.x * (max_x - min_x)) /
				  screen.window_width));
		return 1;
	}
	for (i = 0; (i != elemof(motion)); ++i) {
		unsigned int coords;

		if (motion[i].type != ctl.type)
			continue;
		coords = motion[i].coords;
		if (motion[i].dir)
			megad.pico_pen_coords[coords] += pico_pen_stride;
		else
			megad.pico_pen_coords[coords] -= pico_pen_stride;
		if ((megad.pico_pen_coords[coords] < motion[i].lim[0]) ||
		    (megad.pico_pen_coords[coords] > motion[i].lim[1]))
			megad.pico_pen_coords[coords] =
				motion[i].lim[motion[i].dir];
		break;
	}
	return 1;
}

static int ctl_pico_pen_release(struct ctl& ctl, md& megad)
{
	if (ctl.type == CTL_PICO_PEN_BUTTON)
		megad.pad[0] |= MD_PICO_PENBTN_MASK;
	return 1;
}

#endif

static int ctl_dgen_quit(struct ctl&, md&)
{
	return 0;
}

static int ctl_dgen_reset(struct ctl&, md& megad)
{
	megad.reset();
	return 1;
}

static int ctl_dgen_slot(struct ctl& ctl, md&)
{
	slot = ((int)ctl.type - CTL_DGEN_SLOT0);
	return 1;
}

static int ctl_dgen_slot_next(struct ctl&, md&)
{
	if (slot == 9)
		slot = 0;
	else
		slot++;
	return 1;
}

static int ctl_dgen_slot_prev(struct ctl&, md&)
{
	if (slot == 0)
		slot = 9;
	else
		slot--;
	return 1;
}

static int ctl_dgen_save(struct ctl&, md& megad)
{
	md_save(megad);
	return 1;
}

static int ctl_dgen_load(struct ctl&, md& megad)
{
	md_load(megad);
	return 1;
}

// Cycle Z80 core.
static int ctl_dgen_z80_toggle(struct ctl&, md& megad)
{
	const char *msg;

	megad.cycle_z80();
	switch (megad.z80_core) {
#ifdef WITH_CZ80
	case md::Z80_CORE_CZ80:
		msg = "CZ80 core activated.";
		break;
#endif
#ifdef WITH_MZ80
	case md::Z80_CORE_MZ80:
		msg = "MZ80 core activated.";
		break;
#endif
#ifdef WITH_DRZ80
	case md::Z80_CORE_DRZ80:
		msg = "DrZ80 core activated.";
		break;
#endif
	default:
		msg = "Z80 core disabled.";
		break;
	}
	return 1;
}

// Added this CPU core hot swap.  Compile both Musashi and StarScream
// in, and swap on the fly like DirectX DGen. [PKH]
static int ctl_dgen_cpu_toggle(struct ctl&, md& megad)
{
	const char *msg;

	megad.cycle_cpu();
	switch (megad.cpu_emu) {
#ifdef WITH_STAR
	case md::CPU_EMU_STAR:
		msg = "StarScream CPU core activated.";
		break;
#endif
#ifdef WITH_MUSA
	case md::CPU_EMU_MUSA:
		msg = "Musashi CPU core activated.";
		break;
#endif
#ifdef WITH_CYCLONE
	case md::CPU_EMU_CYCLONE:
		msg = "Cyclone CPU core activated.";
		break;
#endif
	default:
		msg = "CPU core disabled.";
		break;
	}
	return 1;
}

static int ctl_dgen_stop(struct ctl&, md& megad)
{
	if (stop_events(megad, STOPPED) != 0)
		return 0;
	return 1;
}


static int ctl_dgen_game_genie(struct ctl&, md& megad)
{
	if (stop_events(megad, GAME_GENIE) != 0)
		return 0;
	return 1;
}

static int ctl_dgen_volume(struct ctl& ctl, md&)
{
	if (ctl.type == CTL_DGEN_VOLUME_INC)
		++dgen_volume;
	else
		--dgen_volume;
	if (dgen_volume < 0)
		dgen_volume = 0;
	else if (dgen_volume > 100)
		dgen_volume = 100;
	return 1;
}

static int ctl_dgen_fix_checksum(struct ctl&, md& megad)
{
	megad.fix_rom_checksum();
	return 1;
}

static int ctl_dgen_screenshot(struct ctl&, md& megad)
{
	do_screenshot(megad);
	return 1;
}

static int ctl_dgen_debug_enter(struct ctl&, md& megad)
{
	(void)megad;
	return 1;
}

static struct ctl control[] = {
	// Array indices and control[].type must match enum ctl_e's order.
	{ CTL_PAD1_UP, &pad1_up, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_DOWN, &pad1_down, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_LEFT, &pad1_left, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_RIGHT, &pad1_right, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_A, &pad1_a, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_B, &pad1_b, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_C, &pad1_c, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_X, &pad1_x, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_Y, &pad1_y, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_Z, &pad1_z, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_MODE, &pad1_mode, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD1_START, &pad1_start, ctl_pad1, ctl_pad1_release, DEF },
	{ CTL_PAD2_UP, &pad2_up, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_DOWN, &pad2_down, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_LEFT, &pad2_left, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_RIGHT, &pad2_right, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_A, &pad2_a, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_B, &pad2_b, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_C, &pad2_c, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_X, &pad2_x, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_Y, &pad2_y, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_Z, &pad2_z, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_MODE, &pad2_mode, ctl_pad2, ctl_pad2_release, DEF },
	{ CTL_PAD2_START, &pad2_start, ctl_pad2, ctl_pad2_release, DEF },
#ifdef WITH_PICO
	{ CTL_PICO_PEN_UP,
	  &pico_pen_up, ctl_pico_pen, ctl_pico_pen_release, DEF },
	{ CTL_PICO_PEN_DOWN,
	  &pico_pen_down, ctl_pico_pen, ctl_pico_pen_release, DEF },
	{ CTL_PICO_PEN_LEFT,
	  &pico_pen_left, ctl_pico_pen, ctl_pico_pen_release, DEF },
	{ CTL_PICO_PEN_RIGHT,
	  &pico_pen_right, ctl_pico_pen, ctl_pico_pen_release, DEF },
	{ CTL_PICO_PEN_BUTTON,
	  &pico_pen_button, ctl_pico_pen, ctl_pico_pen_release, DEF },
#endif
	{ CTL_DGEN_QUIT, &dgen_quit, ctl_dgen_quit, NULL, DEF },
	{ CTL_DGEN_RESET, &dgen_reset, ctl_dgen_reset, NULL, DEF },
	{ CTL_DGEN_SLOT0, &dgen_slot_0, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT1, &dgen_slot_1, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT2, &dgen_slot_2, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT3, &dgen_slot_3, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT4, &dgen_slot_4, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT5, &dgen_slot_5, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT6, &dgen_slot_6, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT7, &dgen_slot_7, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT8, &dgen_slot_8, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT9, &dgen_slot_9, ctl_dgen_slot, NULL, DEF },
	{ CTL_DGEN_SLOT_NEXT, &dgen_slot_next, ctl_dgen_slot_next, NULL, DEF },
	{ CTL_DGEN_SLOT_PREV, &dgen_slot_prev, ctl_dgen_slot_prev, NULL, DEF },
	{ CTL_DGEN_SAVE, &dgen_save, ctl_dgen_save, NULL, DEF },
	{ CTL_DGEN_LOAD, &dgen_load, ctl_dgen_load, NULL, DEF },
	{ CTL_DGEN_Z80_TOGGLE,
	  &dgen_z80_toggle, ctl_dgen_z80_toggle, NULL, DEF },
	{ CTL_DGEN_CPU_TOGGLE,
	  &dgen_cpu_toggle, ctl_dgen_cpu_toggle, NULL, DEF },
	{ CTL_DGEN_STOP, &dgen_stop, ctl_dgen_stop, NULL, DEF },
	{ CTL_DGEN_GAME_GENIE,
	  &dgen_game_genie, ctl_dgen_game_genie, NULL, DEF },
	{ CTL_DGEN_VOLUME_INC,
	  &dgen_volume_inc, ctl_dgen_volume, NULL, DEF },
	{ CTL_DGEN_VOLUME_DEC,
	  &dgen_volume_dec, ctl_dgen_volume, NULL, DEF },
	{ CTL_DGEN_FIX_CHECKSUM,
	  &dgen_fix_checksum, ctl_dgen_fix_checksum, NULL, DEF },
	{ CTL_DGEN_SCREENSHOT,
	  &dgen_screenshot, ctl_dgen_screenshot, NULL, DEF },
	{ CTL_DGEN_DEBUG_ENTER,
	  &dgen_debug_enter, ctl_dgen_debug_enter, NULL, DEF },
	{ CTL_, NULL, NULL, NULL, DEF }
};

static struct {
	char const* name; ///< Controller button name.
	enum ctl_e const id[2]; ///< Controls indices in control[].
	bool once; ///< If button has been pressed once.
	bool twice; ///< If button has been pressed twice.
	enum rc_binding_type type; ///< Type of code.
	intptr_t code; ///< Temporary code.
} calibration_steps[] = {
	{ "START", { CTL_PAD1_START, CTL_PAD2_START },
	  false, false, RCB_NUM, -1 },
	{ "MODE", { CTL_PAD1_MODE, CTL_PAD2_MODE },
	  false, false, RCB_NUM, -1 },
	{ "A", { CTL_PAD1_A, CTL_PAD2_A },
	  false, false, RCB_NUM, -1 },
	{ "B", { CTL_PAD1_B, CTL_PAD2_B },
	  false, false, RCB_NUM, -1 },
	{ "C", { CTL_PAD1_C, CTL_PAD2_C },
	  false, false, RCB_NUM, -1 },
	{ "X", { CTL_PAD1_X, CTL_PAD2_X },
	  false, false, RCB_NUM, -1 },
	{ "Y", { CTL_PAD1_Y, CTL_PAD2_Y },
	  false, false, RCB_NUM, -1 },
	{ "Z", { CTL_PAD1_Z, CTL_PAD2_Z },
	  false, false, RCB_NUM, -1 },
	{ "UP", { CTL_PAD1_UP, CTL_PAD2_UP },
	  false, false, RCB_NUM, -1 },
	{ "DOWN", { CTL_PAD1_DOWN, CTL_PAD2_DOWN },
	  false, false, RCB_NUM, -1 },
	{ "LEFT", { CTL_PAD1_LEFT, CTL_PAD2_LEFT },
	  false, false, RCB_NUM, -1 },
	{ "RIGHT", { CTL_PAD1_RIGHT, CTL_PAD2_RIGHT },
	  false, false, RCB_NUM, -1 },
	{ NULL, { CTL_, CTL_ },
	  false, false, RCB_NUM, -1 }
};

/**
 * Handle input during calibration process.
 * @param type Type of code.
 * @param code Code to process.
 */
static void manage_calibration(enum rc_binding_type type, intptr_t code)
{
	unsigned int step = 0;

	assert(calibrating_controller < 2);
	if (!calibrating) {
		// Stop emulation, enter calibration mode.
		freeze(true);
		calibrating = true;
		goto ask;
	}
	while (step != elemof(calibration_steps))
		if ((calibration_steps[step].once == true) &&
		    (calibration_steps[step].twice == true))
			++step;
		else
			break;
	if (step == elemof(calibration_steps)) {
		// Reset everything.
		for (step = 0; (step != elemof(calibration_steps)); ++step) {
			calibration_steps[step].once = false;
			calibration_steps[step].twice = false;
			calibration_steps[step].type = RCB_NUM;
			calibration_steps[step].code = -1;
		}
		// Restart emulation.
		freeze(false);
		calibrating = false;
		return;
	}
	if (calibration_steps[step].once == false) {
		char *dump;

		if (type == RCBJ)
			dump = dump_joypad(code);
		else
			dump = NULL;
		assert(calibration_steps[step].twice == false);
		calibration_steps[step].once = true;
		calibration_steps[step].type = type;
		calibration_steps[step].code = code;
		free(dump);
	}

	if ((calibration_steps[step].once != true) ||
	    (calibration_steps[step].twice != true))
		return;
	++step;
ask:
	if (step == elemof(calibration_steps)) {
		code = calibration_steps[(elemof(calibration_steps) - 1)].code;
		if (code == -1)
		{
		}
		else {
			unsigned int i;

			for (i = 0; (i != elemof(calibration_steps)); ++i) {
				enum ctl_e id;

				id = calibration_steps[i].id
					[calibrating_controller];
				type = calibration_steps[i].type;
				code = calibration_steps[i].code;
				assert((size_t)id < elemof(control));
				assert(control[id].type == id);
				if ((id != CTL_) && (type != RCB_NUM))
					(*control[id].rc)[type] = code;
			}

		}
	}

}

static struct rc_binding_item combos[64];

static void manage_combos(md& md, bool pressed, enum rc_binding_type type,
			  intptr_t code)
{
	unsigned int i;

	(void)md;
	for (i = 0; (i != elemof(combos)); ++i) {
		if (!combos[i].assigned) {
			if (!pressed)
				return; // Not in the list, nothing to do.
			// Not found, add it to the list.
			combos[i].assigned = true;
			combos[i].type = type;
			combos[i].code = code;
			return;
		}
		if ((combos[i].type != type) || (combos[i].code != code))
			continue; // Does not match.
		if (pressed)
			return; // Already pressed.
		// Release entry.
		memmove(&combos[i], &combos[i + 1],
			((elemof(combos) - (i + 1)) * sizeof(combos[i])));
		break;
	}
}

static bool check_combos(md& md, struct rc_binding_item item[],
			 unsigned int num)
{
	unsigned int i;
	unsigned int found = 0;

	(void)md;
	for (i = 0; (i != num); ++i) {
		unsigned int j;

		if (!item[i].assigned) {
			num = i;
			break;
		}
		for (j = 0; (j != elemof(combos)); ++j) {
			if (!combos[j].assigned)
				break;
			if ((combos[j].type != item[i].type) ||
			    (combos[j].code != item[i].code))
				continue;
			++found;
			break;
		}
	}
	if (num == 0)
		return false;
	return (found == num);
}

static int manage_game_genie(md& megad, intptr_t ksym, intptr_t ksym_uni)
{
	static char buf[12];
	static kb_input_t input = { buf, 0, sizeof(buf) };
	unsigned int len = strlen(game_genie_str);

	switch (kb_input(&input, ksym, ksym_uni)) {
		unsigned int errors;
		unsigned int applied;
		unsigned int reverted;

	case KB_INPUT_ENTERED:
		megad.patch(input.buf, &errors, &applied, &reverted);
		goto over;
	case KB_INPUT_IGNORED:
	case KB_INPUT_ABORTED:
	case KB_INPUT_CONSUMED:
		break;
	}
	return 0;
over:
	input.buf = buf;
	input.pos = 0;
	input.size = sizeof(buf);
	memset(buf, 0, sizeof(buf));
	return 1;
}

#ifdef WITH_PICO

static void manage_pico_pen(md& megad)
{
	static unsigned long pico_pen_last_update;
	unsigned long pico_pen_now;

	if (!megad.pico_enabled)
		return;
	// Repeat pen motion as long as buttons are not released.
	// This is not necessary when pen is managed by direct coordinates.
	if ((((control[CTL_PICO_PEN_UP].pressed) &&
	      (!control[CTL_PICO_PEN_UP].coord)) ||
	     ((control[CTL_PICO_PEN_DOWN].pressed) &&
	      (!control[CTL_PICO_PEN_DOWN].coord)) ||
	     ((control[CTL_PICO_PEN_LEFT].pressed) &&
	      (!control[CTL_PICO_PEN_LEFT].coord)) ||
	     ((control[CTL_PICO_PEN_RIGHT].pressed) &&
	      (!control[CTL_PICO_PEN_RIGHT].coord))) &&
	    
	     ((pico_pen_now - pico_pen_last_update) >=
	      ((unsigned long)pico_pen_delay * 1000))) {
		if (control[CTL_PICO_PEN_UP].pressed)
			ctl_pico_pen
				(control[CTL_PICO_PEN_UP], megad);
		if (control[CTL_PICO_PEN_DOWN].pressed)
			ctl_pico_pen
				(control[CTL_PICO_PEN_DOWN], megad);
		if (control[CTL_PICO_PEN_LEFT].pressed)
			ctl_pico_pen
				(control[CTL_PICO_PEN_LEFT], megad);
		if (control[CTL_PICO_PEN_RIGHT].pressed)
			ctl_pico_pen
				(control[CTL_PICO_PEN_RIGHT], megad);
		pico_pen_last_update = pico_pen_now;
	}
}

#endif

static bool mouse_is_grabbed()
{
	return (SDL_WM_GrabInput(SDL_GRAB_QUERY) == SDL_GRAB_ON);
}

static void mouse_grab(bool grab)
{
	SDL_GrabMode mode = SDL_WM_GrabInput(SDL_GRAB_QUERY);

	if ((grab) && (!pd_freeze) && (mode == SDL_GRAB_OFF)) {
		// Hide the cursor.
		SDL_ShowCursor(0);
		SDL_WM_GrabInput(SDL_GRAB_ON);
	}
	else if ((!grab) && (mode == SDL_GRAB_ON)) {
		SDL_ShowCursor(1);
		SDL_WM_GrabInput(SDL_GRAB_OFF);
	}
}

static int stop_events(md& megad, enum events status)
{
	struct ctl* ctl;

	stopped = 1;
	freeze(true);
	events = status;
	// Release controls.
	for (ctl = control; (ctl->rc != NULL); ++ctl) {
		if (ctl->pressed == false)
			continue;
		ctl->pressed = false;
		ctl->coord = false;
		if ((ctl->release != NULL) &&
		    (ctl->release(*ctl, megad) == 0))
			return -1; // XXX do something about this.
	}
	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY,
			    SDL_DEFAULT_REPEAT_INTERVAL);
	mouse_grab(false);
	return 0;
}

static void restart_events(md& megad)
{
	(void)megad;
	stopped = 1;
	freeze(false);
	SDL_EnableKeyRepeat(0, 0);
	events = STARTED;
}

static struct {
	unsigned long when[0x100];
	uint8_t enabled[0x100 / 8];
	unsigned int count;
} mouse_motion_release;

#define MOUSE_MOTION_RELEASE_IS_ENABLED(which) \
	(mouse_motion_release.enabled[(which) / 8] & (1 << ((which) % 8)))
#define MOUSE_MOTION_RELEASE_DISABLE(which) \
	(mouse_motion_release.enabled[(which) / 8] &= ~(1 << ((which) % 8)))
#define MOUSE_MOTION_RELEASE_ENABLE(which) \
	(mouse_motion_release.enabled[(which) / 8] |= (1 << ((which) % 8)))

static void mouse_motion_delay_release(unsigned int which, bool enable)
{
	if (which >= elemof(mouse_motion_release.when)) {
		DEBUG(("mouse index too high (%u)", which));
		return;
	}
	if (!enable) {
		if (!MOUSE_MOTION_RELEASE_IS_ENABLED(which))
			return;
		MOUSE_MOTION_RELEASE_DISABLE(which);
		assert(mouse_motion_release.count != 0);
		--mouse_motion_release.count;
		return;
	}
	if (!MOUSE_MOTION_RELEASE_IS_ENABLED(which)) {
		MOUSE_MOTION_RELEASE_ENABLE(which);
		++mouse_motion_release.count;
		assert(mouse_motion_release.count <=
		       elemof(mouse_motion_release.when));
	}
	mouse_motion_release.when[which] =
		(dgen_mouse_delay * 1000);
}

static bool mouse_motion_released(SDL_Event *event)
{
	unsigned int i;
	unsigned long now;

	if (mouse_motion_release.count == 0)
		return false;
	now = 0;
	for (i = 0; (i != mouse_motion_release.count); ++i) {
		unsigned long diff;

		if (!MOUSE_MOTION_RELEASE_IS_ENABLED(i))
			continue;
		diff = (mouse_motion_release.when[i] - now);
		if (diff < (unsigned long)(dgen_mouse_delay * 1000))
			continue;
		event->motion.type = SDL_MOUSEMOTION;
		event->motion.which = i;
		event->motion.xrel = 0;
		event->motion.yrel = 0;
		MOUSE_MOTION_RELEASE_DISABLE(i);
		--mouse_motion_release.count;
		return true;
	}
	return false;
}

#define MOUSE_SHOW_USECS (unsigned long)(2 * 1000000)

// The massive event handler!
// I know this is an ugly beast, but please don't be discouraged. If you need
// help, don't be afraid to ask me how something works. Basically, just handle
// all the event keys, or even ignore a few if they don't make sense for your
// interface.
int pd_handle_events(md &megad)
{
	static uint16_t kpress[0x100];
	static unsigned long hide_mouse_when;
	static bool hide_mouse;
	uint32_t plist[8];
	uint32_t rlist[8];
	unsigned int i, pi, ri;
	SDL_Event event;
	uint16_t ksym_uni;
	intptr_t ksym;
	intptr_t mouse;
	unsigned int which;

	if ((hide_mouse) &&
	    ((hide_mouse_when - 0) >= MOUSE_SHOW_USECS)) {
		if (!mouse_is_grabbed())
			SDL_ShowCursor(0);
		hide_mouse = false;
	}
next_event:
	if (mouse_motion_released(&event))
		goto mouse_motion;
	if (!SDL_PollEvent(&event)) {
#ifdef WITH_PICO
		manage_pico_pen(megad);
#endif
		return 1;
	}
	switch (event.type) {
	case SDL_KEYDOWN:
		ksym = event.key.keysym.sym;
		ksym_uni = event.key.keysym.unicode;
		if ((ksym_uni < 0x20) ||
		    ((ksym >= SDLK_KP0) && (ksym <= SDLK_KP_EQUALS)))
			ksym_uni = 0;
		kpress[(ksym & 0xff)] = ksym_uni;
		if (ksym_uni)
			ksym = ksym_uni;
		else if (event.key.keysym.mod & KMOD_SHIFT)
			ksym |= KEYSYM_MOD_SHIFT;

		// Check for modifiers
		if (event.key.keysym.mod & KMOD_CTRL)
			ksym |= KEYSYM_MOD_CTRL;
		if (event.key.keysym.mod & KMOD_ALT)
			ksym |= KEYSYM_MOD_ALT;
		if (event.key.keysym.mod & KMOD_META)
			ksym |= KEYSYM_MOD_META;

		manage_combos(megad, true, RCBK, ksym);

		if (calibrating) {
			manage_calibration(RCBK, ksym);
			break;
		}

		switch (events) {
			int ret;

		case STARTED:
			break;
		case GAME_GENIE:
		case STOPPED_GAME_GENIE:
			if (manage_game_genie(megad, ksym, ksym_uni) == 0)
				goto next_event;
			if (events == STOPPED_GAME_GENIE) {
				events = STOPPED;
			}
			else
				restart_events(megad);
			goto next_event;
		case STOPPED:
			// In basic stopped mode, handle a few keysyms.
			if (ksym == dgen_game_genie[0]) {
				events = STOPPED_GAME_GENIE;
			}
			else if (ksym == dgen_prompt[0]) {
				events = STOPPED_PROMPT;
			}
			else if (ksym == dgen_quit[0]) {
				restart_events(megad);
				return 0;
			}
			else if (ksym == dgen_stop[0]) {
				restart_events(megad);
			}
		default:
			goto next_event;
		}

		for (struct ctl* ctl = control; (ctl->rc != NULL); ++ctl) {
			if (ksym != (*ctl->rc)[RCBK])
				continue;
			assert(ctl->press != NULL);
			ctl->pressed = true;
			ctl->coord = false;
			if (ctl->press(*ctl, megad) == 0)
				return 0;
		}
		break;
	case SDL_KEYUP:
		ksym = event.key.keysym.sym;
		ksym_uni = kpress[(ksym & 0xff)];
		if ((ksym_uni < 0x20) ||
		    ((ksym >= SDLK_KP0) && (ksym <= SDLK_KP_EQUALS)))
			ksym_uni = 0;
		kpress[(ksym & 0xff)] = 0;
		if (ksym_uni)
			ksym = ksym_uni;

		manage_combos(megad, false, RCBK, ksym);
		manage_combos(megad, false, RCBK, (ksym | KEYSYM_MOD_ALT));
		manage_combos(megad, false, RCBK, (ksym | KEYSYM_MOD_SHIFT));
		manage_combos(megad, false, RCBK, (ksym | KEYSYM_MOD_CTRL));
		manage_combos(megad, false, RCBK, (ksym | KEYSYM_MOD_META));

		if (calibrating)
			break;
		if (events != STARTED)
			break;

		// The only time we care about key releases is for the
		// controls, but ignore key modifiers so they never get stuck.
		for (struct ctl* ctl = control; (ctl->rc != NULL); ++ctl) {
			if (ksym != ((*ctl->rc)[RCBK] & ~KEYSYM_MOD_MASK))
				continue;
			ctl->pressed = false;
			ctl->coord = false;
			if ((ctl->release != NULL) &&
			    (ctl->release(*ctl, megad) == 0))
				return 0;
		}
		break;
	case SDL_MOUSEMOTION:
		if (!mouse_is_grabbed()) {
			// Only show mouse pointer for a few seconds.
			SDL_ShowCursor(1);
			hide_mouse_when = (MOUSE_SHOW_USECS);
			hide_mouse = true;
			break;
		}
	mouse_motion:
		which = event.motion.which;
		pi = 0;
		ri = 0;
		if (event.motion.xrel < 0) {
			plist[pi++] = MO_MOTION(which, 'l');
			rlist[ri++] = MO_MOTION(which, 'r');
		}
		else if (event.motion.xrel > 0) {
			plist[pi++] = MO_MOTION(which, 'r');
			rlist[ri++] = MO_MOTION(which, 'l');
		}
		else {
			rlist[ri++] = MO_MOTION(which, 'r');
			rlist[ri++] = MO_MOTION(which, 'l');
		}
		if (event.motion.yrel < 0) {
			plist[pi++] = MO_MOTION(which, 'u');
			rlist[ri++] = MO_MOTION(which, 'd');
		}
		else if (event.motion.yrel > 0) {
			plist[pi++] = MO_MOTION(which, 'd');
			rlist[ri++] = MO_MOTION(which, 'u');
		}
		else {
			rlist[ri++] = MO_MOTION(which, 'd');
			rlist[ri++] = MO_MOTION(which, 'u');
		}
		if (pi)
			mouse_motion_delay_release(which, true);
		else
			mouse_motion_delay_release(which, false);
		for (i = 0; (i != ri); ++i)
			manage_combos(megad, false, RCBM, rlist[i]);
		for (i = 0; (i != pi); ++i)
			manage_combos(megad, true, RCBM, plist[i]);
		if (calibrating) {
			for (i = 0; ((calibrating) && (i != pi)); ++i)
				manage_calibration(RCBM, plist[i]);
			break;
		}
		if (events != STARTED)
			break;
		for (struct ctl* ctl = control; (ctl->rc != NULL); ++ctl) {
			// Release buttons first.
			for (i = 0; (i != ri); ++i) {
				if ((ctl->pressed == false) ||
				    ((uint32_t)(*ctl->rc)[RCBM] != rlist[i]))
					continue;
				ctl->pressed = false;
				ctl->coord = true;
				ctl->x = event.motion.x;
				ctl->y = event.motion.y;
				if ((ctl->release != NULL) &&
				    (ctl->release(*ctl, megad) == 0))
					return 0;
			}
			for (i = 0; (i != pi); ++i) {
				if ((uint32_t)(*ctl->rc)[RCBM] == plist[i]) {
					assert(ctl->press != NULL);
					ctl->pressed = true;
					ctl->coord = true;
					ctl->x = event.motion.x;
					ctl->y = event.motion.y;
					if (ctl->press(*ctl, megad) == 0)
						return 0;
				}
			}
		}
		break;
	case SDLK_ESCAPE:
	case SDL_QUIT:
		// We've been politely asked to exit, so let's leave
		return 0;
	default:
		break;
	}
	goto next_event;
}



void pd_show_carthead(md& megad)
{
	struct {
		const char *p;
		const char *s;
		size_t len;
	} data[] = {
#define CE(i, s) { i, s, sizeof(s) }
		CE("System", megad.cart_head.system_name),
		CE("Copyright", megad.cart_head.copyright),
		CE("Domestic name", megad.cart_head.domestic_name),
		CE("Overseas name", megad.cart_head.overseas_name),
		CE("Product number", megad.cart_head.product_no),
		CE("Memo", megad.cart_head.memo),
		CE("Countries", megad.cart_head.countries)
	};
	size_t i;

	for (i = 0; (i < (sizeof(data) / sizeof(data[0]))); ++i) {
		char buf[256];
		size_t j, k;

		k = (size_t)snprintf(buf, sizeof(buf), "%s: ", data[i].p);
		if (k >= (sizeof(buf) - 1))
			continue;
		// Filter out extra spaces.
		for (j = 0; (j < data[i].len); ++j)
			if (isgraph(data[i].s[j]))
				break;
		if (j == data[i].len)
			continue;
		while ((j < data[i].len) && (k < (sizeof(buf) - 2))) {
			if (isgraph(data[i].s[j])) {
				buf[(k++)] = data[i].s[j];
				++j;
				continue;
			}
			buf[(k++)] = ' ';
			while ((j < data[i].len) && (!isgraph(data[i].s[j])))
				++j;
		}
		if (buf[(k - 1)] == ' ')
			--k;
		buf[k] = '\n';
		buf[(k + 1)] = '\0';
	}
}

/* Clean up this awful mess :) */
void pd_quit()
{
	size_t i;

	if (mdscr.data) {
		free((void*)mdscr.data);
		mdscr.data = NULL;
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	
	#ifndef NOSOUND
	pd_sound_deinit();
	#endif
	
	if (mdpal)
		mdpal = NULL;
	free(filters_stack_data_buf[0].u8);
	free(filters_stack_data_buf[1].u8);
	assert(filters_stack_size <= elemof(filters_stack));
	assert(filters_stack_data[0].data == NULL);
	filters_stack_default = false;
	for (i = 0; (i != filters_stack_size); ++i) {
		free(filters_stack_data[i + 1].data);
		filters_stack_data[i + 1].data = NULL;
	}
	filters_stack_size = 0;
	SDL_Quit();
}
