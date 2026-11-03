/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include "../SW/GSScanlineEnvironment.h"
#include "../../GSExtra.h"

#include "GSCodeReserve.h"
#include "GSFlatMap.h"
#include "GSKeyHash.h"

#include <stdlib.h>

/* The functions a code generator has made, by the selector each was made
 * for, in a GSFlatMap from the selector to the function: a draw's lookup
 * reads one slot. A selector the generator could not make a function for
 * (the code reserve full) is kept with none, so it is not tried again
 * until Clear. One map per rasterizer, as before: not shared. */
template <class CG, class KEY, class VALUE>
class GSCodeGeneratorFunctionMap
{
	gs_flat_map_t m_map;

	/* The room a function is generated into first, and the most it is
	 * given: a generator stops writing at the end of its room, so a
	 * function that fills it is generated again in twice the room. */
	enum { MIN_SIZE = 8192, MAX_SIZE = 65536 };

	/* NULL when the code reserve has no room left for the function. */
	VALUE Generate(KEY key)
	{
		for (size_t room = MIN_SIZE; room <= MAX_SIZE; room *= 2)
		{
			u8* code_ptr = gs_code_reserve_reserve(room);
			if (!code_ptr)
				return nullptr;

			CG cg(key, code_ptr, room);
			if (cg.getSize() >= room)
				continue; /* cut short at the end of its room */

			gs_code_reserve_commit(cg.getSize());
			return (VALUE)cg.getCode();
		}
		return nullptr;
	}

public:
	GSCodeGeneratorFunctionMap()
	{
		static_assert(sizeof(KEY) % sizeof(size_t) == 0, "the key fills whole words");
		gs_flat_map_init(&m_map, sizeof(KEY), sizeof(VALUE));
	}

	~GSCodeGeneratorFunctionMap()
	{
		gs_flat_map_free(&m_map);
	}

	/* Forgets every function, for a reset of the code reserve: none of
	 * them is handed out again. */
	void Clear()
	{
		gs_flat_map_clear(&m_map);
	}

	VALUE operator[](KEY key)
	{
		const size_t hash = gs_key_hash(&key, sizeof(key));
		VALUE* const found = static_cast<VALUE*>(gs_flat_map_find(&m_map, &key, sizeof(key), hash));
		if (found)
			return *found;
		{
			const VALUE f = Generate(key);
			int inserted;
			VALUE* const slot = static_cast<VALUE*>(gs_flat_map_insert(&m_map, &key, hash, &inserted));
			if (!slot)
				abort(); /* out of memory, as the map's operator[] did without exceptions */
			*slot = f;
			return f;
		}
	}
};
