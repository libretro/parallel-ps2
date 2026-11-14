/* The builders' calls as GSDeviceVK and GSTextureVK make them, and a few
 * around them, with picked values: through the C builders (main.c) and
 * the C++ classes they replaced (the oracle), the device sees the same
 * create infos. Every value is picked into a local first. */
static void scenario(VkDevice dev)
{
	VkPipeline made[64];
	unsigned num_made = 0;
	unsigned round;

	for (round = 0; round < 600; round++)
	{
		const unsigned kind = R(7);
		if (kind <= 2)
		{
			GP_DECL;
			VkSpecializationMapEntry vmap[4], fmap[4];
			u32 vdata[4], fdata[4];
			VkSpecializationInfo vsi, fsi;
			VkStencilOpState front, back;
			unsigned i, n, pick;
			u32 a, b, c, d, e, f, g, h, k;
			VkPipeline p;
			GP_BEGIN;

			pick = R(2);
			if (pick)
				GP_PROV(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
			a = R(5);
			GP_LAYOUT(HDL(VkPipelineLayout, a));
			if (kind == 0)
			{
				/* a utility pipeline */
				GP_VB(0, 32);
				GP_VA(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
				GP_VA(1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
				GP_TOPO(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
				GP_DYN_VS;
				GP_DYN(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
				GP_DYN(VK_DYNAMIC_STATE_LINE_WIDTH);
				GP_NOCULL;
				a = R(40);
				GP_RP(HDL(VkRenderPass, 100 + a), 0);
				a = R(30); b = R(30);
				GP_VS(HDL(VkShaderModule, 200 + a), NULL);
				GP_FS(HDL(VkShaderModule, 300 + b), NULL);
				pick = R(5);
				if (pick == 0)
				{
					memset(&front, 0, sizeof(front));
					front.failOp = VK_STENCIL_OP_KEEP; front.passOp = VK_STENCIL_OP_REPLACE;
					front.compareOp = VK_COMPARE_OP_ALWAYS; front.compareMask = front.writeMask = front.reference = 1;
					GP_STENCIL(1, &front, &front);
				}
				else if (pick == 1)
				{
					a = R(2);
					GP_DEPTH(a, a, VK_COMPARE_OP_ALWAYS);
					b = R(16);
					GP_MASK(0, b);
				}
				else if (pick == 2)
					GP_BLEND(0, 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
						VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);
				else if (pick == 3)
					GP_BLEND(0, 1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
						VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 15u);
			}
			else
			{
				/* a draw pipeline, or a library of some of its parts */
				a = R(50); b = R(2);
				GP_RP(HDL(VkRenderPass, 500 + a), b);
				a = R(5);
				GP_TOPO((VkPrimitiveTopology)a);
				a = R(3); b = R(4); c = R(2);
				GP_RAST((VkPolygonMode)a, (VkCullModeFlags)b, (VkFrontFace)c);
				pick = R(2);
				if (pick)
					GP_LINE(VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT);
				pick = R(3);
				if (pick)
					GP_PROV((VkProvokingVertexModeEXT)R(2));
				GP_DYN_VS;
				GP_DYN(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
				GP_DYN(VK_DYNAMIC_STATE_LINE_WIDTH);
				pick = R(2);
				if (pick)
				{
					GP_DYN(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT);
					GP_DYN(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT);
					GP_DYN(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT);
					GP_DYN(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT);
					GP_DYN(VK_DYNAMIC_STATE_STENCIL_OP_EXT);
				}
				n = 1 + R(4);
				for (i = 0; i < n; i++)
				{
					vmap[i].constantID = R(8); vmap[i].offset = 4u * i; vmap[i].size = 4;
					vdata[i] = R(1000);
					fmap[i].constantID = R(8); fmap[i].offset = 4u * i; fmap[i].size = 4;
					fdata[i] = R(1000);
				}
				vsi.mapEntryCount = n; vsi.pMapEntries = vmap; vsi.dataSize = 4u * n; vsi.pData = vdata;
				fsi.mapEntryCount = n; fsi.pMapEntries = fmap; fsi.dataSize = 4u * n; fsi.pData = fdata;
				pick = R(4);
				if (pick != 1)
				{
					a = R(30);
					GP_VS(HDL(VkShaderModule, 400 + a), &vsi);
				}
				if (pick != 2)
				{
					a = R(30); b = R(2);
					GP_FS(HDL(VkShaderModule, 600 + a), b ? NULL : &fsi);
				}
				if (pick == 3)
				{
					/* a stage set twice: the second replaces the first */
					a = R(30);
					GP_VS(HDL(VkShaderModule, 700 + a), NULL);
				}
				pick = R(2);
				if (pick)
				{
					GP_VB(0, 32);
					for (i = 0; i < 7; i++)
					{
						a = R(60); b = 4u * i;
						GP_VA(i, 0, (VkFormat)a, b);
					}
				}
				a = R(2); b = R(2); c = R(8);
				GP_DEPTH(a, b, (VkCompareOp)c);
				a = R(2);
				memset(&front, 0, sizeof(front));
				memset(&back, 0, sizeof(back));
				front.failOp = (VkStencilOp)R(8); front.passOp = (VkStencilOp)R(8);
				front.depthFailOp = (VkStencilOp)R(8); front.compareOp = (VkCompareOp)R(8);
				front.compareMask = R(256); front.writeMask = R(256); front.reference = R(256);
				back = front;
				back.reference = R(256);
				GP_STENCIL(a, &front, &back);
				a = R(2); b = R(19); c = R(19); d = R(5); e = R(19); f = R(19); g = R(5); h = R(16);
				k = R(2);
				GP_BLEND(k, a, (VkBlendFactor)b, (VkBlendFactor)c, (VkBlendOp)d, (VkBlendFactor)e, (VkBlendFactor)f,
					(VkBlendOp)g, h);
				pick = R(3);
				if (pick == 0)
					GP_FLAGS(VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT);
				pick = R(3);
				if (pick == 0)
				{
					a = 1 + R(15);
					GP_LIB(a);
				}
			}
			GP_CREATE(dev, HDL(VkPipelineCache, 9), p);
			HN(p);
			if (num_made < 64)
				made[num_made++] = p;
		}
		else if (kind == 3 && num_made >= 3)
		{
			/* a pipeline linked from three libraries */
			GP_DECL;
			VkPipeline libs[4];
			u32 a, n;
			VkPipeline p;
			GP_BEGIN;
			a = R(5);
			GP_LAYOUT(HDL(VkPipelineLayout, a));
			libs[0] = made[R(num_made)];
			libs[1] = made[R(num_made)];
			libs[2] = made[R(num_made)];
			libs[3] = made[R(num_made)];
			n = 1 + R(4);
			GP_LINKED(libs, n);
			GP_CREATE(dev, HDL(VkPipelineCache, 3), p);
			HN(p);
		}
		else if (kind == 4)
		{
			/* descriptor set layouts, and a pipeline layout of them */
			DSL_DECL;
			PL_DECL;
			VkDescriptorSetLayout layouts[3];
			unsigned i, j, nsets, nb;
			u32 a, b, c;
			DSL_BEGIN;
			PL_BEGIN;
			nsets = 1 + R(3);
			for (i = 0; i < nsets; i++)
			{
				nb = 1 + R(4);
				for (j = 0; j < nb; j++)
				{
					a = R(11); b = 1 + R(4); c = 1 + R(31);
					DSL_ADD(j, (VkDescriptorType)a, b, (VkShaderStageFlags)c);
				}
				DSL_CREATE(dev, layouts[i]);
				HN(layouts[i]);
			}
			a = R(2);
			if (a)
			{
				b = 1 + R(31); c = 4u * (1 + R(32));
				PL_PUSH((VkShaderStageFlags)b, 0, c);
			}
			for (i = 0; i < nsets; i++)
				PL_SET(layouts[i]);
			{
				VkPipelineLayout pl;
				PL_CREATE(dev, pl);
				HN(pl);
			}
		}
		else if (kind == 5)
		{
			/* descriptor writes, two updates from one builder */
			DSU_DECL;
			unsigned u, i, n;
			u32 a, b, c, d, e;
			DSU_BEGIN;
			for (u = 0; u < 2; u++)
			{
				n = R(3);
				for (i = 0; i < n; i++)
				{
					a = R(5); b = R(4); c = R(90); d = R(9); e = R(4);
					if (e == 0)
						DSU_CIS(HDL(VkDescriptorSet, a), b, HDL(VkImageView, c), HDL(VkSampler, d), (VkImageLayout)(d % 8));
					else if (e == 1)
						DSU_IMG(HDL(VkDescriptorSet, a), b, HDL(VkImageView, c), (VkImageLayout)(d % 8));
					else
						DSU_IA(HDL(VkDescriptorSet, a), b, HDL(VkImageView, c), (VkImageLayout)(d % 8));
				}
				n = R(3);
				for (i = 0; i < n; i++)
				{
					a = R(5); b = R(4); c = R(11); d = R(4096); e = 1 + R(4096);
					DSU_BUF(HDL(VkDescriptorSet, a), b, (VkDescriptorType)c, HDL(VkBuffer, 7), d, e);
				}
				DSU_UPDATE(dev);
			}
		}
		else if (kind == 6)
		{
			/* a framebuffer, with a depth attachment or not */
			FB_DECL;
			u32 a, b, c, d;
			VkFramebuffer fb;
			a = R(40); b = 1 + R(2048); c = 1 + R(2048);
			FB_BEGIN(HDL(VkRenderPass, a), b, c);
			d = R(100);
			FB_ADD(HDL(VkImageView, d));
			d = R(3);
			if (d)
				FB_ADD(HDL(VkImageView, 200 + d));
			FB_CREATE(dev, fb);
			HN(fb);
		}
		else
		{
			/* a feature chain: appended in order, never twice */
			VkBaseOutStructure head, nodes[4];
			const VkBaseOutStructure* s;
			unsigned i, n;
			memset(&head, 0, sizeof(head));
			memset(nodes, 0, sizeof(nodes));
			for (i = 0; i < 4; i++)
				nodes[i].sType = (VkStructureType)(1000 + i);
			n = 1 + R(6);
			for (i = 0; i < n; i++)
			{
				const unsigned at = R(4);
				CHAIN(&head, &nodes[at]);
			}
			for (s = head.pNext; s; s = s->pNext)
				HU(s->sType);
			HU(0xc4a1);
		}
	}
}
