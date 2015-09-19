// DGen/SDL 1.17
// by Joe Groff <joe@pknet.com>
// Read LICENSE for copyright etc., but if you've seen one BSDish license,
// you've seen them all ;)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>

#ifdef __MINGW32__
#include <windows.h>
#include <wincon.h>
#endif

#define IS_MAIN_CPP
#include "system.h"
#include "md.h"
#include "pd.h"
#include "pd-defs.h"
#include "rc.h"
#include "rc-vars.h"

#ifdef __BEOS__
#include <OS.h>
#endif

#ifdef __MINGW32__
static long dgen_mingw_detach = 1;
#endif

// Defined in ras.cpp, and set to true if the Genesis palette's changed.
extern int pal_dirty;

FILE *debug_log = NULL;


// Temporary garbage can string :)
static char temp[65536] = "";

// Show help and exit with code 2

// Save/load states
// It is externed from your implementation to change the current slot
// (I know this is a hack :)
int slot = 0;
void md_save(md& megad)
{
	FILE *save;
	char file[64];

	if (!megad.plugged) {
		return;
	}
	if (((size_t)snprintf(file,
			      sizeof(file),
			      "%s.gs%d",
			      megad.romname,
			      slot) >= sizeof(file)) ||
	    ((save = dgen_fopen("saves", file, DGEN_WRITE)) == NULL)) {
		snprintf(temp, sizeof(temp),
			 "Couldn't save state to slot %d!", slot);
		return;
	}
	megad.export_gst(save);
	fclose(save);
	snprintf(temp, sizeof(temp), "Saved state to slot %d.", slot);
}

void md_load(md& megad)
{
	FILE *load;
	char file[64];

	if (!megad.plugged) {
		return;
	}
	if (((size_t)snprintf(file,
			      sizeof(file),
			      "%s.gs%d",
			      megad.romname,
			      slot) >= sizeof(file)) ||
	    ((load = dgen_fopen("saves", file, DGEN_READ)) == NULL)) {
		snprintf(temp, sizeof(temp),
			 "Couldn't load state from slot %d!", slot);
		return;
	}
	megad.import_gst(load);
	fclose(load);
	snprintf(temp, sizeof(temp), "Loaded state from slot %d.", slot);
}

// Load/save states from file
void ram_save(md& megad)
{
	FILE *save;
	int ret;

	if (!megad.has_save_ram())
		return;
	save = dgen_fopen("ram", megad.romname, DGEN_WRITE);
	if (save == NULL)
		goto fail;
	ret = megad.put_save_ram(save);
	fclose(save);
	if (ret == 0)
		return;
fail:
	fprintf(stderr, "Couldn't save battery RAM to `%s'\n", megad.romname);
}

void ram_load(md& megad)
{
	FILE *load;
	int ret;

	if (!megad.has_save_ram())
		return;
	load = dgen_fopen("ram", megad.romname, DGEN_READ);
	if (load == NULL)
		goto fail;
	ret = megad.get_save_ram(load);
	fclose(load);
	if (ret == 0)
		return;
fail:
	fprintf(stderr, "Couldn't load battery RAM from `%s'\n",
		megad.romname);
}

int dgen(char* romname)
{
  int c = 0, stop = 0, usec = 0, start_slot = -1;
  unsigned long frames, frames_old, fps;
  char *patches = NULL, *rom = NULL;
  unsigned long oldclk, newclk, startclk, fpsclk;
  FILE *file = NULL;
  unsigned int samples;
  class md *megad;
  bool first = true;
  bool forced_hz = false;
  bool forced_pal = false;


  // Check all our options
  snprintf(temp, sizeof(temp), "%s%s",
	   "s:hvr:n:p:R:NPH:d:D:",
	   pd_options);

  // Initialize the platform-dependent stuff.
	if (!pd_graphics_init(dgen_sound, dgen_pal, dgen_hz))
    {
      fprintf(stderr, "main: Couldn't initialize graphics!\n");
      return 1;
    }
    
    #ifndef NOSOUND
	if(dgen_sound)
    {
      long rate = dgen_soundrate;

      if (dgen_soundsegs < 0)
	      dgen_soundsegs = 0;
      samples = (dgen_soundsegs * (rate / dgen_hz));
      pd_sound_init(rate, samples);
    }
    #endif

	rom = romname;
	// Create the megadrive object.
	megad = new md(dgen_pal, dgen_region);
	if ((megad == NULL) || (!megad->okay())) {
		fprintf(stderr, "main: Mega Drive initialization failed.\n");
		goto clean_up;
	}
next_rom:
	// Load the requested ROM.
	if (rom != NULL) {
		if (megad->load(rom)) {
				goto clean_up;
		}
	}

	first = false;
	// Set untouched pads.
	megad->pad[0] = MD_PAD_UNTOUCHED;
	megad->pad[1] = MD_PAD_UNTOUCHED;
#ifdef WITH_JOYSTICK
	if (dgen_joystick)
		megad->init_joysticks();
#endif
	// Load patches, if given.
	if (patches) {
		printf("main: Using patch codes \"%s\".\n", patches);
		megad->patch(patches, NULL, NULL, NULL);
		// Use them only once.
		patches = NULL;
	}
	// Reset
	megad->reset();

	// Automatic region settings from ROM header.
	if (!dgen_region) {
		uint8_t c = megad->region_guess();
		int hz;
		int pal;

		md::region_info(c, &pal, &hz, 0, 0, 0);
		if (forced_hz)
			hz = dgen_hz;
		if (forced_pal)
			pal = dgen_pal;
		if ((hz != dgen_hz) || (pal != dgen_pal) ||
		    (c != megad->region)) {
			megad->region = c;
			dgen_hz = hz;
			dgen_pal = pal;
			printf("main: reconfiguring for region \"%c\": "
			       "%dHz (%s)\n", c, hz, (pal ? "PAL" : "NTSC"));
			pd_graphics_reinit(dgen_sound, dgen_pal, dgen_hz);
			if (dgen_sound) {
				long rate = dgen_soundrate;

				pd_sound_deinit();
				samples = (dgen_soundsegs * (rate / dgen_hz));
				pd_sound_init(rate, samples);
			}
			megad->pal = pal;
			megad->init_pal();
			megad->init_sound();
		}
	}

	// Load up save RAM
	ram_load(*megad);
	// If -s option was given, load the requested slot
	if (start_slot >= 0) {
		slot = start_slot;
		md_load(*megad);
	}
	// If autoload is on, load save state 0
	else if (dgen_autoload) {
		slot = 0;
		md_load(*megad);
	}

	// Show cartridge header
	if (dgen_show_carthead)
		pd_show_carthead(*megad);

	// Go around, and around, and around, and around... ;)
	frames = 0;
	frames_old = 0;
	fps = 0;
	while (!stop) {
#ifndef NOSOUND
			if (dgen_sound) {
				megad->one_frame(&mdscr, mdpal, &sndi);
				pd_sound_write();
			}
			else
#endif
			megad->one_frame(&mdscr, mdpal, NULL);

			pd_graphics_update(megad->plugged);
			++frames;
		stop |= (pd_handle_events(*megad) ^ 1);
	}

#ifdef WITH_JOYSTICK
	if (dgen_joystick)
		megad->deinit_joysticks();
#endif

#ifdef WITH_DEBUGGER
	megad->debug_leave();
#endif
	ram_save(*megad);
	if (dgen_autosave) {
		slot = 0;
		md_save(*megad);
	}
	megad->unplug();
	if (file) {
		fclose(file);
		file = NULL;
	}
clean_up:
	// Cleanup
	delete megad;
	pd_sound_deinit();
	pd_quit();

	// Come back anytime :)
	return 0;
}
