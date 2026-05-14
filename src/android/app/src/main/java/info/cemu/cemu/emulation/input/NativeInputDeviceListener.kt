package info.cemu.cemu.emulation.input

import android.content.Context
import android.hardware.input.InputManager
import info.cemu.cemu.common.android.inputdevice.listGameControllers
import info.cemu.cemu.common.android.inputdevice.toControllerInfo
import info.cemu.cemu.common.input.InputDeviceListener
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.nativeinterface.NativeInput
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

class NativeInputDeviceListener(private val context: Context) {
    private val inputManager
        get() = context.getSystemService(Context.INPUT_SERVICE) as InputManager?

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var seenDescriptors: Set<String> = emptySet()

    private fun refreshControllers() {
        val gameControllers = listGameControllers()
        val gameControllerInfos = gameControllers
            .map { it.toControllerInfo() }
            .toTypedArray()

        NativeInput.setControllers(gameControllerInfos)

        val currentDescriptors = gameControllerInfos.map { it.descriptor }.toSet()
        val newlyAttached = currentDescriptors - seenDescriptors
        seenDescriptors = currentDescriptors

        if (newlyAttached.isNotEmpty()) {
            scope.launch { applyBindingsFor(newlyAttached) }
        }
    }

    private suspend fun applyBindingsFor(descriptors: Set<String>) {
        val bindings = AppSettingsStore.dataStore.data.first().controllerBindings
        for (descriptor in descriptors) {
            val binding = bindings[descriptor] ?: continue
            if (binding.profileName.isEmpty()) continue
            NativeInput.loadControllerProfile(binding.controllerIndex, binding.profileName)
        }
    }

    private val listener = object : InputDeviceListener {
        override fun onInputDeviceChanged() = refreshControllers()
    }

    fun register() {
        inputManager?.registerInputDeviceListener(listener, null)
        refreshControllers()
    }

    fun unregister() {
        inputManager?.unregisterInputDeviceListener(listener)
        scope.cancel()
    }
}
