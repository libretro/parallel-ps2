/* Copyright (c) 2017-2026 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "aligned_alloc.hpp"
#include <string.h>
#include <memory>
#include <algorithm>
#include <type_traits>

namespace Util
{
template <typename T>
class DynamicArray
{
public:
	// Only POD-like types work here since we don't invoke placement new or delete.
	static_assert(std::is_trivially_default_constructible<T>::value, "T must be trivially constructible.");
	static_assert(std::is_trivially_destructible<T>::value, "T must be trivially destructible.");

	// Returns false if the allocation failed, leaving the array untouched.
	// The old code reset buffer to whatever memalign_alloc returned without
	// checking it, so a failed allocation left data() null for good and every
	// later read memcpy'd from a null-derived pointer. Seen as an access
	// violation inside read_transfer_fifo, copying from a wild source into the
	// fixed EE mapping, after a transfer asked for an implausible size.
	bool reserve(size_t n)
	{
		size_t grown;
		size_t align;
		T *new_ptr;

		if (n <= N)
			return true;

		grown = N * 3 / 2;
		if (grown > n)
			n = grown;

		align = alignof(T) > 64 ? alignof(T) : 64;

		new_ptr = static_cast<T *>(::Util::memalign_alloc(align, n * sizeof(T)));
		if (!new_ptr)
			return false;

		if (buffer)
			memcpy(new_ptr, buffer.get(), N * sizeof(T));

		buffer.reset(new_ptr);
		N = n;
		return true;
	}

	T &operator[](size_t index) { return buffer.get()[index]; }
	const T &operator[](size_t index) const { return buffer.get()[index]; }
	T *data() { return buffer.get(); }
	const T *data() const { return buffer.get(); }
	size_t get_capacity() const { return N; }

private:
	std::unique_ptr<T, AlignedDeleter> buffer;
	size_t N = 0;
};
}
