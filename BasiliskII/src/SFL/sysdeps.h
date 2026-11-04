/*
 *  sysdeps.h - System dependent definitions for the SFL platform layer
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

#ifndef SYSDEPS_H
#define SYSDEPS_H

#include "config.h"
#include "user_strings_sfl.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef SHEEPSHAVER
#define POWERPC_ROM 1
#define EMULATED_PPC 1

#define ROM_IS_WRITE_PROTECTED 0
#define USE_SCRATCHMEM_SUBTERFUGE 1

#define PPC_REENTRANT_JIT 1
#define PPC_CHECK_INTERRUPTS 1
#define PPC_DECODE_CACHE 1
#define PPC_FLIGHT_RECORDER 0
#define PPC_PROFILE_COMPILE_TIME 0
#define PPC_PROFILE_GENERIC_CALLS 0
#define KPX_MAX_CPUS 1
#define PPC_ENABLE_JIT 0
#define USE_ETHER_FULL_DRIVER 1
#define PPC_NATIVE_JIT 0
#else
#define EMULATED_68K 1
#define ROM_IS_WRITE_PROTECTED 1
#define REAL_ADDRESSING 0
#define DIRECT_ADDRESSING 0
#endif

typedef unsigned char uint8;
typedef signed char int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;
#define VAL64(a) (a ## LL)
#define UVAL64(a) (a ## uLL)
#if SIZEOF_VOID_P == 4
typedef uint32 uintptr;
typedef int32 intptr;
#else
typedef uint64 uintptr;
typedef int64 intptr;
#endif

#if defined(__i386__) || defined(__x86_64__)
#define UNALIGNED_PROFITABLE 1
#endif

static inline uint16 generic_bswap_16(uint16 x)
{
	return (uint16)(((x & 0xff) << 8) | ((x >> 8) & 0xff));
}

static inline uint32 generic_bswap_32(uint32 x)
{
	return ((x & 0xff000000) >> 24) | ((x & 0x00ff0000) >> 8) | ((x & 0x0000ff00) << 8) | ((x & 0x000000ff) << 24);
}

static inline uint64 generic_bswap_64(uint64 x)
{
	return ((uint64)generic_bswap_32((uint32)x) << 32) | generic_bswap_32((uint32)(x >> 32));
}

#define bswap_16 generic_bswap_16
#define bswap_32 generic_bswap_32
#define bswap_64 generic_bswap_64

#ifdef WORDS_BIGENDIAN
static inline uint16 tswap16(uint16 x) { return x; }
static inline uint32 tswap32(uint32 x) { return x; }
static inline uint64 tswap64(uint64 x) { return x; }
#else
static inline uint16 tswap16(uint16 x) { return bswap_16(x); }
static inline uint32 tswap32(uint32 x) { return bswap_32(x); }
static inline uint64 tswap64(uint64 x) { return bswap_64(x); }
#endif

#define do_byteswap_16(x) (generic_bswap_16((uint16)(x)))
#define do_byteswap_32(x) (generic_bswap_32((uint32)(x)))

#ifdef WORDS_BIGENDIAN
#define ntohl(x) ((uint32)(x))
#define htonl(x) ((uint32)(x))
#define ntohs(x) ((uint16)(x))
#define htons(x) ((uint16)(x))
#else
#define ntohl(x) generic_bswap_32((uint32)(x))
#define htonl(x) generic_bswap_32((uint32)(x))
#define ntohs(x) generic_bswap_16((uint16)(x))
#define htons(x) generic_bswap_16((uint16)(x))
#endif

typedef volatile int spinlock_t;
static const spinlock_t SPIN_LOCK_UNLOCKED = 0;
static inline void spin_lock(spinlock_t *lock) {}
static inline void spin_unlock(spinlock_t *lock) {}
static inline int spin_trylock(spinlock_t *lock) { return 1; }

typedef int64 tm_time_t;
typedef int64 loff_t;

extern void timer_init(void);
extern uint64 GetTicks_usec(void);
extern void Delay_usec(uint32 usec);

typedef struct rgb_color {
	uint8 red;
	uint8 green;
	uint8 blue;
	uint8 alpha;
} rgb_color;

#ifndef SHEEPSHAVER
#define uae_u8 uint8
#define uae_s8 int8
#define uae_u16 uint16
#define uae_s16 int16
#define uae_u32 uint32
#define uae_s32 int32
#define uae_u64 uint64
#define uae_s64 int64
typedef uae_u32 uaecptr;
#ifdef _MSC_VER
#define __inline__ inline
#endif
#define CPU_EMU_SIZE 0
#define FPU_IEEE 1
#define IEEE_FLOAT_FORMAT 1
#define USE_PREFETCH_BUFFER 0
#define SUPPORTS_EXTFS 1
#define ASM_SYM(name)
#define write_log printf
#define call_mem_get_func(func, addr) ((*func)(addr))
#define call_mem_put_func(func, addr, value) ((*func)(addr, value))
#define ENUMDECL typedef enum
#define ENUMNAME(name) name
#define REGPARAM
#define REGPARAM2
#define do_get_mem_byte(address) (*(const uint8 *)(address))
#define do_put_mem_byte(address, value) (*(uint8 *)(address) = (uint8)(value))
#define do_get_mem_word_unswapped(address) (*(const uint16 *)(address))

static inline uint16 do_get_mem_word(const void *address)
{
	uint16 value;
	memcpy(&value, address, sizeof(value));
	return tswap16(value);
}

static inline uint32 do_get_mem_long(const void *address)
{
	uint32 value;
	memcpy(&value, address, sizeof(value));
	return tswap32(value);
}

static inline void do_put_mem_word(void *address, uint16 value)
{
	value = tswap16(value);
	memcpy(address, &value, sizeof(value));
}

static inline void do_put_mem_long(void *address, uint32 value)
{
	value = tswap32(value);
	memcpy(address, &value, sizeof(value));
}
#else
#define CallMacOS(type, tvect) call_macos((uintptr)tvect)
#define CallMacOS1(type, tvect, arg1) call_macos1((uintptr)tvect, (uintptr)arg1)
#define CallMacOS2(type, tvect, arg1, arg2) call_macos2((uintptr)tvect, (uintptr)arg1, (uintptr)arg2)
#define CallMacOS3(type, tvect, arg1, arg2, arg3) call_macos3((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3)
#define CallMacOS4(type, tvect, arg1, arg2, arg3, arg4) call_macos4((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4)
#define CallMacOS5(type, tvect, arg1, arg2, arg3, arg4, arg5) call_macos5((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4, (uintptr)arg5)
#define CallMacOS6(type, tvect, arg1, arg2, arg3, arg4, arg5, arg6) call_macos6((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4, (uintptr)arg5, (uintptr)arg6)
#define CallMacOS7(type, tvect, arg1, arg2, arg3, arg4, arg5, arg6, arg7) call_macos7((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4, (uintptr)arg5, (uintptr)arg6, (uintptr)arg7)

#ifdef __cplusplus
extern "C" {
#endif
extern uint32 call_macos(uint32 tvect);
extern uint32 call_macos1(uint32 tvect, uint32 arg1);
extern uint32 call_macos2(uint32 tvect, uint32 arg1, uint32 arg2);
extern uint32 call_macos3(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3);
extern uint32 call_macos4(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4);
extern uint32 call_macos5(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5);
extern uint32 call_macos6(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6);
extern uint32 call_macos7(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7);
extern uint32 call_macos8(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7, uint32 arg8);
#ifdef __cplusplus
}
#endif

#endif

#endif
