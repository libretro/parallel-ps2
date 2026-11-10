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

#include <vector>
#include <unordered_map>

/* The socket adapter's sessions by key. Touched by the network thread
 * alone (net.cpp), so it is a plain map. */
template <class Key, class T>
class SessionMap
{
	std::unordered_map<Key, T> map;

public:
	void Add(Key key, T value) { map[key] = value; }
	void Remove(Key key) { map.erase(key); }
	void Clear() { map.clear(); }

	/* A copy: a caller may remove the key it is visiting. */
	std::vector<Key> GetKeys()
	{
		std::vector<Key> keys;
		keys.reserve(map.size());
		for (auto iter = map.begin(); iter != map.end(); ++iter)
			keys.push_back(iter->first);
		return keys;
	}

	/* Does not insert when the key is missing. */
	bool TryGetValue(Key key, T* value)
	{
		auto search = map.find(key);
		if (search == map.end())
			return false;
		*value = search->second;
		return true;
	}

	bool ContainsKey(Key key) { return map.find(key) != map.end(); }
};
