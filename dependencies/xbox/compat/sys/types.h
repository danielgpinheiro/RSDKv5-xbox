#ifndef XBOX_COMPAT_SYS_TYPES_H
#define XBOX_COMPAT_SYS_TYPES_H

// Dummy <sys/types.h> for the original-Xbox / nxdk build.
//
// nxdk's libc does not ship a <sys/types.h>, but some compiled sources include it
// (notably the nxdk-sdl3 SDL3 backend, e.g. src/video/SDL_video.c). Upstream nxdk
// used to lack this header; a fork carried an empty stub in
// lib/xboxrt/libc_extensions/sys/types.h. To let the submodule point straight at
// XboxDev/nxdk master (which has no such stub), that stub now lives here in the
// parent repo and is put on the include path via Makefile.nxdk (-I dependencies/xbox/compat).
//
// Intentionally empty: the sources that include it on Xbox don't actually use any
// type from it (the code paths that would are compiled out for this platform).

#endif // XBOX_COMPAT_SYS_TYPES_H
