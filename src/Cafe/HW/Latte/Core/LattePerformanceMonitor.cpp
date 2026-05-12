#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"

performanceMonitor_t performanceMonitor{};

void LattePerformanceMonitor_frameEnd()
{
	// per-frame stats
	performanceMonitor.gpuTime_shaderCreate.frameFinished();
	performanceMonitor.gpuTime_frameTime.frameFinished();
	performanceMonitor.gpuTime_idleTime.frameFinished();
	performanceMonitor.gpuTime_fenceTime.frameFinished();

	performanceMonitor.gpuTime_dcStageTextures.frameFinished();
	performanceMonitor.gpuTime_dcStageVertexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageIndexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageMRT.frameFinished();
	performanceMonitor.gpuTime_dcStageDrawcallAPI.frameFinished();
	performanceMonitor.gpuTime_waitForAsync.frameFinished();

	uint32 elapsedTime = GetTickCount() - performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate;
	if (elapsedTime >= 1000)
	{
		bool isFirstUpdate = performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate == 0;
		// sum up raw stats
		uint32 totalElapsedTime = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 totalElapsedTimeFPS = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 elapsedFrames = 0;
		uint32 elapsedFrames2S = 0; // elapsed frames for last two entries (seconds)
		uint64 skippedCycles = 0;
		uint64 vertexDataUploaded = 0;
		uint64 vertexDataCached = 0;
		uint64 uniformBankUploadedData = 0;
		uint64 uniformBankUploadedCount = 0;
		uint64 indexDataUploaded = 0;
		uint64 indexDataCached = 0;
		uint32 frameCounter = 0;
		uint32 drawCallCounter = 0;
		uint32 fastDrawCallCounter = 0;
		uint32 shaderBindCounter = 0;
		uint32 recompilerLeaveCount = 0;
		uint32 threadLeaveCount = 0;
		for (sint32 i = 0; i < PERFORMANCE_MONITOR_TRACK_CYCLES; i++)
		{
			elapsedFrames += performanceMonitor.cycle[i].frameCounter;
			skippedCycles += performanceMonitor.cycle[i].skippedCycles;
			vertexDataUploaded += performanceMonitor.cycle[i].vertexDataUploaded;
			vertexDataCached += performanceMonitor.cycle[i].vertexDataCached;
			uniformBankUploadedData += performanceMonitor.cycle[i].uniformBankUploadedData;
			uniformBankUploadedCount += performanceMonitor.cycle[i].uniformBankUploadedCount;
			indexDataUploaded += performanceMonitor.cycle[i].indexDataUploaded;
			indexDataCached += performanceMonitor.cycle[i].indexDataCached;
			frameCounter += performanceMonitor.cycle[i].frameCounter;
			drawCallCounter += performanceMonitor.cycle[i].drawCallCounter;
			fastDrawCallCounter += performanceMonitor.cycle[i].fastDrawCallCounter;
			shaderBindCounter += performanceMonitor.cycle[i].shaderBindCount;
			recompilerLeaveCount += performanceMonitor.cycle[i].recompilerLeaveCount;
			threadLeaveCount += performanceMonitor.cycle[i].threadLeaveCount;
		}
		elapsedFrames = std::max<uint32>(elapsedFrames, 1);
		elapsedFrames2S = performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 0) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S += performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S = std::max<uint32>(elapsedFrames2S, 1);
		// calculate stats
		uint64 passedCycles = PPCInterpreter_getMainCoreCycleCounter() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastCycleCount;
		passedCycles -= skippedCycles;
		uint64 vertexDataUploadPerFrame = (vertexDataUploaded / (uint64)elapsedFrames);
		vertexDataUploadPerFrame /= 1024ULL;
		uint64 vertexDataCachedPerFrame = (vertexDataCached / (uint64)elapsedFrames);
		vertexDataCachedPerFrame /= 1024ULL;
		uint64 uniformBankDataUploadedPerFrame = (uniformBankUploadedData / (uint64)elapsedFrames);
		uniformBankDataUploadedPerFrame /= 1024ULL;
		uint32 uniformBankCountUploadedPerFrame = (uint32)(uniformBankUploadedCount / (uint64)elapsedFrames);
		uint64 indexDataUploadPerFrame = (indexDataUploaded / (uint64)elapsedFrames);

		double fps = (double)elapsedFrames2S * 1000.0 / (double)totalElapsedTimeFPS;
		uint32 shaderBindsPerFrame = shaderBindCounter / elapsedFrames;
		passedCycles = passedCycles * 1000ULL / totalElapsedTime;
		uint32 rlps = (uint32)((uint64)recompilerLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		uint32 tlps = (uint32)((uint64)threadLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		// set stats
		performanceMonitor.stats.indexDataUploadPerFrame = indexDataUploadPerFrame;
		// next counter cycle
		sint32 nextCycleIndex = (performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES;
		performanceMonitor.cycle[nextCycleIndex].drawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].fastDrawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].frameCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].shaderBindCount = 0;
		performanceMonitor.cycle[nextCycleIndex].lastCycleCount = PPCInterpreter_getMainCoreCycleCounter();
		performanceMonitor.cycle[nextCycleIndex].skippedCycles = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedData = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedCount = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].recompilerLeaveCount = 0;
		performanceMonitor.cycle[nextCycleIndex].threadLeaveCount = 0;
		performanceMonitor.cycleIndex = nextCycleIndex;

		// next update in 1 second
		performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate = GetTickCount();

		if (isFirstUpdate)
		{
			LatteOverlay_updateStats(0.0, 0, 0);
			WindowSystem::UpdateWindowTitles(false, false, 0.0);
		}
		else
		{
			LatteOverlay_updateStats(fps, drawCallCounter / elapsedFrames, fastDrawCallCounter / elapsedFrames);
			WindowSystem::UpdateWindowTitles(false, false, fps);
		}
		{
			uint64 totalUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_frameTime.getPreviousFrameValue());
			uint64 idleUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_idleTime.getPreviousFrameValue());
			uint64 fenceUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_fenceTime.getPreviousFrameValue());
			uint64 texUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_dcStageTextures.getPreviousFrameValue());
			uint64 vtxUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_dcStageVertexMgr.getPreviousFrameValue());
			uint64 shUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.getPreviousFrameValue());
			uint64 idxUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_dcStageIndexMgr.getPreviousFrameValue());
			uint64 mrtUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_dcStageMRT.getPreviousFrameValue());
			uint64 dcUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_dcStageDrawcallAPI.getPreviousFrameValue());
			uint64 awUS = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_waitForAsync.getPreviousFrameValue());
			cemuLog_log(LogType::Force,
				"[VkPerf] fps={:.1f} draws={} fast={} | frame={}us idle={}us | passes={} (FBOchg={} extEnd={}) ext[imgCopy={} surfCopy={} clrD={} clrC={} upload={} readback={} query={} submit={} imgui={} bufCache={} other={}] | DS={} pipes={}",
				fps, drawCallCounter / elapsedFrames, fastDrawCallCounter / elapsedFrames,
				totalUS, idleUS,
				performanceMonitor.vk.numBeginRenderpassPerFrame.get(),
				performanceMonitor.vk.numRenderpassFBOChangesPerFrame.get(),
				performanceMonitor.vk.numRenderpassExternalEndsPerFrame.get(),
				performanceMonitor.vk.extEndImageCopy.get(),
				performanceMonitor.vk.extEndSurfaceCopy.get(),
				performanceMonitor.vk.extEndClearDepth.get(),
				performanceMonitor.vk.extEndClearColor.get(),
				performanceMonitor.vk.extEndTextureUpload.get(),
				performanceMonitor.vk.extEndTextureReadback.get(),
				performanceMonitor.vk.extEndQuery.get(),
				performanceMonitor.vk.extEndSubmit.get(),
				performanceMonitor.vk.extEndImgui.get(),
				performanceMonitor.vk.extEndBufferCache.get(),
				performanceMonitor.vk.extEndOther.get(),
				performanceMonitor.vk.numDescriptorSets.get(), performanceMonitor.vk.numGraphicPipelines.get());
		}
	}
}

void LattePerformanceMonitor_frameBegin()
{
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();
	performanceMonitor.vk.numRenderpassFBOChangesPerFrame.reset();
	performanceMonitor.vk.numRenderpassSelfDepBreaksPerFrame.reset();
	performanceMonitor.vk.numRenderpassExternalEndsPerFrame.reset();
	performanceMonitor.vk.extEndImageCopy.reset();
	performanceMonitor.vk.extEndSurfaceCopy.reset();
	performanceMonitor.vk.extEndClearDepth.reset();
	performanceMonitor.vk.extEndClearColor.reset();
	performanceMonitor.vk.extEndTextureUpload.reset();
	performanceMonitor.vk.extEndTextureReadback.reset();
	performanceMonitor.vk.extEndQuery.reset();
	performanceMonitor.vk.extEndSubmit.reset();
	performanceMonitor.vk.extEndImgui.reset();
	performanceMonitor.vk.extEndBufferCache.reset();
	performanceMonitor.vk.extEndOther.reset();
}
