// RXDK MSVC-compatibility: -fms-extensions removes char16_t/char32_t as keywords.
// libc++ headers expect them to exist. Typedef __char16_t/__char32_t (Clang builtins).
#ifndef _RXDK_CHAR_TYPES_H_
#define _RXDK_CHAR_TYPES_H_

#if defined(__cplusplus) && (defined(__XBOX__) || defined(_XBOX))
typedef __char16_t char16_t;
typedef __char32_t char32_t;
#endif

#endif
