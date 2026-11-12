// SPDX-FileCopyrightText: 2024 Hans-Kristian Arntzen
// SPDX-License-Identifier: LGPL-3.0+

#pragma once

#include "../Vulkan/VKLoaderPlatformDefines.h"
#include "SaveState.h"
#include "Config.h"
#include "gs_interface.hpp"
#include "device.hpp"
#include "context.hpp"

#include <libretro_vulkan.h>

class GSRendererPGS final
{
public:
	explicit GSRendererPGS(u8 *basemem);
	~GSRendererPGS();

	bool Init();
	void Reset(bool hardware_reset);

	void Transfer(const u8 *mem, u32 size);

	void VSync(u32 field, bool registers_written);
	inline ParallelGS::GSInterface &get_interface() { return iface; };
	void ReadFIFO(u8 *mem, u32 size);

	void UpdateConfig();

	int Freeze(freezeData *data, bool sizeonly);
	int Defrost(freezeData *data);

	u8 *GetRegsMem();

private:
	ParallelGS::PrivRegisterState *priv;
	Vulkan::Device dev;
	ParallelGS::GSInterface iface;

	/* The device's pipeline cache and the list of pipelines in use, as
	 * read at Init: kept to merge into, and to leave a cache file that
	 * has not changed alone. */
	uint8_t *pipeline_blob       = nullptr;
	size_t pipeline_blob_size    = 0;
	Vulkan::Device::ComputePipelineRecord *pipeline_list = nullptr;
	unsigned pipeline_list_count = 0;
	Vulkan::Device::GraphicsPipelineRecord *pipeline_list_graphics = nullptr;
	unsigned pipeline_list_graphics_count = 0;
	bool pipelines_persist       = false;
	void load_pipelines();
	void save_pipelines();

	ParallelGS::SuperSampling current_super_sampling = ParallelGS::SuperSampling::X1;
	bool current_ordered_super_sampling     = false;
	bool current_super_sample_textures      = false;

	/* The geometry last given to the frontend, this session's: a session
	 * after it starts from the frontend's av_info, not from these. */
	retro_game_geometry geom  = {};
	float last_aspect         = 0.0f;
	uint32_t last_base_width  = 0;
	uint32_t last_base_height = 0;

	static int GetSaveStateSize();
};

/* The one renderer object, made and freed by GS.cpp; GSRendererPGS.cpp's
 * op table calls through it. Declared here, once, so both see the same
 * type: MSVC names a variable by its type, so two different declarations
 * do not link. */
extern GSRendererPGS* g_pgs_renderer;

// libretro integration.
/* What GS.cpp calls this renderer through; see gs_renderer_ops in GS.h. */
extern const struct gs_renderer_ops pgs_renderer_ops;

/* pgs_* context negotiation: GSLibretroHooks.h. */
#include "../../GSLibretroHooks.h"
