/*
 * Hand-written replacement for the config.h that rp2040-doom's
 * CMakeLists.txt would otherwise generate from doom/cmake/config.h.cin.
 *
 * Most engine sources include this via doomtype.h.
 *
 * The wolfDemo target is not on a hosted POSIX (no autotools), so we
 * pin the few feature checks the cin file would have run. arm-none-eabi
 * with newlib provides strcasecmp/strncasecmp via <strings.h>, so we
 * declare both as available; doomtype.h then takes the strings.h path.
 */

#ifndef WOLFDEMO_DOOM_CONFIG_H
#define WOLFDEMO_DOOM_CONFIG_H

#define PACKAGE_NAME      "Chocolate Doom"
#define PACKAGE_TARNAME   "chocolate-doom"
#define PACKAGE_VERSION   "3.0.0"
#define PACKAGE_STRING    "Chocolate Doom 3.0.0"
#define PROGRAM_PREFIX    "chocolate-"

/* arm-none-eabi-gcc + newlib both ship strcasecmp/strncasecmp in
 * <strings.h>. The 0/1 form matches what the cmake cin file uses. */
#define HAVE_DECL_STRCASECMP  1
#define HAVE_DECL_STRNCASECMP 1

/* Bare-metal: no dirent.h, no mmap, no libsamplerate, no libpng. */
/* (Leave HAVE_DIRENT_H, HAVE_MMAP, HAVE_LIBSAMPLERATE, HAVE_LIBPNG
 * undefined - the engine paths that need them are already disabled by
 * NO_FILE_ACCESS / NO_USE_LIBSAMPLERATE / etc.) */

/*
 * Forward declarations for Pico SDK functions that the engine calls in
 * DOOM_TINY=1 paths but doesn't include the SDK header for. We provide
 * the implementations in src/doom_glue/. config.h is the natural place
 * because doomtype.h pulls it into every engine TU.
 */
extern void panic(const char *fmt, ...) __attribute__((noreturn));

/* I_Error is declared in i_system.h, but a few engine files (e.g.
 * w_file_memory.c via `#define panic I_Error` in its non-PICO_BUILD
 * branch) call it without including that header. */
extern void I_Error(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif /* WOLFDEMO_DOOM_CONFIG_H */
