#pragma once
#ifndef __RXDK_COMPAT_WINNT_H__
#define __RXDK_COMPAT_WINNT_H__

#define _NTDEF_
#define _WINNT_

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif

typedef long LONG;
typedef long *PLONG;
typedef long LONG_PTR, *PLONG_PTR;
typedef unsigned long ULONG_PTR, DWORD_PTR;
typedef unsigned int UINT_PTR, *PUINT_PTR;
typedef unsigned int SIZE_T, *PSIZE_T;
typedef LONG HRESULT;
typedef short SHORT;
typedef unsigned short WCHAR;

#ifndef S_OK
#define S_OK ((HRESULT)0L)
#endif
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1L)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif

#define MAKE_HRESULT(sev, facility, code) ((HRESULT)(((unsigned long)(sev)<<31) | ((unsigned long)(facility)<<16) | ((unsigned long)(code))))

#define VOID void
typedef char CHAR;
typedef CHAR *LPSTR, *PSTR;
typedef const CHAR *LPCSTR, *PCSTR;
typedef void *PVOID;
typedef PVOID HANDLE;

#ifndef DECLARE_HANDLE
#define DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__ *name
#endif

DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HMODULE);
DECLARE_HANDLE(HINSTANCE);

typedef struct _FILETIME { unsigned long dwLowDateTime, dwHighDateTime; } FILETIME, *PFILETIME, *LPFILETIME;

#define NTAPI __stdcall
typedef LONG NTSTATUS;

#endif
