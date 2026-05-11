#pragma once

#include "util/math/vector2.h"
#include <vulkan/vulkan_core.h>

#if BOOST_PLAT_ANDROID
#include <android/native_window.h>
#endif

struct SwapchainInfoVk
{
	enum class VSync
	{
		// values here must match GeneralSettings2::m_vsync
		Immediate = 0,
		FIFO = 1,
		MAILBOX = 2,
		SYNC_AND_LIMIT = 3, // synchronize emulated vsync events to monitor vsync. But skip events if rate higher than virtual vsync period
	};

	struct SwapchainSupportDetails
	{
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	void Cleanup();
	void Create();

	bool IsValid() const;

	void WaitAvailableFence();
	void ResetAvailableFence() const;

	bool AcquireImage();
	// retrieve semaphore of last acquire for submitting a wait operation
	// only one wait operation must be submitted per acquire (which submits a single signal operation)
	// therefore subsequent calls will return a NULL handle
	VkSemaphore ConsumeAcquireSemaphore();

	static void UnrecoverableError(const char* errMsg);

	static SwapchainSupportDetails QuerySwapchainSupport(VkSurfaceKHR surface, const VkPhysicalDevice& device);

	VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes);
	VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
	VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

	VkSwapchainCreateInfoKHR CreateSwapchainCreateInfo(VkSurfaceKHR surface, const SwapchainSupportDetails& swapchainSupport, const VkSurfaceFormatKHR& surfaceFormat, uint32 imageCount, const VkExtent2D& extent);


	VkExtent2D getExtent() const
	{
		return m_actualExtent;
	}

	// Logical landscape-orientation extent that callers use for viewport/aspect math.
	// On Android this is the screen size as the user sees it (e.g. 2424x1080) even
	// when the swapchain image is portrait-rotated. m_actualExtent is the physical
	// swapchain extent, which is swapped when m_preRotation is 90/270.
	VkExtent2D getLogicalExtent() const
	{
		if (m_preRotation == 90 || m_preRotation == 270)
			return { m_actualExtent.height, m_actualExtent.width };
		return m_actualExtent;
	}

	// Rotation (degrees CW) baked into the swapchain via preTransform. 0 means
	// no rotation (preTransform == IDENTITY). Output-shader vertex math reads
	// this to rotate the full-screen quad into the swapchain's coordinate
	// system, and DrawBackbufferQuad uses it to remap the viewport rect.
	uint32 m_preRotation = 0;

	SwapchainInfoVk(bool mainWindow, Vector2i size);
	SwapchainInfoVk(const SwapchainInfoVk&) = delete;
	SwapchainInfoVk(SwapchainInfoVk&&) noexcept = default;
	~SwapchainInfoVk();

	bool mainWindow{};

#if BOOST_PLAT_ANDROID
	bool surfaceWasLost = false;
#endif

	bool m_shouldRecreate = false;
	VSync m_vsyncState = VSync::Immediate;
	bool hasDefinedSwapchainImage{}; // indicates if the swapchain image is in a defined state
	VkInstance m_instance{};
	VkPhysicalDevice m_physicalDevice{};
	VkDevice m_logicalDevice{};
	VkSurfaceKHR m_surface{};
	VkSurfaceFormatKHR m_surfaceFormat{};
	VkSwapchainKHR m_swapchain{};
	Vector2i m_desiredExtent{};
	VkExtent2D m_actualExtent{};
	uint32 swapchainImageIndex = (uint32)-1;
	uint64 m_presentId = 1;
	uint64 m_queueDepth = 0; // number of frames with pending presentation requests
	uint64 m_maxQueued = 0; // the maximum number of frames with presentation requests.


	// swapchain image ringbuffer (indexed by swapchainImageIndex)
	std::vector<VkImage> m_swapchainImages;
	std::vector<VkImageView> m_swapchainImageViews;
	std::vector<VkFramebuffer> m_swapchainFramebuffers;
	std::vector<VkSemaphore> m_presentSemaphores; // indexed by swapchainImageIndex

	VkRenderPass m_swapchainRenderPass = nullptr;

private:
	uint32 m_acquireIndex = 0;
	std::vector<VkSemaphore> m_acquireSemaphores; // indexed by m_acquireIndex
	VkFence m_imageAvailableFence{};
	VkFence m_awaitableFence = VK_NULL_HANDLE;
	VkSemaphore m_currentSemaphore = VK_NULL_HANDLE;

#if BOOST_PLAT_ANDROID
	void RecreateSurface();
	ANativeWindow* m_currentWindow = nullptr;
#endif

	std::array<uint32, 2> m_swapchainQueueFamilyIndices;
};
