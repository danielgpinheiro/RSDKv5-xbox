#pragma once

// XDK headers unconditionally #define __cdecl to empty.
// Restore it as the Clang builtin keyword so our declarations use cdecl.
#ifdef __cdecl
#undef __cdecl
#endif

#ifdef __cplusplus
extern "C" {
#endif

float __cdecl tanf(float x);
float __cdecl asinf(float x);
float __cdecl acosf(float x);
float __cdecl expf(float x);
float __cdecl logf(float x);
float __cdecl ldexpf(float x, int e);

#ifdef __cplusplus
}
#endif
