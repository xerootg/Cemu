#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"

LatteTextureVk::LatteTextureVk(class VulkanRenderer* vkRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle,
	Latte::E_HWTILEMODE tileMode, bool isDepth)
	: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth), m_vkr(vkRenderer)
{
	vkObjTex = new VKRObjectTexture();

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	
	sint32 effectiveBaseWidth = width;
	sint32 effectiveBaseHeight = height;
	sint32 effectiveBaseDepth = depth;
	if (overwriteInfo.hasResolutionOverwrite)
	{
		effectiveBaseWidth = overwriteInfo.width;
		effectiveBaseHeight = overwriteInfo.height;
		effectiveBaseDepth = overwriteInfo.depth;
	}
	effectiveBaseDepth = std::max(1, effectiveBaseDepth);

	imageInfo.extent.width = effectiveBaseWidth;
	imageInfo.extent.height = effectiveBaseHeight;
	imageInfo.mipLevels = mipLevels;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	if (dim == Latte::E_DIM::DIM_3D)
	{
		imageInfo.extent.depth = effectiveBaseDepth;
		imageInfo.arrayLayers = 1;
		imageInfo.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
	}
	else
	{
		imageInfo.extent.depth = 1;
		imageInfo.arrayLayers = effectiveBaseDepth;
		if (dim != Latte::E_DIM::DIM_1D && (effectiveBaseDepth % 6) == 0 && effectiveBaseWidth == effectiveBaseHeight)
			imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}
	
	VulkanRenderer::FormatInfoVK texFormatInfo;
	vkRenderer->GetTextureFormatInfoVK(format, isDepth, dim, effectiveBaseWidth, effectiveBaseHeight, &texFormatInfo);
	cemu_assert_debug(hasStencil == ((texFormatInfo.vkImageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0));
	imageInfo.format = texFormatInfo.vkImageFormat;
	vkObjTex->m_imageAspect = texFormatInfo.vkImageAspect;

	if (isDepth == false && texFormatInfo.isCompressed)
	{
		imageInfo.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
	}
	if (isDepth == false)
		imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

	if (isDepth)
	{
		imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}
	else
	{
		if(Latte::IsCompressedFormat(format) == false && texFormatInfo.vkImageFormat != VK_FORMAT_R4G4_UNORM_PACK8) // Vulkan's R4G4 cant be used as a color attachment
			imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	if (dim == Latte::E_DIM::DIM_2D)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else if (dim == Latte::E_DIM::DIM_1D)
		imageInfo.imageType = VK_IMAGE_TYPE_1D;
	else if (dim == Latte::E_DIM::DIM_3D)
		imageInfo.imageType = VK_IMAGE_TYPE_3D;
	else if (dim == Latte::E_DIM::DIM_2D_ARRAY)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else if (dim == Latte::E_DIM::DIM_CUBEMAP)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else if (dim == Latte::E_DIM::DIM_2D_MSAA)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else
	{
		cemu_assert_unimplemented();
	}

	if (vkCreateImage(m_vkr->GetLogicalDevice(), &imageInfo, nullptr, &vkObjTex->m_image) != VK_SUCCESS)
		m_vkr->UnrecoverableError("Failed to create texture image");
	
	if (m_vkr->IsDebugMarkersEnabled())
	{
		VkDebugUtilsObjectNameInfoEXT objName{};
		objName.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		objName.objectType = VK_OBJECT_TYPE_IMAGE;
		objName.pNext = nullptr;
		objName.objectHandle = (uint64_t)vkObjTex->m_image;
		auto objNameStr = fmt::format("tex_{:08x}_fmt{:04x}", physAddress, (uint32)format);
		objName.pObjectName = objNameStr.c_str();
		vkSetDebugUtilsObjectNameEXT(m_vkr->GetLogicalDevice(), &objName);
	}

	vkObjTex->m_flags = imageInfo.flags;
	vkObjTex->m_format = imageInfo.format;

	// init layout array
	m_layoutsMips = std::max(mipLevels, 1u); // todo - use effective mip count
	m_layoutsDepth = std::max(depth, 1u);
	if (Is3DTexture())
		m_layouts.resize(m_layoutsMips, VK_IMAGE_LAYOUT_UNDEFINED); // one per mip
	else
		m_layouts.resize(m_layoutsMips * m_layoutsDepth, VK_IMAGE_LAYOUT_UNDEFINED); // one per layer per mip
}

LatteTextureVk::~LatteTextureVk()
{
	cemu_assert_debug(views.empty());

	m_vkr->surfaceCopy_notifyTextureRelease(this);

	// Release Mali BC5-workaround baked images. These are independent VkImages allocated
	// outside the texture heap and live alongside the main vkObjTex.
	for (auto& kv : m_bakedViews)
	{
		BakedView* bv = kv.second.get();
		if (bv->image != VK_NULL_HANDLE)
			vkDestroyImage(m_vkr->GetLogicalDevice(), bv->image, nullptr);
		if (bv->memory != VK_NULL_HANDLE)
			vkFreeMemory(m_vkr->GetLogicalDevice(), bv->memory, nullptr);
	}
	m_bakedViews.clear();

	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(vkObjTex);
	vkObjTex = nullptr;
}

LatteTextureVk::BakedView* LatteTextureVk::GetOrCreateBakedView(uint32 compSelKey, const uint8 compSel[4])
{
	// Only BC5 textures benefit from this; bail out otherwise.
	if (format != Latte::E_GX2SURFFMT::BC5_UNORM && format != Latte::E_GX2SURFFMT::BC5_SNORM)
		return nullptr;

	auto it = m_bakedViews.find(compSelKey);
	if (it != m_bakedViews.end() && it->second->uploaded)
		return it->second.get();

	BakedView* bv;
	if (it == m_bakedViews.end())
	{
		// Create the VkImage matching this texture's geometry but as VK_FORMAT_R8G8B8A8_UNORM
		// so the shader can sample with identity VkComponentMapping.
		auto owned = std::make_unique<BakedView>();
		bv = owned.get();
		bv->compSelKey = compSelKey;

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = mipLevels;
		imageInfo.arrayLayers = std::max<uint32>(depth, 1);
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		if (dim == Latte::E_DIM::DIM_CUBEMAP && (depth % 6) == 0 && width == height)
			imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

		if (vkCreateImage(m_vkr->GetLogicalDevice(), &imageInfo, nullptr, &bv->image) != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "BAKED_VIEW: vkCreateImage failed for compSelKey={:04x}", compSelKey);
			return nullptr;
		}

		// Allocate dedicated memory. We avoid Cemu's texture heap because it's tuned for the
		// game's main texture set; baked views are a small auxiliary set with simple lifetime.
		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(m_vkr->GetLogicalDevice(), bv->image, &memReq);
		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		uint32 memTypeIdx = 0;
		if (!m_vkr->GetMemoryManager()->FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memTypeIdx))
		{
			cemuLog_log(LogType::Force, "BAKED_VIEW: FindMemoryType failed for compSelKey={:04x}", compSelKey);
			vkDestroyImage(m_vkr->GetLogicalDevice(), bv->image, nullptr);
			bv->image = VK_NULL_HANDLE;
			return nullptr;
		}
		allocInfo.memoryTypeIndex = memTypeIdx;
		if (vkAllocateMemory(m_vkr->GetLogicalDevice(), &allocInfo, nullptr, &bv->memory) != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "BAKED_VIEW: vkAllocateMemory failed for compSelKey={:04x}", compSelKey);
			vkDestroyImage(m_vkr->GetLogicalDevice(), bv->image, nullptr);
			bv->image = VK_NULL_HANDLE;
			return nullptr;
		}
		vkBindImageMemory(m_vkr->GetLogicalDevice(), bv->image, bv->memory, 0);

		m_bakedViews[compSelKey] = std::move(owned);
	}
	else
	{
		bv = it->second.get();
	}

	UploadBakedView(bv, compSel);
	return bv;
}

void LatteTextureVk::UploadBakedView(BakedView* baked, const uint8 compSel[4])
{
	// Decode BC5 source data CPU-side with the requested compSel baked into RGBA8 layout,
	// upload to the baked VkImage. Handles all slices and mip levels.
	// End any active renderpass — vkCmdCopyBufferToImage is invalid inside one.
	m_vkr->draw_endRenderPass();
	auto device = m_vkr->GetLogicalDevice();
	uint32 layerCount = std::max<uint32>(depth, 1);

	// Precompute compSel→channel mapping. Latte source for BC5 is (R, G); default B=0, A=1.
	// compSel: 0=R, 1=G, 2=B(=0 for BC5), 3=A(=1 for BC5), 4=ZERO, 5=ONE, 6/7=ZERO
	auto resolveChannel = [&](uint8 c, float r, float g) -> float
	{
		switch (c) {
			case 0: return r;
			case 1: return g;
			case 2: return 0.0f;
			case 3: return 1.0f;
			case 4: return 0.0f;
			case 5: return 1.0f;
			default: return 0.0f;
		}
	};

	// Choose BC5 block decoder based on signedness.
	auto blockDecodeFn = (format == Latte::E_GX2SURFFMT::BC5_SNORM) ? &decodeBC5Block_SNORM : &decodeBC5Block_UNORM;

	for (uint32 layer = 0; layer < layerCount; layer++)
	{
		for (uint32 mip = 0; mip < (uint32)mipLevels; mip++)
		{
			LatteTextureLoaderCtx loaderCtx{};
			LatteTextureLoader_begin(&loaderCtx, layer, mip, physAddress, physMipAddress, format, dim, width, height, layerCount, mipLevels, pitch, tileMode, swizzle);

			uint32 mipWidth = loaderCtx.width;
			uint32 mipHeight = loaderCtx.height;
			uint32 rgbaSize = mipWidth * mipHeight * 4;
			if (rgbaSize == 0)
				continue;

			// Stage the decoded RGBA8 data via Cemu's staging allocator.
			auto& staging = m_vkr->GetMemoryManager()->getStagingAllocator();
			auto upload = staging.AllocateBufferMemory(rgbaSize, 16);
			uint8* dstBase = (uint8*)upload.memPtr;

			// Iterate BC5 blocks (4x4) — Cemu's LatteTextureLoader handles detiling.
			for (uint32 by = 0; by < mipHeight; by += 4)
			{
				for (uint32 bx = 0; bx < mipWidth; bx += 4)
				{
					uint8* blockData = LatteTextureLoader_GetInput(&loaderCtx, (sint32)bx, (sint32)by);
					float rgBlock[4 * 4 * 2];
					blockDecodeFn(blockData, rgBlock);

					uint32 blockSizeX = std::min<uint32>(4, mipWidth - bx);
					uint32 blockSizeY = std::min<uint32>(4, mipHeight - by);
					for (uint32 py = 0; py < blockSizeY; py++)
					{
						for (uint32 px = 0; px < blockSizeX; px++)
						{
							float r = rgBlock[(px + py * 4) * 2 + 0];
							float g = rgBlock[(px + py * 4) * 2 + 1];
							uint8* outPx = dstBase + ((bx + px) + (by + py) * mipWidth) * 4;
							for (uint32 c = 0; c < 4; c++)
							{
								float v = resolveChannel(compSel[c], r, g);
								outPx[c] = (uint8)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
							}
						}
					}
				}
			}
			staging.FlushReservation(upload);

			// Transition image layout to TRANSFER_DST_OPTIMAL for this subresource.
			VkCommandBuffer cmd = m_vkr->getCurrentCommandBuffer();
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = baked->uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = baked->image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = mip;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.baseArrayLayer = layer;
			barrier.subresourceRange.layerCount = 1;
			barrier.srcAccessMask = baked->uploaded ? VK_ACCESS_SHADER_READ_BIT : 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd,
				baked->uploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

			VkBufferImageCopy region{};
			region.bufferOffset = upload.bufferOffset;
			region.bufferRowLength = mipWidth;
			region.bufferImageHeight = mipHeight;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = mip;
			region.imageSubresource.baseArrayLayer = layer;
			region.imageSubresource.layerCount = 1;
			region.imageExtent = { mipWidth, mipHeight, 1 };
			vkCmdCopyBufferToImage(cmd, upload.vkBuffer, baked->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			// Transition back to SHADER_READ_ONLY_OPTIMAL.
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
	}
	baked->uploaded = true;
}

LatteTextureView* LatteTextureVk::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
{
	cemu_assert_debug(mipCount > 0);
	cemu_assert_debug(sliceCount > 0);
	cemu_assert_debug((firstMip + mipCount) <= this->mipLevels);
	cemu_assert_debug((firstSlice + sliceCount) <= this->depth);
	return new LatteTextureViewVk(m_vkr->GetLogicalDevice(), this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
}

void LatteTextureVk::AllocateOnHost()
{
	auto allocationInfo = VulkanRenderer::GetInstance()->GetMemoryManager()->imageMemoryAllocate(GetImageObj()->m_image);
	vkObjTex->m_allocation = allocationInfo;
}
