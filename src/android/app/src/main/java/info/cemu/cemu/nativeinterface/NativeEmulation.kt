package info.cemu.cemu.nativeinterface

import android.view.Surface

object NativeEmulation {
    @JvmStatic
    external fun initializeEmulation()

    @JvmStatic
    external fun setDPI(dpi: Float)


    @JvmStatic
    external fun setSurface(surface: Surface?, isMainCanvas: Boolean)

    @JvmStatic
    external fun initializeSurface(isMainCanvas: Boolean)

    @JvmStatic
    external fun clearPadSurface()

    @JvmStatic
    external fun setSurfaceSize(width: Int, height: Int, isMainCanvas: Boolean)

    @JvmStatic
    external fun initializeRenderer()

    object PrepareTitleResult {
        const val SUCCESSFUL: Int = 0
        const val ERROR_GAME_BASE_FILES_NOT_FOUND: Int = 1
        const val ERROR_NO_DISC_KEY: Int = 2
        const val ERROR_NO_TITLE_TIK: Int = 3
        const val ERROR_UNKNOWN: Int = 4
    }

    @JvmStatic
    external fun prepareTitle(launchPath: String?): Int

    @JvmStatic
    external fun launchTitle()

    @JvmStatic
    external fun pauseTitle()

    @JvmStatic
    external fun resumeTitle()

    @JvmStatic
    external fun triggerReleaseForeground()

    @JvmStatic
    external fun triggerAcquireForeground()

    @JvmStatic
    external fun isForegroundReleased(): Boolean

    /**
     * Returns the current TV/main canvas dimensions packed into a single Long:
     * `(width << 32) | (height & 0xFFFFFFFF)`. Returns 0 if no surface has been set yet.
     * Used by the foreground-service nerd-stats notification; sample on a slow loop.
     */
    @JvmStatic
    external fun getCurrentRenderResolutionPacked(): Long

    @JvmStatic
    external fun initializeSystems()

    @JvmStatic
    external fun setReplaceTVWithPadView(swapped: Boolean)

    @JvmStatic
    external fun supportsLoadingCustomDriver(): Boolean

    // JIT precompile progress polling. The title-load screen reads these
    // while launchTitle() runs to render a determinate "Warming JIT cache"
    // progress bar instead of an indeterminate spinner.
    object JitPrecompilePhase {
        const val IDLE: Int = 0
        const val RUNNING: Int = 1
        const val DONE: Int = 2
    }

    @JvmStatic
    external fun getJitPrecompilePhase(): Int

    @JvmStatic
    external fun getJitPrecompileTotal(): Int

    @JvmStatic
    external fun getJitPrecompileRemaining(): Int
}
