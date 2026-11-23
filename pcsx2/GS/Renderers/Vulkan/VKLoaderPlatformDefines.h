/* SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team */
/* SPDX-License-Identifier: LGPL-3.0+ */
#pragma once

#ifdef _WIN32
/* vulkan.h pulls in windows.h on Windows, so ours comes first, lean and
 * without min/max. Nothing outside this directory: Granite includes this
 * with only its own include paths. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
