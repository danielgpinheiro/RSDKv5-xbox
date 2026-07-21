// RXDK compat header — included before any C++ STL headers.
#ifndef _RXDK_CHAR_TYPES_H_
#define _RXDK_CHAR_TYPES_H_

#if defined(__cplusplus) && (defined(__XBOX__) || defined(_XBOX))
// -fms-extensions removes char16_t/char32_t as C++ keywords.
// libc++ headers expect them to exist. The __char versions are Clang builtins.
typedef __char16_t char16_t;
typedef __char32_t char32_t;
#endif

// RXDK libc++ include order fix: pre-define all C wrapper guard macros
#define _LIBCPP_ERRNO_H
#define _LIBCPP_MATH_H
#define _LIBCPP_STDDEF_H
#define _LIBCPP_STDIO_H
#define _LIBCPP_STDLIB_H
#define _LIBCPP_STRING_H
#define _LIBCPP_UCHAR_H
#define _LIBCPP_WCHAR_H
#define _LIBCPP_WCTYPE_H

#endif
