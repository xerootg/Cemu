package info.cemu.cemu.settings.input.controller

import android.view.KeyEvent
import android.view.MotionEvent
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import info.cemu.cemu.common.android.inputdevice.hasMotion
import info.cemu.cemu.common.android.inputdevice.hasRumble
import info.cemu.cemu.common.android.inputdevice.listGameControllers
import info.cemu.cemu.common.android.inputdevice.toControllerInfo
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.settings.ControllerProfileBinding
import info.cemu.cemu.nativeinterface.NativeInput
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

data class ButtonInfo(
    val name: String,
    val id: Int,
)

data class InputController(
    val id: Int,
    val name: String,
    val descriptor: String,
    val hasMotion: Boolean,
    val hasRumble: Boolean,
)

data class ActiveController(
    val controller: InputController,
    val settings: NativeInput.ControllerSettings
)

class ControllersViewModel(val controllerIndex: Int) : ViewModel() {
    private var _controllerType = MutableStateFlow(NativeInput.getControllerType(controllerIndex))
    val controllerType = _controllerType.asStateFlow()

    private val _controls = MutableStateFlow<Map<Int, String>>(emptyMap())
    val controls = _controls.asStateFlow()

    private val _controllers = MutableStateFlow<List<InputController>>(emptyList())
    val controllers = _controllers.asStateFlow()

    private val _buttonToBind = MutableStateFlow<ButtonInfo?>(null)
    val buttonToBind = _buttonToBind.asStateFlow()

    fun setButtonToBind(buttonInfo: ButtonInfo) {
        _buttonToBind.value = buttonInfo
    }

    fun clearButtonToBind() {
        _buttonToBind.value = null
    }

    private var controllersCount = NativeInput.ControllerCount(0, 0)

    private fun getControllerMapping(buttonId: Int) =
        buttonId to NativeInput.getControllerMapping(controllerIndex, buttonId)

    fun setControllerType(controllerType: Int) {
        if (!isControllerTypeAllowed(controllerType) || _controllerType.value == controllerType)
            return
        _controllerType.value = controllerType
        NativeInput.setControllerType(controllerIndex, controllerType)
        refreshControllerData()
    }

    fun mapKeyEvent(keyEvent: KeyEvent, buttonId: Int) {
        InputMapper.mapKeyEventToMappingId(controllerIndex, buttonId, keyEvent)
        _controls.value += getControllerMapping(buttonId)
    }

    fun refreshAvailableControllers(): Boolean {
        val gameControllers = listGameControllers()

        val inputControllers = gameControllers.map {
            InputController(
                id = it.id,
                name = it.name,
                descriptor = it.descriptor,
                hasMotion = it.hasMotion(),
                hasRumble = it.hasRumble(),
            )
        }
        setActiveController(inputControllers.firstOrNull())
        _controllers.value = inputControllers

        NativeInput.setControllers(gameControllers.map { it.toControllerInfo() }.toTypedArray())

        return gameControllers.isNotEmpty()
    }

    fun mapAllInputs(deviceId: Int) {
        val oldControls = _controls.value
        _controls.value = emptyMap()
        oldControls.keys.forEach { NativeInput.clearControllerMapping(controllerIndex, it) }

        InputMapper.mapAllInputs(deviceId, controllerIndex)

        val buttons = getNativeButtonsForControllerType(controllerType.value)
        buttons.forEach { _controls.value += getControllerMapping(it.nativeKeyCode) }
    }

    fun tryMapMotionEvent(motionEvent: MotionEvent, buttonId: Int): Boolean {
        if (InputMapper.tryMapMotionEventToMappingId(controllerIndex, buttonId, motionEvent)) {
            _controls.value += getControllerMapping(buttonId)
            return true
        }
        return false
    }

    fun clearButtonMapping(buttonId: Int) {
        _controls.value -= buttonId
        NativeInput.clearControllerMapping(controllerIndex, buttonId)
    }

    private fun refreshControllerData() {
        controllersCount = NativeInput.getControllersCount()
        _controls.value = NativeInput.getControllerMappings(controllerIndex)
    }

    fun isControllerTypeAllowed(controllerType: Int): Boolean {
        if (controllerType == NativeInput.EmulatedControllerType.DISABLED) {
            return true
        }

        val currentControllerType = this.controllerType.value

        if (controllerType == NativeInput.EmulatedControllerType.VPAD) {
            return currentControllerType == NativeInput.EmulatedControllerType.VPAD || controllersCount.vpadCount < NativeInput.MAX_VPAD_CONTROLLERS
        }

        val isWPAD = currentControllerType != NativeInput.EmulatedControllerType.VPAD
                && currentControllerType != NativeInput.EmulatedControllerType.DISABLED

        return isWPAD || controllersCount.wpadCount < NativeInput.MAX_WPAD_CONTROLLERS
    }

    private val _activeController = MutableStateFlow<ActiveController?>(null)
    val activeController = _activeController.asStateFlow()

    private val _profiles = MutableStateFlow<List<String>>(emptyList())
    val profiles = _profiles.asStateFlow()

    private val _currentProfileName = MutableStateFlow("")
    val currentProfileName = _currentProfileName.asStateFlow()

    val controllerBindings = AppSettingsStore.dataStore.data
        .map { it.controllerBindings }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyMap())

    fun refreshProfiles() {
        _profiles.value = NativeInput.getControllerProfiles().toList().sorted()
        _currentProfileName.value = NativeInput.getCurrentProfileName(controllerIndex)
    }

    fun loadProfile(name: String): Boolean {
        if (!NativeInput.loadControllerProfile(controllerIndex, name)) {
            return false
        }
        _controllerType.value = NativeInput.getControllerType(controllerIndex)
        refreshControllerData()
        refreshAvailableControllers()
        _currentProfileName.value = NativeInput.getCurrentProfileName(controllerIndex)
        return true
    }

    fun saveProfile(name: String): Boolean {
        if (!NativeInput.saveControllerProfile(controllerIndex, name)) {
            return false
        }
        _currentProfileName.value = NativeInput.getCurrentProfileName(controllerIndex)
        refreshProfiles()
        return true
    }

    fun deleteProfile(name: String): Boolean {
        if (!NativeInput.deleteControllerProfile(name)) {
            return false
        }
        if (_currentProfileName.value == name) {
            _currentProfileName.value = ""
        }
        viewModelScope.launch {
            AppSettingsStore.dataStore.updateData { settings ->
                val cleaned = settings.controllerBindings.filterValues { it.profileName != name }
                if (cleaned.size == settings.controllerBindings.size) settings
                else settings.copy(controllerBindings = cleaned)
            }
        }
        refreshProfiles()
        return true
    }

    fun bindProfileToDevice(descriptor: String, profileName: String, autoLoad: Boolean) {
        viewModelScope.launch {
            AppSettingsStore.dataStore.updateData { settings ->
                val map = settings.controllerBindings.toMutableMap()
                if (autoLoad && profileName.isNotEmpty()) {
                    map[descriptor] = ControllerProfileBinding(controllerIndex, profileName)
                } else {
                    map.remove(descriptor)
                }
                settings.copy(controllerBindings = map)
            }
        }
    }

    fun setActiveController(controller: InputController?) {
        if (controller == null) {
            _activeController.value = null
            return
        }

        val settings = NativeInput.getControllerSettings(
            controllerIndex,
            controller.descriptor,
            controller.name
        ) ?: NativeInput.ControllerSettings()
        _activeController.value = ActiveController(controller, settings)
    }

    fun setControllerSettings(
        controller: InputController,
        settings: NativeInput.ControllerSettings
    ) {
        NativeInput.setControllerSettings(
            controllerIndex,
            controller.descriptor,
            controller.name,
            settings
        )
        _activeController.value = _activeController.value?.copy(settings = settings)
    }

    fun save() {
        NativeInput.saveInputs()
    }

    init {
        refreshControllerData()
        refreshProfiles()
    }

    companion object {
        val CONTROLLER_INDEX_KEY = object : CreationExtras.Key<Int> {}
        val Factory: ViewModelProvider.Factory = viewModelFactory {
            initializer {
                ControllersViewModel(
                    this[CONTROLLER_INDEX_KEY] as Int
                )
            }
        }
    }
}