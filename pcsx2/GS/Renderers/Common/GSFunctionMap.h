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

#include <unordered_map>

template <class KEY, class VALUE>
class GSFunctionMap
{
protected:
	struct ActivePtr
	{
		u64 frame;
		VALUE f;
	};

	std::unordered_map<KEY, ActivePtr*> m_map_active;

	ActivePtr* m_active;

	virtual VALUE GetDefaultFunction(KEY key) = 0;

public:
	GSFunctionMap()
		: m_active(NULL)
	{
	}

	virtual ~GSFunctionMap()
	{
		for (auto& i : m_map_active)
			delete i.second;
	}

	VALUE operator[](KEY key)
	{
		m_active = NULL;

		auto it = m_map_active.find(key);

		if (it != m_map_active.end())
			m_active = it->second;
		else
		{
			ActivePtr* p = new ActivePtr();

			memset(p, 0, sizeof(*p));

			p->frame = (u64)-1;

			p->f = GetDefaultFunction(key);

			m_map_active[key] = p;

			m_active = p;
		}

		return m_active->f;
	}
};


template <class CG, class KEY, class VALUE>
class GSCodeGeneratorFunctionMap : public GSFunctionMap<KEY, VALUE>
{
	std::unordered_map<u64, VALUE> m_cgmap;

	/* The room a function is generated into first, and the most it is
	 * given: a generator stops writing at the end of its room, so a
	 * function that fills it is generated again in twice the room. */
	enum { MIN_SIZE = 8192, MAX_SIZE = 65536 };

public:
	GSCodeGeneratorFunctionMap() { }

	~GSCodeGeneratorFunctionMap() = default;

	/* Forgets every function, for a reset of the code reserve: none of
	 * them is handed out again. */
	void Clear()
	{
		m_cgmap.clear();
		for (auto& i : this->m_map_active)
			delete i.second;
		this->m_map_active.clear();
		this->m_active = NULL;
	}

	/* NULL when the code reserve has no room left for the function. */
	VALUE GetDefaultFunction(KEY key)
	{
		auto i = m_cgmap.find(key);

		if (i != m_cgmap.end())
			return i->second;

		for (size_t room = MIN_SIZE; room <= MAX_SIZE; room *= 2)
		{
			u8* code_ptr = gs_code_reserve_reserve(room);
			if (!code_ptr)
				return nullptr;

			CG cg(key, code_ptr, room);
			if (cg.getSize() >= room)
				continue; /* cut short at the end of its room */

			gs_code_reserve_commit(cg.getSize());

			VALUE ret = (VALUE)cg.getCode();
			m_cgmap[key] = ret;
			return ret;
		}
		return nullptr;
	}
};
