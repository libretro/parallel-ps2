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

/* Every Vulkan function Granite and paraLLEl-GS call, by how it is
 * loaded. Include with the macro of each kind wanted defined:
 *   GRANITE_VK_LOADER    vkGetInstanceProcAddr(NULL, ...), global
 *   GRANITE_VK_INSTANCE  vkGetInstanceProcAddr(instance, ...), global
 *   GRANITE_VK_DEVICE    vkGetDeviceProcAddr(device, ...), in the table
 * Only these are loaded: a call to one not listed fails to compile. */

#ifdef GRANITE_VK_LOADER
GRANITE_VK_LOADER(vkCreateInstance)
GRANITE_VK_LOADER(vkEnumerateInstanceExtensionProperties)
GRANITE_VK_LOADER(vkEnumerateInstanceLayerProperties)
GRANITE_VK_LOADER(vkEnumerateInstanceVersion)
#endif

#ifdef GRANITE_VK_INSTANCE
GRANITE_VK_INSTANCE(vkCreateDevice)
GRANITE_VK_INSTANCE(vkDestroyInstance)
GRANITE_VK_INSTANCE(vkEnumerateDeviceExtensionProperties)
GRANITE_VK_INSTANCE(vkEnumeratePhysicalDevices)
GRANITE_VK_INSTANCE(vkGetDeviceProcAddr)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceExternalBufferProperties)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceFeatures2)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceFormatProperties2)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceImageFormatProperties2)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceMemoryProperties)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceMemoryProperties2)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceProperties)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceProperties2)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties2)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR)
GRANITE_VK_INSTANCE(vkGetPhysicalDeviceToolPropertiesEXT)
GRANITE_VK_INSTANCE(vkCreateDebugUtilsMessengerEXT)
GRANITE_VK_INSTANCE(vkDestroyDebugUtilsMessengerEXT)
GRANITE_VK_INSTANCE(vkSetDebugUtilsObjectNameEXT)
GRANITE_VK_INSTANCE(vkCmdBeginDebugUtilsLabelEXT)
GRANITE_VK_INSTANCE(vkCmdEndDebugUtilsLabelEXT)
GRANITE_VK_INSTANCE(vkCmdInsertDebugUtilsLabelEXT)
#endif

#ifdef GRANITE_VK_DEVICE
GRANITE_VK_DEVICE(vkAllocateCommandBuffers)
GRANITE_VK_DEVICE(vkAllocateDescriptorSets)
GRANITE_VK_DEVICE(vkAllocateMemory)
GRANITE_VK_DEVICE(vkBeginCommandBuffer)
GRANITE_VK_DEVICE(vkBindBufferMemory)
GRANITE_VK_DEVICE(vkBindImageMemory)
GRANITE_VK_DEVICE(vkBindImageMemory2)
GRANITE_VK_DEVICE(vkCmdBeginRenderPass)
GRANITE_VK_DEVICE(vkCmdBindDescriptorBuffersEXT)
GRANITE_VK_DEVICE(vkCmdBindDescriptorSets)
GRANITE_VK_DEVICE(vkCmdBindPipeline)
GRANITE_VK_DEVICE(vkCmdBindResourceHeapEXT)
GRANITE_VK_DEVICE(vkCmdBindSamplerHeapEXT)
GRANITE_VK_DEVICE(vkCmdBindVertexBuffers)
GRANITE_VK_DEVICE(vkCmdBlitImage)
GRANITE_VK_DEVICE(vkCmdCopyBuffer)
GRANITE_VK_DEVICE(vkCmdCopyBufferToImage)
GRANITE_VK_DEVICE(vkCmdCopyImage)
GRANITE_VK_DEVICE(vkCmdDispatch)
GRANITE_VK_DEVICE(vkCmdDispatchIndirect)
GRANITE_VK_DEVICE(vkCmdDraw)
GRANITE_VK_DEVICE(vkCmdEndRenderPass)
GRANITE_VK_DEVICE(vkCmdFillBuffer)
GRANITE_VK_DEVICE(vkCmdPipelineBarrier2)
GRANITE_VK_DEVICE(vkCmdPipelineBarrier2KHR)
GRANITE_VK_DEVICE(vkCmdPushConstants)
GRANITE_VK_DEVICE(vkCmdPushDataEXT)
GRANITE_VK_DEVICE(vkCmdPushDescriptorSetWithTemplate)
GRANITE_VK_DEVICE(vkCmdPushDescriptorSetWithTemplateKHR)
GRANITE_VK_DEVICE(vkCmdResetEvent2)
GRANITE_VK_DEVICE(vkCmdResetEvent2KHR)
GRANITE_VK_DEVICE(vkCmdSetBlendConstants)
GRANITE_VK_DEVICE(vkCmdSetDepthBias)
GRANITE_VK_DEVICE(vkCmdSetDescriptorBufferOffsetsEXT)
GRANITE_VK_DEVICE(vkCmdSetEvent2)
GRANITE_VK_DEVICE(vkCmdSetEvent2KHR)
GRANITE_VK_DEVICE(vkCmdSetScissor)
GRANITE_VK_DEVICE(vkCmdSetStencilCompareMask)
GRANITE_VK_DEVICE(vkCmdSetStencilReference)
GRANITE_VK_DEVICE(vkCmdSetStencilWriteMask)
GRANITE_VK_DEVICE(vkCmdSetViewport)
GRANITE_VK_DEVICE(vkCmdWaitEvents2)
GRANITE_VK_DEVICE(vkCmdWaitEvents2KHR)
GRANITE_VK_DEVICE(vkCopyMemoryToImage)
GRANITE_VK_DEVICE(vkCreateBuffer)
GRANITE_VK_DEVICE(vkCreateBufferView)
GRANITE_VK_DEVICE(vkCreateCommandPool)
GRANITE_VK_DEVICE(vkCreateComputePipelines)
GRANITE_VK_DEVICE(vkCreateDescriptorPool)
GRANITE_VK_DEVICE(vkCreateDescriptorSetLayout)
GRANITE_VK_DEVICE(vkCreateDescriptorUpdateTemplate)
GRANITE_VK_DEVICE(vkCreateFence)
GRANITE_VK_DEVICE(vkCreateFramebuffer)
GRANITE_VK_DEVICE(vkCreateGraphicsPipelines)
GRANITE_VK_DEVICE(vkCreateImage)
GRANITE_VK_DEVICE(vkCreateImageView)
GRANITE_VK_DEVICE(vkCreatePipelineBinariesKHR)
GRANITE_VK_DEVICE(vkCreatePipelineCache)
GRANITE_VK_DEVICE(vkCreatePipelineLayout)
GRANITE_VK_DEVICE(vkCreateRenderPass2)
GRANITE_VK_DEVICE(vkCreateRenderPass2KHR)
GRANITE_VK_DEVICE(vkCreateSampler)
GRANITE_VK_DEVICE(vkCreateSamplerYcbcrConversion)
GRANITE_VK_DEVICE(vkCreateSemaphore)
GRANITE_VK_DEVICE(vkCreateShaderModule)
GRANITE_VK_DEVICE(vkDestroyBuffer)
GRANITE_VK_DEVICE(vkDestroyBufferView)
GRANITE_VK_DEVICE(vkDestroyCommandPool)
GRANITE_VK_DEVICE(vkDestroyDescriptorPool)
GRANITE_VK_DEVICE(vkDestroyDescriptorSetLayout)
GRANITE_VK_DEVICE(vkDestroyDescriptorUpdateTemplate)
GRANITE_VK_DEVICE(vkDestroyDevice)
GRANITE_VK_DEVICE(vkDestroyFence)
GRANITE_VK_DEVICE(vkDestroyFramebuffer)
GRANITE_VK_DEVICE(vkDestroyImage)
GRANITE_VK_DEVICE(vkDestroyImageView)
GRANITE_VK_DEVICE(vkDestroyPipeline)
GRANITE_VK_DEVICE(vkDestroyPipelineBinaryKHR)
GRANITE_VK_DEVICE(vkDestroyPipelineCache)
GRANITE_VK_DEVICE(vkDestroyPipelineLayout)
GRANITE_VK_DEVICE(vkDestroyRenderPass)
GRANITE_VK_DEVICE(vkDestroySampler)
GRANITE_VK_DEVICE(vkDestroySamplerYcbcrConversion)
GRANITE_VK_DEVICE(vkDestroySemaphore)
GRANITE_VK_DEVICE(vkDestroyShaderModule)
GRANITE_VK_DEVICE(vkDeviceWaitIdle)
GRANITE_VK_DEVICE(vkEndCommandBuffer)
GRANITE_VK_DEVICE(vkFlushMappedMemoryRanges)
GRANITE_VK_DEVICE(vkFreeCommandBuffers)
GRANITE_VK_DEVICE(vkFreeMemory)
GRANITE_VK_DEVICE(vkGetBufferDeviceAddress)
GRANITE_VK_DEVICE(vkGetBufferMemoryRequirements2)
GRANITE_VK_DEVICE(vkGetDescriptorEXT)
GRANITE_VK_DEVICE(vkGetDescriptorSetLayoutBindingOffsetEXT)
GRANITE_VK_DEVICE(vkGetDescriptorSetLayoutSizeEXT)
GRANITE_VK_DEVICE(vkGetDeviceQueue)
GRANITE_VK_DEVICE(vkGetImageMemoryRequirements)
GRANITE_VK_DEVICE(vkGetImageMemoryRequirements2)
GRANITE_VK_DEVICE(vkGetPipelineBinaryDataKHR)
GRANITE_VK_DEVICE(vkGetPipelineCacheData)
GRANITE_VK_DEVICE(vkGetPipelineKeyKHR)
GRANITE_VK_DEVICE(vkGetSemaphoreCounterValue)
GRANITE_VK_DEVICE(vkInvalidateMappedMemoryRanges)
GRANITE_VK_DEVICE(vkMapMemory)
GRANITE_VK_DEVICE(vkQueueSubmit2)
GRANITE_VK_DEVICE(vkQueueSubmit2KHR)
GRANITE_VK_DEVICE(vkQueueWaitIdle)
GRANITE_VK_DEVICE(vkReleaseCapturedPipelineDataKHR)
GRANITE_VK_DEVICE(vkResetCommandPool)
GRANITE_VK_DEVICE(vkResetDescriptorPool)
GRANITE_VK_DEVICE(vkResetFences)
GRANITE_VK_DEVICE(vkTransitionImageLayout)
GRANITE_VK_DEVICE(vkTrimCommandPool)
GRANITE_VK_DEVICE(vkUnmapMemory)
GRANITE_VK_DEVICE(vkUpdateDescriptorSetWithTemplate)
GRANITE_VK_DEVICE(vkUpdateDescriptorSets)
GRANITE_VK_DEVICE(vkWaitForFences)
GRANITE_VK_DEVICE(vkWaitSemaphores)
GRANITE_VK_DEVICE(vkWriteResourceDescriptorsEXT)
GRANITE_VK_DEVICE(vkWriteSamplerDescriptorsEXT)
#endif
