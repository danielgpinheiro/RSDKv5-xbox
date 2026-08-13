// Single-header pl_mpeg (phoboslab/pl_mpeg, MIT) — implementation translation unit.
// Compiled as C so the library's C source never goes through the C++ front-end.
// Video.cpp includes pl_mpeg.h for the declarations only (no PL_MPEG_IMPLEMENTATION).
//
// PLM_NO_STDIO drops the fopen/fread file-source constructors: nxdk's libc has no
// usable stdio file I/O (the project builds miniz with MINIZ_NO_STDIO for the same
// reason), and they'd otherwise be dead code that still needs fopen to link. The
// Xbox path feeds the decoder through a plm_buffer load callback backed by the
// engine's own file layer instead.
#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
