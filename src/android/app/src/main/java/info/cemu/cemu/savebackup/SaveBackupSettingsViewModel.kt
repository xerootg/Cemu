package info.cemu.cemu.savebackup

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.datastore.core.DataStore
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import info.cemu.cemu.common.settings.AppSettings
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.settings.SaveBackupSettings
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

class SaveBackupSettingsViewModel(
    private val dataStore: DataStore<AppSettings> = AppSettingsStore.dataStore,
) : ViewModel() {
    val settings = dataStore.data.map { it.saveBackupSettings }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), SaveBackupSettings())

    sealed class RestoreUiState {
        object Idle : RestoreUiState()
        object Loading : RestoreUiState()
        data class ListReady(val entries: List<SaveBackupManager.BackupEntry>) : RestoreUiState()
        data class Confirming(val sourceUri: Uri, val displayName: String) : RestoreUiState()
        data class InProgress(val phase: String) : RestoreUiState()
        data class Done(val filesRestored: Int, val bytesWritten: Long) : RestoreUiState()
        data class Error(val message: String) : RestoreUiState()
    }

    private val _restore = MutableStateFlow<RestoreUiState>(RestoreUiState.Idle)
    val restore: StateFlow<RestoreUiState> = _restore.asStateFlow()

    fun openRestorePicker(context: Context) {
        _restore.value = RestoreUiState.Loading
        viewModelScope.launch {
            val list = SaveBackupManager.listAvailableBackups(context)
            _restore.value = RestoreUiState.ListReady(list)
        }
    }

    /** Called when the user picked an archive (either from the list or via OpenDocument). */
    fun chooseSource(uri: Uri, displayName: String) {
        _restore.value = RestoreUiState.Confirming(uri, displayName)
    }

    fun dismissRestore() {
        _restore.value = RestoreUiState.Idle
    }

    fun runRestore(context: Context, sourceUri: Uri, snapshotFirst: Boolean) {
        _restore.value = RestoreUiState.InProgress("Starting")
        viewModelScope.launch {
            val result = SaveBackupManager.restoreFrom(context, sourceUri, snapshotFirst) { p ->
                _restore.value = RestoreUiState.InProgress(p.phase)
            }
            _restore.value = when (result) {
                is SaveBackupManager.RestoreResult.Success ->
                    RestoreUiState.Done(result.filesRestored, result.bytesWritten)
                is SaveBackupManager.RestoreResult.Failure ->
                    RestoreUiState.Error(result.message)
            }
        }
    }

    fun setEnabled(enabled: Boolean) {
        viewModelScope.launch {
            dataStore.updateData { it.copy(saveBackupSettings = it.saveBackupSettings.copy(enabled = enabled)) }
        }
    }

    fun setKeepCount(n: Int) {
        viewModelScope.launch {
            dataStore.updateData { it.copy(saveBackupSettings = it.saveBackupSettings.copy(keepCount = n)) }
        }
    }

    fun setMinInterval(minutes: Int) {
        viewModelScope.launch {
            dataStore.updateData { it.copy(saveBackupSettings = it.saveBackupSettings.copy(minIntervalMinutes = minutes)) }
        }
    }

    /**
     * Commit a freshly-picked tree URI. Caller must have already invoked
     * takePersistableUriPermission so the grant survives reboot.
     */
    fun setDestination(context: Context, uri: Uri) {
        val display = DocumentFile.fromTreeUri(context, uri)?.name ?: uri.toString()
        viewModelScope.launch {
            dataStore.updateData {
                it.copy(
                    saveBackupSettings = it.saveBackupSettings.copy(
                        destinationUri = uri.toString(),
                        destinationDisplayName = display,
                    )
                )
            }
        }
    }

    fun clearDestination(context: Context) {
        // Snapshot the current URI off the StateFlow before mutating it.
        val uriStr = settings.value.destinationUri
        viewModelScope.launch {
            // Best-effort release of the persisted grant; ignore if the OS already dropped it.
            uriStr?.let {
                runCatching {
                    context.contentResolver.releasePersistableUriPermission(
                        Uri.parse(it),
                        Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
                    )
                }
            }
            dataStore.updateData {
                it.copy(
                    saveBackupSettings = it.saveBackupSettings.copy(
                        destinationUri = null,
                        destinationDisplayName = null,
                    )
                )
            }
        }
    }
}
