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

/*
 * A doubly linked list in one array, in C: elements of a fixed size, each
 * followed by its next and prev indexes, chained through index 0, which
 * is the head and also "past the end". A position is a u16 index that
 * stays valid until that element is erased (the array grows by copying,
 * in order), which is what lets a caller keep it to erase or move by.
 *
 * The texture caches keep one per GS page and erase by the index they
 * kept; the device keeps its texture pool in two.
 *
 * An empty list owns nothing: the array is made at the first insertion,
 * since of GS_MAX_PAGES lists a frame touches a handful.
 *
 * Insert, erase and move are static in the header, as the flat map's
 * find is. Not thread safe.
 */

#ifndef GS_FAST_LIST_H
#define GS_FAST_LIST_H

#include <stddef.h>
#include <string.h>

#include <retro_inline.h>

#include "common/Pcsx2Defs.h"

/* Build with -DFASTLIST_CHECKS=1 to have a stale index say so, with the
 * list and the index, instead of being ignored. The guards themselves are
 * always on; this only makes them audible. */
#ifndef FASTLIST_CHECKS
#define FASTLIST_CHECKS 0
#endif
#if FASTLIST_CHECKS
#include <stdio.h>
#define GS_FAST_LIST_REPORT(l, op, index) \
	fprintf(stderr, "FastList %p: %s(%u) with capacity %u, %u held - stale index\n", (const void*)(l), (op), \
		(unsigned)(index), (unsigned)(l)->capacity, (unsigned)(l)->count)
#else
#define GS_FAST_LIST_REPORT(l, op, index) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs_fast_list
{
	/* capacity elements of stride bytes: the data, then at link the next
	 * and prev indexes; then the free indexes, capacity - 1 of them */
	unsigned char* buf;
	u16 capacity;
	u16 count; /* elements held, and the top of the free stack */
	u16 elem_size;
	u16 stride;
	u16 link;
} gs_fast_list;

#define GS_FAST_LIST_ELEM(l, i) ((l)->buf + (size_t)(i) * (l)->stride)
#define GS_FAST_LIST_NEXT(l, i) (((u16*)(GS_FAST_LIST_ELEM(l, i) + (l)->link))[0])
#define GS_FAST_LIST_PREV(l, i) (((u16*)(GS_FAST_LIST_ELEM(l, i) + (l)->link))[1])
#define GS_FAST_LIST_FREE(l)    ((u16*)((l)->buf + (size_t)(l)->capacity * (l)->stride))

/* An empty list of elements of this size. Allocates nothing. */
void gs_fast_list_init(gs_fast_list* l, size_t elem_size);
/* Empty again, owning nothing */
void gs_fast_list_clear(gs_fast_list* l);
/* (the first insertion's array, or twice the room: zero if it cannot be
 * had, the list as it was) */
int gs_fast_list_grow(gs_fast_list* l);

/* The data at an index, and the indexes either side of it (0: none) */
static INLINE void* gs_fast_list_data(const gs_fast_list* l, u16 index)
{
	return GS_FAST_LIST_ELEM(l, index);
}
static INLINE u16 gs_fast_list_next(const gs_fast_list* l, u16 index)
{
	return GS_FAST_LIST_NEXT(l, index);
}
static INLINE u16 gs_fast_list_prev(const gs_fast_list* l, u16 index)
{
	return GS_FAST_LIST_PREV(l, index);
}
/* (index 0 is the head and past the end, so a list with no array answers
 * 0 to both without one to read) */
static INLINE u16 gs_fast_list_first(const gs_fast_list* l)
{
	return l->buf ? GS_FAST_LIST_NEXT(l, 0) : 0;
}
static INLINE u16 gs_fast_list_last(const gs_fast_list* l)
{
	return l->buf ? GS_FAST_LIST_PREV(l, 0) : 0;
}

static INLINE void gs_fast_list_link_front(gs_fast_list* l, u16 index)
{
	const u16 head = GS_FAST_LIST_NEXT(l, 0);
	GS_FAST_LIST_PREV(l, index) = 0;
	GS_FAST_LIST_NEXT(l, index) = head;
	GS_FAST_LIST_PREV(l, head)  = index;
	GS_FAST_LIST_NEXT(l, 0)     = index;
}

static INLINE void gs_fast_list_unlink(gs_fast_list* l, u16 index)
{
	const u16 next = GS_FAST_LIST_NEXT(l, index);
	const u16 prev = GS_FAST_LIST_PREV(l, index);
	GS_FAST_LIST_NEXT(l, prev) = next;
	GS_FAST_LIST_PREV(l, next) = prev;
}

/* @data copied in at the front; its index, or 0 when there is no room
 * (out of memory, or 65534 held) and the list is as it was */
static INLINE u16 gs_fast_list_insert_front(gs_fast_list* l, const void* data)
{
	u16 index;
	if ((!l->buf || l->count == l->capacity - 1) && !gs_fast_list_grow(l))
		return 0;
	index = GS_FAST_LIST_FREE(l)[l->count++];
	memcpy(GS_FAST_LIST_ELEM(l, index), data, l->elem_size);
	gs_fast_list_link_front(l, index);
	return index;
}

/* An index kept from an insertion, erased. One that is not an index - 0,
 * past the array, or into a list holding nothing (one emptied since it
 * was kept: a texture cache purge) - is ignored: the free stack's top
 * would otherwise wrap below zero and the push land far past the array. */
static INLINE void gs_fast_list_erase(gs_fast_list* l, u16 index)
{
	if (!l->buf || index == 0 || index >= l->capacity || l->count == 0)
	{
		GS_FAST_LIST_REPORT(l, "EraseIndex", index);
		return;
	}
	gs_fast_list_unlink(l, index);
	GS_FAST_LIST_FREE(l)[--l->count] = index;
}

/* An index kept from an insertion, made the first. A stale one is ignored,
 * as by erase. */
static INLINE void gs_fast_list_move_front(gs_fast_list* l, u16 index)
{
	if (!l->buf || index == 0 || index >= l->capacity)
	{
		GS_FAST_LIST_REPORT(l, "MoveFront", index);
		return;
	}
	if (GS_FAST_LIST_NEXT(l, 0) != index)
	{
		gs_fast_list_unlink(l, index);
		gs_fast_list_link_front(l, index);
	}
}

#ifdef __cplusplus
}

#include <type_traits>

template <class T>
class FastListIterator;

template <class T>
class FastListReverseIterator;

/* The C list, with the C++ caches' names. T is copied in as bytes. */
template <class T>
class FastList
{
	friend class FastListIterator<T>;
	friend class FastListReverseIterator<T>;

	static_assert(std::is_trivially_copyable<T>::value, "elements are copied as bytes");

	gs_fast_list m_l;

public:
	__forceinline FastList() { gs_fast_list_init(&m_l, sizeof(T)); }
	__forceinline ~FastList() { gs_fast_list_clear(&m_l); }
	FastList(const FastList&) = delete;
	FastList& operator=(const FastList&) = delete;

	void clear() { gs_fast_list_clear(&m_l); }

	/* (0 when there is no room: an index erase and move ignore) */
	__forceinline u16 InsertFront(const T& data) { return gs_fast_list_insert_front(&m_l, &data); }
	__forceinline void push_front(const T& data) { gs_fast_list_insert_front(&m_l, &data); }

	__forceinline const T& back() const { return Data(gs_fast_list_last(&m_l)); }
	__forceinline void pop_back() { gs_fast_list_erase(&m_l, gs_fast_list_last(&m_l)); }

	__forceinline u16 size() const { return m_l.count; }
	__forceinline bool empty() const { return m_l.count == 0; }

	__forceinline void EraseIndex(const u16 index) { gs_fast_list_erase(&m_l, index); }
	__forceinline void MoveFront(const u16 index) { gs_fast_list_move_front(&m_l, index); }

	__forceinline const FastListIterator<T> begin() const { return FastListIterator<T>(this, gs_fast_list_first(&m_l)); }
	__forceinline const FastListIterator<T> end() const { return FastListIterator<T>(this, 0); }

	__forceinline FastListIterator<T> erase(FastListIterator<T> i)
	{
		/* (the next index read before the element goes back on the stack) */
		const u16 index = i.Index();
		++i;
		EraseIndex(index);
		return i;
	}

	__forceinline const FastListReverseIterator<T> rbegin() const
	{
		return FastListReverseIterator<T>(this, gs_fast_list_last(&m_l));
	}
	__forceinline const FastListReverseIterator<T> rend() const { return FastListReverseIterator<T>(this, 0); }

private:
	__forceinline const T& Data(const u16 index) const
	{
		return *static_cast<const T*>(gs_fast_list_data(&m_l, index));
	}
	__forceinline u16 NextIndex(const u16 index) const { return gs_fast_list_next(&m_l, index); }
	__forceinline u16 PrevIndex(const u16 index) const { return gs_fast_list_prev(&m_l, index); }
};

/* (const iterators, by index: an erase elsewhere in the list leaves them
 * valid, as does growth) */
template <class T>
class FastListIterator
{
	const FastList<T>* m_fastlist;
	u16 m_index;

public:
	__forceinline FastListIterator(const FastList<T>* fastlist, const u16 index)
		: m_fastlist(fastlist)
		, m_index(index)
	{
	}
	__forceinline bool operator!=(const FastListIterator<T>& other) const { return m_index != other.m_index; }
	__forceinline bool operator==(const FastListIterator<T>& other) const { return m_index == other.m_index; }
	__forceinline const FastListIterator<T>& operator++()
	{
		m_index = m_fastlist->NextIndex(m_index);
		return *this;
	}
	__forceinline const FastListIterator<T> operator++(int)
	{
		FastListIterator<T> copy(*this);
		++(*this);
		return copy;
	}
	__forceinline const FastListIterator<T>& operator--()
	{
		m_index = m_fastlist->PrevIndex(m_index);
		return *this;
	}
	__forceinline const FastListIterator<T> operator--(int)
	{
		FastListIterator<T> copy(*this);
		--(*this);
		return copy;
	}
	__forceinline const T& operator*() const { return m_fastlist->Data(m_index); }
	__forceinline u16 Index() const { return m_index; }
};

template <class T>
class FastListReverseIterator
{
	const FastList<T>* m_fastlist;
	u16 m_index;

public:
	__forceinline FastListReverseIterator(const FastList<T>* fastlist, const u16 index)
		: m_fastlist(fastlist)
		, m_index(index)
	{
	}
	__forceinline bool operator!=(const FastListReverseIterator<T>& other) const { return m_index != other.m_index; }
	__forceinline bool operator==(const FastListReverseIterator<T>& other) const { return m_index == other.m_index; }
	__forceinline const FastListReverseIterator<T>& operator++()
	{
		m_index = m_fastlist->PrevIndex(m_index);
		return *this;
	}
	__forceinline const FastListReverseIterator<T> operator++(int)
	{
		FastListReverseIterator<T> copy(*this);
		++(*this);
		return copy;
	}
	__forceinline const FastListReverseIterator<T>& operator--()
	{
		m_index = m_fastlist->NextIndex(m_index);
		return *this;
	}
	__forceinline const FastListReverseIterator<T> operator--(int)
	{
		FastListReverseIterator<T> copy(*this);
		--(*this);
		return copy;
	}
	__forceinline const T& operator*() const { return m_fastlist->Data(m_index); }
	__forceinline u16 Index() const { return m_index; }
};
#endif

#endif
