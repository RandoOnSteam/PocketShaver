/*
 *  config.h - Build configuration for the SFL platform layer
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MACEMU_CONFIG_H
#define MACEMU_CONFIG_H

#ifdef SHEEPSHAVER
#define PACKAGE "SheepShaver"
#define PACKAGE_NAME "SheepShaver"
#define PACKAGE_STRING "SheepShaver 2.3"
#define PACKAGE_TARNAME "SheepShaver"
#define PACKAGE_VERSION "2.3"
#define VERSION "2.3"
#else
#define PACKAGE "BasiliskII"
#define PACKAGE_NAME "Basilisk II"
#define PACKAGE_STRING "Basilisk II 1.0"
#define PACKAGE_TARNAME "BasiliskII"
#define PACKAGE_VERSION "1.0"
#define VERSION "1.0"
#endif
#define PACKAGE_BUGREPORT ""
#define PACKAGE_URL ""

#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_FENV_H 1
#if defined(SFLFAULTHANDLER)
#define HAVE_SIGSEGV_SKIP_INSTRUCTION 1
#if defined(_WIN32)
#define HAVE_WIN32_EXCEPTIONS 1
#elif defined(__APPLE__)
#define HAVE_MACH_EXCEPTIONS 1
#else
#define HAVE_SIGINFO_T 1
#endif
#endif

#define USE_SFL 1
#ifdef SHEEPSHAVER
#define SFL_PAGED_ADDRESSING 1
#endif

#if defined(__BIG_ENDIAN__) || defined(__ppc__) || defined(__powerpc__) || defined(__POWERPC__) \
	|| defined(powerc) || defined(__m68k__) \
	|| (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define WORDS_BIGENDIAN 1
#endif

#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#if defined(__LP64__) || defined(_LP64)
#define SIZEOF_LONG 8
#else
#define SIZEOF_LONG 4
#endif
#define SIZEOF_LONG_LONG 8
#if defined(_WIN64) || defined(__LP64__) || defined(_LP64)
#define SIZEOF_VOID_P 8
#else
#define SIZEOF_VOID_P 4
#endif
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 8

#define HOST_FLOAT_FORMAT IEEE_FLOAT_FORMAT

#ifdef _MSC_VER
	#include <stddef.h>
	typedef ptrdiff_t ssize_t;
	#ifdef _M_IX86
		#ifndef __i386__
			#define __i386__
		#endif
	#elif defined(_M_AMD64)
		#ifndef __x86_64__
			#define __x86_64__
		#endif
	#endif
	#define ATTRIBUTE_PACKED
	#define ATTRIBUTE_UNUSED
#else
	#define ATTRIBUTE_PACKED __attribute__((__packed__))
	#ifndef ATTRIBUTE_UNUSED
		#define ATTRIBUTE_UNUSED __attribute__((__unused__))
	#endif
#endif

#endif
