// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

/* The instruction set questions the emulator asks, over the probe in
 * libretro-common. cpu_features_get() runs the probe once and hands out
 * a cached mask thereafter, so these are a load and a test. */

#include <features/features_cpu.h>
#include <libretro.h>

/* Plain bit tests over the cached mask; no wrapper functions to inline. */
#define CPU_HAS_SSE41 ((cpu_features_get() & RETRO_SIMD_SSE4) != 0)
#define CPU_HAS_AVX   ((cpu_features_get() & RETRO_SIMD_AVX)  != 0)
#define CPU_HAS_AVX2  ((cpu_features_get() & RETRO_SIMD_AVX2) != 0)
#define CPU_HAS_AVX512 ((cpu_features_get() & RETRO_SIMD_AVX512) != 0)

