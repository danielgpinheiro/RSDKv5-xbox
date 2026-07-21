#ifdef __XBOX__
#include <picolibc.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#define NT_INCLUDED
#include <windef.h>
#include <d3d8.h>

int _fltused = 0x9875;

int *__errno(void) { static int e; return &e; }

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
_CI(acos) _CI(asin) _CI(atan) _CI(cos)
_CI(log) _CI(log10) _CI(sin) _CI(sqrt) _CI(tan)
double __CIfmod(double x, double y) { return fmod(x, y); }
double __CIatan2(double y, double x) { return atan2(y, x); }
double __CIpow(double x, double y) { return pow(x, y); }

FILE *__iob[1] = { NULL };
unsigned long long __aulldvrm(unsigned long long a, unsigned long long b) { return (b) ? (a % b) : 0; }
long long __alldvrm(long long a, long long b) { return (b) ? (a % b) : 0; }

__asm__(".section .eh_frame,\"a\"\n.balign 4\n.global ___eh_frame_start\n___eh_frame_start:\n.global ___eh_frame_end\n___eh_frame_end:\n.text");

/* D3D stdcall wrappers: use __asm__(".def ...") or aliases */
/* These need exact ASM name control. Using __asm__ labels: */

__asm__(".globl _D3DDevice_CreateTexture2@28\n"
        ".set _D3DDevice_CreateTexture2@28, D3DDevice_CreateTexture@28\n");
__asm__(".globl _D3DDevice_CreateVertexBuffer2@4\n"
        ".set _D3DDevice_CreateVertexBuffer2@4, D3DDevice_CreateVertexBuffer@4\n");
__asm__(".globl _D3DDevice_GetRenderTarget2@0\n"
        ".set _D3DDevice_GetRenderTarget2@0, D3DDevice_GetRenderTarget@0\n");
__asm__(".globl _D3DTexture_GetSurfaceLevel2@8\n"
        ".set _D3DTexture_GetSurfaceLevel2@8, D3DTexture_GetSurfaceLevel@8\n");
__asm__(".globl _D3DVertexBuffer_Lock2@8\n"
        ".set _D3DVertexBuffer_Lock2@8, D3DVertexBuffer_Lock@8\n");

#endif
