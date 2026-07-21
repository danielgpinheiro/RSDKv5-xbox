#ifdef __XBOX__
#include <picolibc.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <d3d8.h>

int _fltused = 0x9875;

int *__errno(void) {
    static int e;
    return &e;
}

int __strnicmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b) {
        char ca = (char)((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a);
        char cb = (char)((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b);
        int d = ca - cb;
        if (d) return d;
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

int _ftol2_sse(float x) { return (int)x; }
int _ftol(float x) { return (int)x; }

#define _CI(name) double __CI##name(double x) { return name(x); }
_CI(acos)
_CI(asin)
_CI(atan)
_CI(cos)
_CI(log)
_CI(log10)
_CI(sin)
_CI(sqrt)
_CI(tan)
double __CIfmod(double x, double y) { return fmod(x, y); }
double __CIatan2(double y, double x) { return atan2(y, x); }
double __CIpow(double x, double y) { return pow(x, y); }

FILE *__iob[1] = { NULL };

/* D3D 2-suffix to non-2-suffix wrappers for XDK -> RXDK compat */
__declspec(dllexport) HRESULT WINAPI D3DDevice_CreateTexture2(UINT w, UINT h, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, D3DTexture **ppT) {
    return D3DDevice_CreateTexture(w, h, lv, u, f, p, ppT);
}
__declspec(dllexport) HRESULT WINAPI D3DDevice_CreateSurface2(UINT w, UINT h, D3DFORMAT f, D3DSurface **ppS) {
    return 0x8876086C;
}
__declspec(dllexport) HRESULT WINAPI D3DDevice_CreateVertexBuffer2(UINT len, DWORD u, DWORD fvf, D3DPOOL p, D3DVertexBuffer **ppVB) {
    return D3DDevice_CreateVertexBuffer(len, u, fvf, p, ppVB);
}
__declspec(dllexport) HRESULT WINAPI D3DDevice_GetRenderTarget2(D3DSurface **ppRT) {
    D3DDevice_GetRenderTarget(ppRT);
    return 0;
}
__declspec(dllexport) HRESULT WINAPI D3DTexture_GetSurfaceLevel2(D3DTexture *pThis, UINT lv, D3DSurface **ppSL) {
    return D3DTexture_GetSurfaceLevel(pThis, lv, ppSL);
}
__declspec(dllexport) void WINAPI D3DVertexBuffer_Lock2(D3DVertexBuffer *pThis, UINT offs, UINT sz, BYTE **ppbD, DWORD f) {
    D3DVertexBuffer_Lock(pThis, offs, sz, ppbD, f);
}

unsigned long long __aulldvrm(unsigned long long a, unsigned long long b) { return (b) ? (a % b) : 0; }
long long __alldvrm(long long a, long long b) { return (b) ? (a % b) : 0; }

__asm__(".section .eh_frame,\"a\"\n.balign 4\n.global ___eh_frame_start\n___eh_frame_start:\n.global ___eh_frame_end\n___eh_frame_end:\n.text");

#endif
