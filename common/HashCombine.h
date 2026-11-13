/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

// Use XXH3 for combining, matching the hash already used for the GS shader
// caches. The previous implementation was the boost hash_combine
// mixer over std::hash<T>; for the integral keys every call site passes,
// std::hash<integer> is the identity function, so structured/sequential keys
// (pipeline and PSSelector bitfields, etc.) went into a weak mixer with poor
// avalanche. XXH3 gives proper avalanche and is the fastest hash already
// vendored, so this is both more uniform and better distributed.
#ifndef XXH_versionNumber
	#define XXH_STATIC_LINKING_ONLY 1
	#define XXH_INLINE_ALL 1
	#include <xxhash.h>
#endif

// Fold one trivially-copyable value into seed using XXH3, seeded by the running
// seed so the combine is order-dependent. Every current caller passes integral
// keys; the static_assert keeps it that way (XXH3 hashes raw bytes, which is
// only meaningful for trivially-copyable types with no padding concerns here).
template <typename T>
static inline void HashCombine(std::size_t& seed, const T& v)
{
	static_assert(std::is_trivially_copyable<T>::value,
		"HashCombine requires trivially-copyable values");
	seed = static_cast<std::size_t>(
		XXH3_64bits_withSeed(&v, sizeof(T), static_cast<XXH64_hash_t>(seed)));
}

template <typename T, typename... Rest>
static inline void HashCombine(std::size_t& seed, const T& v, Rest&&... rest)
{
	HashCombine(seed, v);
	(HashCombine(seed, std::forward<Rest>(rest)), ...);
}
