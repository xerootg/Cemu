package info.cemu.cemu.emulation

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import info.cemu.cemu.BuildConfig
import info.cemu.cemu.common.android.inputevent.isFromPhysicalController
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.ui.components.ActivityContent
import info.cemu.cemu.common.ui.localization.TranslatableContent
import info.cemu.cemu.emulation.input.ControllerCallbacks
import info.cemu.cemu.emulation.input.ControllerMotionHandler
import info.cemu.cemu.emulation.input.DeviceControllerCallbacks
import info.cemu.cemu.emulation.input.DeviceMotionHandler
import info.cemu.cemu.emulation.input.HotkeyManager
import info.cemu.cemu.emulation.input.InputHandler
import info.cemu.cemu.emulation.input.NativeInputDeviceListener
import info.cemu.cemu.nativeinterface.NativeEmulation
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlin.system.exitProcess

private class InputDelegateManager(context: Context) {
    private val nativeInputDeviceListener = NativeInputDeviceListener(context)
    private val controllerCallbacks = ControllerCallbacks(context)
    private val controllerMotionHandler = ControllerMotionHandler(context)
    private val deviceControllerCallbacks = DeviceControllerCallbacks(context)
    private val deviceMotionHandler = DeviceMotionHandler(context)

    fun setDeviceMotionEnabled(isListening: Boolean) =
        deviceMotionHandler.setIsListening(isListening)

    fun registerAll() {
        nativeInputDeviceListener.register()
        controllerCallbacks.register()
        controllerMotionHandler.register()
        deviceControllerCallbacks.register()
    }

    fun unregisterAll() {
        nativeInputDeviceListener.unregister()
        controllerCallbacks.unregister()
        controllerMotionHandler.unregister()
        deviceControllerCallbacks.unregister()
    }

    fun onResume(rotation: Int) {
        registerAll()
        deviceMotionHandler.setDeviceRotation(rotation)
        deviceMotionHandler.resumeListening()
    }

    fun onPause() {
        unregisterAll()
        deviceMotionHandler.pauseListening()
    }
}

class EmulationActivity : AppCompatActivity() {
    private lateinit var inputManager: InputDelegateManager
    private var processInputEvents = true

    // Modern permission contract for POST_NOTIFICATIONS (Android 13+, API 33). Without
    // the runtime grant, our foreground-service notification is silently dropped and
    // on Android 14+ the system kills the service for not posting one. Result observed
    // via `dumpsys notification`: numEnqueuedByApp=1, numPostedByApp=0, importance=NONE.
    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            EmulationForegroundService.start(this)
        } else {
            Log.w(
                "Cemu",
                "POST_NOTIFICATIONS denied — foreground service may be killed by Android during long backgrounding",
            )
            // Still try; the service will at least promote priority briefly.
            EmulationForegroundService.start(this)
        }
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (processInputEvents && InputHandler.onMotionEvent(event)) {
            return true
        }

        return super.onGenericMotionEvent(event)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        HotkeyManager.onKeyEvent(event)

        if (processInputEvents && InputHandler.onKeyEvent(event)) {
            return true
        }

        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE && event.isFromPhysicalController()) {
            return true
        }

        return super.dispatchKeyEvent(event)
    }

    private fun getGamePath(): String {
        val extras = intent.extras
        val data = intent.data
        var launchPath: String? = null

        if (extras != null) {
            launchPath = extras.getString(EXTRA_LAUNCH_PATH)
        }

        if (launchPath == null && data != null) {
            launchPath = data.toString()
        }

        if (launchPath == null) {
            throw RuntimeException("launchPath is null")
        }

        return launchPath
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Cold-start variant of the notification "Quit" path: Activity was killed by the
        // system and the user tapped the action — the new launch intent carries the flag.
        if (intent.getBooleanExtra(EXTRA_QUIT_FROM_NOTIFICATION, false)) {
            onQuit()
            return
        }

        inputManager = InputDelegateManager(this)

        setupHotkeys()

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        setFullscreen()

        // Promote the process to foreground-service priority so Android's LMK doesn't
        // reclaim the emulator during long backgrounding sessions. On Android 13+ we
        // need the runtime POST_NOTIFICATIONS grant first — otherwise the notification
        // never shows and Android 14+ will kill the service.
        ensureForegroundServiceWithRationale()

        val gamePath = getGamePath()

        setContent {
            TranslatableContent {
                ActivityContent {
                    EmulationScreen(
                        gamePath = gamePath,
                        setMotionSensorEnabled = inputManager::setDeviceMotionEnabled,
                        onQuit = ::onQuit,
                        onMinimize = { moveTaskToBack(true) },
                        setInputListeningEnabled = { processInputEvents = it },
                    )
                }
            }
        }
    }

    override fun onPause() {
        super.onPause()

        // Auto-pause the title via the ProcUI release path the moment the Activity loses
        // focus (Home button, app switcher, screen off, dialog over us). Idempotent — if
        // the user already toggled the in-game checkbox, this is a no-op. Fires BEFORE
        // input cleanup so any in-flight input event still reaches the running PPC thread
        // rather than being dropped between pause and the unregister.
        NativeEmulation.triggerReleaseForeground()
        inputManager.onPause()
    }

    override fun onResume() {
        super.onResume()

        // Symmetric: input listeners back online first, then resume emulation so the
        // game's first frame after acquire has fresh input plumbed.
        inputManager.onResume(display.rotation)
        NativeEmulation.triggerAcquireForeground()
    }

    override fun onNewIntent(intent: android.content.Intent) {
        super.onNewIntent(intent)
        // The notification's "Quit" action and any future deep-links route back through
        // singleTop launch; check the flag and bail out if asked.
        if (intent.getBooleanExtra(EXTRA_QUIT_FROM_NOTIFICATION, false)) {
            onQuit()
        }
    }

    private fun ensureForegroundServiceWithRationale() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            // Pre-Android-13: notification permission is granted at install time.
            EmulationForegroundService.start(this)
            return
        }
        val alreadyGranted = ContextCompat.checkSelfPermission(
            this, Manifest.permission.POST_NOTIFICATIONS
        ) == PackageManager.PERMISSION_GRANTED
        if (alreadyGranted) {
            EmulationForegroundService.start(this)
            return
        }
        // First-time (or previously-denied) — explain why before bouncing to the system
        // dialog. Android's permission dialog gives no context; users are more likely to
        // grant when they understand what the notification is for.
        AlertDialog.Builder(this)
            .setTitle("Keep your game running in the background")
            .setMessage(
                "Cemu uses a persistent notification so Android keeps the emulator alive " +
                "when you switch apps or lock the screen.\n\n" +
                "Without notifications, Android may kill the emulator after a short time " +
                "in the background and you'll lose your progress.\n\n" +
                "We'll only show one quiet, silent notification while a game is running."
            )
            .setPositiveButton("Allow notifications") { _, _ ->
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
            .setNegativeButton("Not now") { _, _ ->
                // User opted out — skip the service entirely. Game still runs but Android
                // is free to reclaim the process during long backgrounding.
                Log.i("Cemu", "Notification permission declined by user; foreground service not started")
            }
            .setCancelable(false)
            .show()
    }

    private fun setupHotkeys() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                AppSettingsStore.dataStore.data.map { it.hotkeySettings }
                    .distinctUntilChanged()
                    .collect { HotkeyManager.setHotkeyMappings(it) }
            }
        }
    }

    private fun setFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller.hide(WindowInsetsCompat.Type.systemBars())
    }

    private fun onQuit() {
        EmulationForegroundService.stop(this)
        finish()
        exitProcess(0)
    }

    override fun onDestroy() {
        // Belt-and-suspenders: if the Activity is destroyed without going through onQuit
        // (e.g., system reclaim, "Recent Apps" swipe-to-close), tear down the foreground
        // service too so we don't leave the notification orphaned.
        EmulationForegroundService.stop(this)
        super.onDestroy()
    }

    companion object {
        const val EXTRA_LAUNCH_PATH: String = BuildConfig.APPLICATION_ID + ".LaunchPath"
        const val EXTRA_QUIT_FROM_NOTIFICATION: String = BuildConfig.APPLICATION_ID + ".QuitFromNotification"
    }
}
