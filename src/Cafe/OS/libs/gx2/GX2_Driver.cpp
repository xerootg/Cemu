#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/gx2/GX2_Driver.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/OS/libs/coreinit/coreinit_Memory.h"
#include "Cafe/OS/RPL/rpl.h"

using namespace coreinit;

// Hooks into the Latte command processor's foreground-release park. Declared
// here rather than in a header because GX2 is the only intended caller and the
// linkage is one-directional (GX2 -> Latte CP).
namespace LatteCP
{
	void NotifyForegroundReleased();
	void NotifyForegroundAcquired();
}

namespace GX2
{
	bool GX2DrawDone(); // GX2_Event.cpp

	static SysAllocator<OSDriverInterface> s_GX2Driver;
	static SysAllocator<char, 8> s_GX2DriverName;
	static uint32be s_driverArgUkn1;
	static uint32be s_driverArgUkn2;
	static bool s_driverRegistered = false;

	static const char* GX2Driver_GetName()
	{
		strcpy(s_GX2DriverName.GetPtr(), "GX2");
		return s_GX2DriverName.GetPtr();
	}

	static void GX2Driver_Init()
	{
		// no-op: real gx2.rpl uses this for per-core command-buffer reinit, but
		// Cemu's HLE keeps those alive across module load -- nothing to do.
	}

	static void GX2Driver_OnAcquireForeground()
	{
		cemuLog_log(LogType::Force, "GX2Driver: onAcquireForeground");
		// Wake the Latte CP from its parked state. Must happen before any
		// subsequent GX2 submit reaches the ring.
		LatteCP::NotifyForegroundAcquired();
		OSMemoryBarrier();
	}

	static void GX2Driver_OnReleaseForeground()
	{
		cemuLog_log(LogType::Force, "GX2Driver: onReleaseForeground -- draining GPU pipeline");
		// Drain in-flight work. Must complete BEFORE setting the park flag,
		// otherwise the Latte thread parks while GX2WaitTimeStamp inside
		// GX2DrawDone is still waiting for the timestamp PM4 to retire -> deadlock.
		GX2DrawDone();
		// Now safe to park: the ring is drained and any further submits during
		// the game's RELEASE callback are caught by the 100ms backstop in the
		// Latte CP wait loop.
		LatteCP::NotifyForegroundReleased();
		OSMemoryBarrier();
	}

	static void GX2Driver_OnDone()
	{
		// no-op
	}

	void GX2Driver_Register(uint32 moduleHandle)
	{
		if (s_driverRegistered)
			return;
		s_GX2Driver->getDriverName = RPLLoader_MakePPCCallable([](PPCInterpreter_t* hCPU) {
			MEMPTR<const char> namePtr(GX2Driver_GetName());
			osLib_returnFromFunction(hCPU, namePtr.GetMPTR());
		});
		s_GX2Driver->init = RPLLoader_MakePPCCallable([](PPCInterpreter_t* hCPU) {
			GX2Driver_Init();
			osLib_returnFromFunction(hCPU, 0);
		});
		s_GX2Driver->onAcquireForeground = RPLLoader_MakePPCCallable([](PPCInterpreter_t* hCPU) {
			GX2Driver_OnAcquireForeground();
			osLib_returnFromFunction(hCPU, 0);
		});
		s_GX2Driver->onReleaseForeground = RPLLoader_MakePPCCallable([](PPCInterpreter_t* hCPU) {
			GX2Driver_OnReleaseForeground();
			osLib_returnFromFunction(hCPU, 0);
		});
		s_GX2Driver->done = RPLLoader_MakePPCCallable([](PPCInterpreter_t* hCPU) {
			GX2Driver_OnDone();
			osLib_returnFromFunction(hCPU, 0);
		});

		s_driverArgUkn1 = 0;
		s_driverArgUkn2 = 0;
		uint32be ukn3 = 0;
		// Priority 250 matches real gx2.rpl (verified in
		// tools/procui_background/FINDINGS.md). Higher than ProcUI's 200 means
		// GX2 dispatches AFTER ProcUI on release -- intentional: ProcUI signals
		// the title state first, GX2 quiesces last.
		OSDriver_Register(moduleHandle, 250, s_GX2Driver.GetPtr(), 0,
		                  &s_driverArgUkn1, &s_driverArgUkn2, &ukn3);
		s_driverRegistered = true;
	}

	void GX2Driver_Deregister(uint32 moduleHandle)
	{
		if (!s_driverRegistered)
			return;
		OSDriver_Deregister(moduleHandle, 0);
		s_driverRegistered = false;
	}
}
