#pragma once

#include "Cafe/OS/common/OSCommon.h"

namespace GX2
{
	// Registers GX2 with coreinit's OSDriver system. Called from the gx2 module's
	// rpl_entry on RplEntryReason::Loaded. Real gx2.rpl does this at the end of
	// GX2Init; we hoist it to module load because Cemu's HLE GX2 has no GX2Init
	// PPC code path that runs early enough to register before the first
	// foreground-release transition could arrive.
	void GX2Driver_Register(uint32 moduleHandle);
	void GX2Driver_Deregister(uint32 moduleHandle);
}
