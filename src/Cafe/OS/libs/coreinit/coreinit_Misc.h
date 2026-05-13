#pragma once

namespace coreinit
{
	uint32 OSGetUPID();
	uint32 OSGetPFID();
	uint64 OSGetTitleID();
	uint32 __OSGetProcessSDKVersion();
	uint32 OSLaunchTitleByPathl(const char* path, uint32 pathLength, uint32 argc);
	uint32 OSRestartGame(uint32 argc, MEMPTR<char>* argv);

	void OSReleaseForeground();
	void OSSavesDone_ReadyToRelease();

	// Queue a release-foreground transition (game gets MsgReleaseForeground on next system-queue
	// receive). Use TriggerAcquireForegroundTransition() to bring it back. The combined
	// StartBackgroundForegroundTransition() fires both, useful only when you don't actually want
	// to leave the title backgrounded.
	void TriggerReleaseForegroundTransition();
	void TriggerAcquireForegroundTransition();
	void StartBackgroundForegroundTransition();

	// Returns true if the title is currently in the released-foreground state from Cemu's POV
	// (between TriggerReleaseForegroundTransition and the next TriggerAcquireForegroundTransition).
	bool IsForegroundReleased();

	// Invokes the onAcquireForeground / onReleaseForeground callback on each registered
	// OSDriver (GX2, snd_core, etc.) in priority order. Called from UpdateSystemMessageQueue
	// and from OSReleaseForeground itself.
	void DispatchOSDriverOnAcquireForeground();
	void DispatchOSDriverOnReleaseForeground();

	struct OSDriverInterface
	{
		MEMPTR<void> getDriverName;
		MEMPTR<void> init;
		MEMPTR<void> onAcquireForeground;
		MEMPTR<void> onReleaseForeground;
		MEMPTR<void> done;
	};
	static_assert(sizeof(OSDriverInterface) == 0x14);

	uint32 OSDriver_Register(uint32 moduleHandle, sint32 priority, OSDriverInterface* driverCallbacks, sint32 driverId, uint32be* outUkn1, uint32be* outUkn2, uint32be* outUkn3);
	uint32 OSDriver_Deregister(uint32 moduleHandle, sint32 driverId);

	enum class COSReportModule
	{
		coreinit = 0,
	};

	enum class COSReportLevel
	{
		Error = 0,
		Warn = 1,
		Info = 2
	};

	void OSFatal(const char* msg);

	sint32 ppc_vprintf(const char* formatStr, char* strOut, sint32 maxLength, ppc_va_list* vargs);

	void miscInit();
};