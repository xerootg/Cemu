package info.cemu.cemu.emulation

import android.view.SurfaceHolder
import androidx.datastore.core.DataStore
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import info.cemu.cemu.common.either.Either
import info.cemu.cemu.common.either.Error
import info.cemu.cemu.common.either.Success
import info.cemu.cemu.common.android.inputdevice.listGameControllers
import info.cemu.cemu.common.either.attemptWithContext
import info.cemu.cemu.common.either.bind
import info.cemu.cemu.common.either.mapError
import info.cemu.cemu.common.settings.AppSettings
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.settings.InputOverlayRect
import info.cemu.cemu.common.settings.InputOverlaySettings
import info.cemu.cemu.common.settings.OverlayInputConfig
import info.cemu.cemu.nativeinterface.NativeEmulation
import info.cemu.cemu.nativeinterface.NativeEmulation.PrepareTitleResult
import info.cemu.cemu.nativeinterface.NativeException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class SideMenuState(
    val isMotionEnabled: Boolean = false,
    val isTVReplacedWithPad: Boolean = false,
    val isPadVisible: Boolean = false,
    val isInputOverlayVisible: Boolean = false,
    // When true, pressing Back while the side drawer is already open minimizes the app
    // (moveTaskToBack) instead of closing the drawer. First Back-press still opens the
    // drawer as usual. Quick "stash the game" gesture for use in public.
    val isDoubleBackToMinimizeEnabled: Boolean = true,
)

class ConditionFlags(
    var isMainConditionMet: Boolean = false, var isPadConditionMet: Boolean = false
) {
    fun get(isMain: Boolean): Boolean {
        return if (isMain) isMainConditionMet else isPadConditionMet
    }

    fun set(isMain: Boolean, value: Boolean) {
        if (isMain) {
            isMainConditionMet = value
        } else {
            isPadConditionMet = value
        }
    }
}

// Phases shown in the title-load dialog while emulation spins up. Each step
// of initializeEmulation() updates the phase before it runs so the user can
// see WHERE the load is, instead of a single indeterminate spinner. The
// WarmingJitCache phase carries a determinate (done/total) so the bar
// actually moves on cold-cache launches where precompile takes 10-60s.
sealed interface LoadingPhase {
    object PreparingTitle : LoadingPhase
    object InitializingSystems : LoadingPhase
    object InitializingRenderer : LoadingPhase
    object StartingTitle : LoadingPhase
    // etaSeconds is null until we have enough samples to make a stable estimate
    // (≥1s elapsed AND ≥1% progress). Codegen is roughly linear per function, so
    // mean-rate * remaining is a good projection — no need for windowed regression.
    data class WarmingJitCache(val done: Int, val total: Int, val etaSeconds: Int?) : LoadingPhase
}

sealed interface NativeError {
    data class SurfaceCreationError(val message: String) : NativeError
    data class RendererInitializationError(val message: String) : NativeError

    object GameFilesNotFoundError : NativeError
    object NoDiscKeysError : NativeError
    object NoTitleTikError : NativeError
    data class UnknownTilePrepareError(val launchPath: String) : NativeError
    data class SystemInitializationError(val message: String) : NativeError

    object LaunchingTitleError : NativeError
}

class EmulationViewModel(
    private val launchPath: String,
    private val dataStore: DataStore<AppSettings> = AppSettingsStore.dataStore
) : ViewModel() {
    private val _emulationError = MutableStateFlow<NativeError?>(null)
    val emulationError = _emulationError.asStateFlow()

    private val _sideMenuState = MutableStateFlow(SideMenuState())
    val sideMenuState = _sideMenuState.asStateFlow()

    val isInputOverlayVisible =
        sideMenuState.map { it.isInputOverlayVisible }
            .stateIn(
                viewModelScope,
                SharingStarted.WhileSubscribed(5000),
                false,
            )

    init {
        viewModelScope.launch {
            val settings = dataStore.data.first()
            // No physical controller attached → force the touch overlay on for this session
            // even if the persisted preference is off; touch is the only input available.
            val overlayVisible = settings.inputOverlaySettings.isOverlayEnabled
                    || listGameControllers().isEmpty()
            _sideMenuState.update { it.copy(isInputOverlayVisible = overlayVisible) }
        }
    }

    val inputOverlaySettings = dataStore.data.map { it.inputOverlaySettings }.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        InputOverlaySettings(),
    )

    fun saveInputOverlayRectangles(inputOverlayRectMap: Map<OverlayInputConfig, InputOverlayRect>) {
        viewModelScope.launch {
            dataStore.updateData {
                val overlaySettings =
                    it.inputOverlaySettings.copy(inputOverlayRectMap = inputOverlayRectMap)

                it.copy(inputOverlaySettings = overlaySettings)
            }
        }
    }

    fun resetInputOverlayLayout() {
        viewModelScope.launch {
            dataStore.updateData {
                val overlaySettings =
                    it.inputOverlaySettings.copy(inputOverlayRectMap = emptyMap())

                it.copy(inputOverlaySettings = overlaySettings)
            }
        }
    }

    fun updateSideMenuState(sideMenuState: SideMenuState) {
        _sideMenuState.value = sideMenuState
    }

    val gamePadPosition = dataStore.data.map { it.emulationSettings.gamePadPosition }
        .stateIn(
            viewModelScope,
            SharingStarted.WhileSubscribed(5000),
            null,
        )

    val destroyedSurfaces = ConditionFlags()
    var setSurfaces = ConditionFlags()

    private inner class CanvasSurfaceHolderCallback(val isMainCanvas: Boolean) :
        SurfaceHolder.Callback {

        override fun surfaceCreated(surfaceHolder: SurfaceHolder) {}

        override fun surfaceChanged(
            surfaceHolder: SurfaceHolder,
            format: Int,
            width: Int,
            height: Int,
        ) {
            try {
                NativeEmulation.setSurfaceSize(width, height, isMainCanvas)

                if (setSurfaces.get(isMainCanvas)) {
                    return
                }

                NativeEmulation.setSurface(surfaceHolder.surface, isMainCanvas)
                val mainSurfaceWasDestroyed = destroyedSurfaces.get(isMain = true)

                if (mainSurfaceWasDestroyed && isMainCanvas) {
                    NativeEmulation.resumeTitle()
                }

                setSurfaces.set(isMainCanvas, true)

                val padSurfaceWasSet = setSurfaces.get(isMain = false)
                if ((!isMainCanvas && !mainSurfaceWasDestroyed) || (isMainCanvas && padSurfaceWasSet)) {
                    NativeEmulation.initializeSurface(isMainCanvas = false)
                }

                destroyedSurfaces.set(isMainCanvas, false)
            } catch (exception: NativeException) {
                _emulationError.value = NativeError.SurfaceCreationError(exception.message!!)
            }
        }

        override fun surfaceDestroyed(surfaceHolder: SurfaceHolder) {
            if (setSurfaces.get(isMain = false)) {
                NativeEmulation.clearPadSurface()
                setSurfaces.set(isMain = false, false)
                destroyedSurfaces.set(isMain = false, true)
            }

            if (isMainCanvas) {
                NativeEmulation.pauseTitle()

                setSurfaces.set(isMain = true, false)
                destroyedSurfaces.set(isMain = true, true)
            }
        }
    }

    val mainHolderCallback: SurfaceHolder.Callback = CanvasSurfaceHolderCallback(true)
    val padHolderCallback: SurfaceHolder.Callback = CanvasSurfaceHolderCallback(false)

    private suspend fun initializeSystems() = attemptWithContext(Dispatchers.IO) {
        NativeEmulation.initializeSystems()
    }.mapError { NativeError.SystemInitializationError(it) }

    private suspend fun initializeRenderer() = attemptWithContext(Dispatchers.IO) {
        NativeEmulation.initializeRenderer()
        NativeEmulation.initializeSurface(isMainCanvas = true)
    }.mapError { NativeError.RendererInitializationError(it) }

    private suspend fun prepareTitle(): Either<Unit, NativeError> =
        attemptWithContext(Dispatchers.IO) { NativeEmulation.prepareTitle(launchPath) }
            .fold(
                onSuccess = { result ->
                    when (result) {
                        PrepareTitleResult.SUCCESSFUL -> Success(Unit)
                        PrepareTitleResult.ERROR_GAME_BASE_FILES_NOT_FOUND -> Error(NativeError.GameFilesNotFoundError)
                        PrepareTitleResult.ERROR_NO_DISC_KEY -> Error(NativeError.NoDiscKeysError)
                        PrepareTitleResult.ERROR_NO_TITLE_TIK -> Error(NativeError.NoTitleTikError)
                        else -> Error(NativeError.UnknownTilePrepareError(launchPath))
                    }
                },
                onError = { Error(NativeError.UnknownTilePrepareError(launchPath)) }
            )

    private suspend fun launchTitle() =
        attemptWithContext(Dispatchers.IO) { NativeEmulation.launchTitle() }
            .mapError { NativeError.LaunchingTitleError }

    private val _isEmulationInitialized = MutableStateFlow(false)
    val isEmulationInitialized = _isEmulationInitialized.asStateFlow()

    private val _loadingPhase = MutableStateFlow<LoadingPhase>(LoadingPhase.PreparingTitle)
    val loadingPhase = _loadingPhase.asStateFlow()

    private var emulationInitializationJob: Job? = null

    // After launchTitle() spawns the detached _LaunchTitleThread we keep the
    // loading dialog up and poll the native precompile progress atomics. The
    // dialog only dismisses once the precompile queue has fully drained —
    // before that point, returning control to the title screen meant the
    // recompiler worker thread was hot during Latte_Start, which manifested
    // as a chuggy title intro on lower-end devices.
    private suspend fun awaitJitPrecompileDone() {
        val phaseDone = NativeEmulation.JitPrecompilePhase.DONE
        // Marks the wall-clock moment we first observed real progress (done > 0).
        // ETA is computed from elapsed-since-that-edge / done * remaining, which
        // self-corrects as the rate stabilizes. Anchoring on first-progress instead
        // of first-Running-observation avoids skew from the early ramp where the
        // worker thread is still cold-cache and codegen is artificially slow.
        var progressStartElapsedMs = -1L
        var progressStartDone = 0
        while (true) {
            val phase = NativeEmulation.getJitPrecompilePhase()
            val total = NativeEmulation.getJitPrecompileTotal()
            val remaining = NativeEmulation.getJitPrecompileRemaining()
            if (phase == phaseDone) return
            _loadingPhase.value = if (total > 0) {
                val done = (total - remaining).coerceAtLeast(0)
                if (progressStartElapsedMs < 0 && done > 0) {
                    progressStartElapsedMs = android.os.SystemClock.elapsedRealtime()
                    progressStartDone = done
                }
                val etaSeconds = computeEtaSeconds(
                    done = done,
                    total = total,
                    progressStartElapsedMs = progressStartElapsedMs,
                    progressStartDone = progressStartDone,
                )
                LoadingPhase.WarmingJitCache(
                    done = done,
                    total = total,
                    etaSeconds = etaSeconds,
                )
            } else {
                // Phase Idle/Running with no work discovered yet — between
                // launchTitle returning and PPCRecompiler_precompileLoadedModules
                // being reached inside _LaunchTitleThread. Briefly indeterminate.
                LoadingPhase.StartingTitle
            }
            delay(100)
        }
    }

    private fun computeEtaSeconds(
        done: Int,
        total: Int,
        progressStartElapsedMs: Long,
        progressStartDone: Int,
    ): Int? {
        // Wait until we have an anchor, ≥1s of motion past it, and the rate is
        // computable. Without these guards the ETA wobbles wildly on the first
        // ~500ms when only a handful of small functions have been processed.
        if (progressStartElapsedMs < 0) return null
        val elapsedSinceAnchorMs = android.os.SystemClock.elapsedRealtime() - progressStartElapsedMs
        if (elapsedSinceAnchorMs < 1000L) return null
        val doneSinceAnchor = done - progressStartDone
        if (doneSinceAnchor <= 0) return null
        val remaining = total - done
        if (remaining <= 0) return 0
        val msPerFunction = elapsedSinceAnchorMs.toDouble() / doneSinceAnchor.toDouble()
        val etaMs = msPerFunction * remaining
        // Clamp to a reasonable range; if the rate calc explodes (e.g. clock
        // glitch), better to show no ETA than a 99999s estimate.
        if (etaMs.isNaN() || etaMs < 0 || etaMs > 3_600_000.0) return null
        return (etaMs / 1000.0).toInt()
    }

    fun initializeEmulation() {
        if (_isEmulationInitialized.value || emulationInitializationJob != null) {
            return
        }

        emulationInitializationJob = viewModelScope.launch {
            _loadingPhase.value = LoadingPhase.PreparingTitle
            val result = prepareTitle()
                .bind {
                    _loadingPhase.value = LoadingPhase.InitializingSystems
                    initializeSystems()
                }
                .bind {
                    _loadingPhase.value = LoadingPhase.InitializingRenderer
                    initializeRenderer()
                }
                .bind {
                    _loadingPhase.value = LoadingPhase.StartingTitle
                    launchTitle()
                }
            result.onError { _emulationError.value = it }
            // Only wait on the JIT precompile when the title actually launched.
            // On error the error dialog replaces the loading dialog, so the
            // exact loading-phase value past this point is irrelevant.
            if (result is Success) {
                awaitJitPrecompileDone()
            }

            _isEmulationInitialized.value = true
        }
    }

    companion object {
        val LAUNCH_PATH_KEY = object : CreationExtras.Key<String> {}
        val Factory: ViewModelProvider.Factory = viewModelFactory {
            initializer {
                EmulationViewModel(
                    this[LAUNCH_PATH_KEY] as String
                )
            }
        }
    }
}