#pragma once
#ifndef _WINGDI_
#define _WINGDI_

#define WINGDIAPI
#define NOGDI
#define WINVER 0x0502

typedef unsigned int WINBOOL;

#ifdef __cplusplus
extern "C" {
#endif

struct HDC__ { int unused; };
typedef struct HDC__ *HDC;

struct HPALETTE__ { int unused; };
typedef struct HPALETTE__ *HPALETTE;

struct HGDIOBJ__ { int unused; };
typedef struct HGDIOBJ__ *HGDIOBJ;

typedef HGDIOBJ HFONT, HBITMAP, HBRUSH, HPEN, HRGN;

typedef unsigned long COLORREF;

typedef struct tagPALETTEENTRY {
    unsigned char peRed, peGreen, peBlue, peFlags;
} PALETTEENTRY;

typedef struct tagLOGPALETTE {
    unsigned short palVersion, palNumEntries;
    PALETTEENTRY palPalEntry[1];
} LOGPALETTE;

#ifdef __cplusplus
}
#endif

#endif
