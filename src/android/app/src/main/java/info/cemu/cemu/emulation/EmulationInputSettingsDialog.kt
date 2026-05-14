package info.cemu.cemu.emulation

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.toRoute
import info.cemu.cemu.settings.input.InputSettingsScreen
import info.cemu.cemu.settings.input.controller.ControllerInputSettingsScreen
import info.cemu.cemu.settings.input.device.DeviceInputSettingsScreen
import info.cemu.cemu.settings.input.hotkeys.HotkeySettingsScreen
import info.cemu.cemu.settings.inputoverlay.InputOverlaySettingsScreen
import kotlinx.serialization.Serializable

private object EmulationInputRoutes {
    @Serializable
    object Home

    @Serializable
    data class Controller(val index: Int)

    @Serializable
    object Hotkeys

    @Serializable
    object InputOverlay

    @Serializable
    object Device
}

@Composable
fun EmulationInputSettingsDialog(onDismiss: () -> Unit) {
    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(
            usePlatformDefaultWidth = false,
            dismissOnBackPress = false,
        ),
    ) {
        Surface(
            modifier = Modifier.fillMaxSize(),
            color = MaterialTheme.colorScheme.background,
        ) {
            val navController = rememberNavController()

            fun popOrDismiss() {
                if (!navController.popBackStack()) onDismiss()
            }

            NavHost(
                navController = navController,
                startDestination = EmulationInputRoutes.Home,
            ) {
                composable<EmulationInputRoutes.Home> {
                    InputSettingsScreen(
                        navigateBack = ::popOrDismiss,
                        goToInputOverlaySettings = {
                            navController.navigate(EmulationInputRoutes.InputOverlay)
                        },
                        goToControllerSettings = { index ->
                            navController.navigate(EmulationInputRoutes.Controller(index))
                        },
                        goToHostInputSettings = {
                            navController.navigate(EmulationInputRoutes.Device)
                        },
                        goToHotkeySettings = {
                            navController.navigate(EmulationInputRoutes.Hotkeys)
                        },
                    )
                }
                composable<EmulationInputRoutes.Controller> { entry ->
                    val index = entry.toRoute<EmulationInputRoutes.Controller>().index
                    ControllerInputSettingsScreen(
                        navigateBack = ::popOrDismiss,
                        controllerIndex = index,
                    )
                }
                composable<EmulationInputRoutes.Hotkeys> {
                    HotkeySettingsScreen(navigateBack = ::popOrDismiss)
                }
                composable<EmulationInputRoutes.InputOverlay> {
                    InputOverlaySettingsScreen(navigateBack = ::popOrDismiss)
                }
                composable<EmulationInputRoutes.Device> {
                    DeviceInputSettingsScreen(navigateBack = ::popOrDismiss)
                }
            }
        }
    }
}
