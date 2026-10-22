/*
 * SPDX-FileCopyrightText: Copyright 2026 NSPX
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hand-written libarchive config.h for the NXbox mod extractor: MSVC (or clang-cl) targeting
 * Windows 10 / the UWP AppContainer. Only the read side of zip, 7z and rar is built (see
 * ../../CMakeLists.txt), so most of libarchive's knobs stay undefined. Kept deliberately
 * conservative: a feature is only claimed when every MSVC UCRT and the WINAPI_FAMILY_APP
 * partition have it. Crypto/random go through BCrypt, which UWP allows (the CryptoAPI fallback,
 * CryptAcquireContext, is desktop-only and must stay off).
 */
#ifndef NXBOX_LIBARCHIVE_CONFIG_H
#define NXBOX_LIBARCHIVE_CONFIG_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10 */
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
/* Windows 10 20H1 (NTDDI_WIN10_VB): the first SDK level where the UWP partition carries
 * BY_HANDLE_FILE_INFORMATION (19H1) and GetVolumePathNameW, both named by libarchive's
 * Windows headers. Every Xbox Series OS is newer. */
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000008
#endif

#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define LIBARCHIVE_STATIC 1

/* The read-only formats need no ACL, xattr or crypto-digest backends. */
/* #undef ARCHIVE_ACL_* */
/* #undef ARCHIVE_XATTR_* */

/* Headers every UCRT has. */
#define HAVE_CTYPE_H 1
#define HAVE_DIRECT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_IO_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MEMORY_H 1
#define HAVE_PROCESS_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UTIME_H 1
#define HAVE_TIME_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_WINDOWS_H 1
#define HAVE_BCRYPT_H 1
#define HAVE_WINCRYPT_H 1
#define HAVE_WINIOCTL_H 1

/* Compression libraries, both linked by ../../CMakeLists.txt. */
#define HAVE_LIBZ 1
#define HAVE_ZLIB_H 1
#define HAVE_LIBLZMA 1
#define HAVE_LZMA_H 1
#define HAVE_LZMA_STREAM_ENCODER_MT 0

/* Types and limits (C99 <stdint.h> on every supported MSVC). */
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_WCHAR_T 1
#define SIZEOF_WCHAR_T 2
#define HAVE_EILSEQ 1

/* CRT functions. */
#define HAVE_LOCALTIME_S 1
#define HAVE__LOCALTIME64_S 1
#define HAVE_GMTIME_S 1
#define HAVE__GMTIME64_S 1
#define HAVE__CTIME64_S 1
#define HAVE__FSEEKI64 1
#define HAVE__GET_TIMEZONE 1
#define HAVE__MKGMTIME 1
#define HAVE__MKGMTIME64 1
#define HAVE_MBRTOWC 1
#define HAVE_WCRTOMB 1
#define HAVE_MEMMOVE 1
#define HAVE_SETLOCALE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_STRNLEN 1
#define HAVE_STRRCHR 1
#define HAVE_VPRINTF 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_TZSET 1

/* Windows has none of these POSIX types; these are exactly what libarchive's own CMake
 * (CHECK_TYPE_SIZE fallbacks) defines on Windows. MSVC and clang-cl also lack ssize_t. */
#if defined(_WIN32)
#if defined(_MSC_VER)
#if defined(_WIN64)
#define ssize_t int64_t
#else
#define ssize_t long
#endif
#define mode_t unsigned short
#endif
#define uid_t short
#define gid_t short
#define id_t short
#if defined(_MSC_VER)
#define pid_t int
#endif
#endif

/* MinGW (only used to syntax-check this config off-target) ships these itself. */
#if defined(__MINGW32__)
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_UNISTD_H 1
#endif

#define ICONV_CONST
#define LSTAT_FOLLOWS_SLASHED_SYMLINK 0
#define HAVE_STRUCT_TM_TM_GMTOFF 0

/* libarchive's own identity, for archive_version_details(). */
#define VERSION "3.8.1"
#define BSDCAT_VERSION_STRING "3.8.1"
#define BSDCPIO_VERSION_STRING "3.8.1"
#define BSDTAR_VERSION_STRING "3.8.1"
#define BSDUNZIP_VERSION_STRING "3.8.1"
#define LIBARCHIVE_VERSION_NUMBER "3008001"
#define LIBARCHIVE_VERSION_STRING "3.8.1"

#endif /* NXBOX_LIBARCHIVE_CONFIG_H */
