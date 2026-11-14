/* SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team */
/* SPDX-License-Identifier: LGPL-3.0+ */
#pragma once

#ifdef _WIN32
/* vulkan.h pulls in windows.h on Windows, so ours comes first; C units
 * (the loader) take windows.h with the same two settings, without
 * RedtapeWindows.h's C++ helpers. */
#ifdef __cplusplus
#include "common/RedtapeWindows.h"
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif
