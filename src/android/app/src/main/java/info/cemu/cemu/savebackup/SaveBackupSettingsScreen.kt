package info.cemu.cemu.savebackup

import android.content.Intent
import android.text.format.DateUtils
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import info.cemu.cemu.common.ui.components.Button
import info.cemu.cemu.common.ui.components.ScreenContent
import info.cemu.cemu.common.ui.components.SingleSelection
import info.cemu.cemu.common.ui.components.Toggle
import info.cemu.cemu.common.ui.localization.tr

@Composable
fun SaveBackupSettingsScreen(
    navigateBack: () -> Unit,
    viewModel: SaveBackupSettingsViewModel = viewModel(),
) {
    val context = LocalContext.current
    val settings by viewModel.settings.collectAsState()
    val restoreState by viewModel.restore.collectAsState()

    val pickFolder =
        rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri == null) return@rememberLauncherForActivityResult
            // Persist the grant so the backup can run from a Service after the app is killed.
            context.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
            )
            viewModel.setDestination(context, uri)
        }

    // OpenDocument lets the user pick any zip — used as the escape-hatch when restoring
    // from outside the configured destination (sideloaded archive, friend's save, etc.).
    val pickZip =
        rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) return@rememberLauncherForActivityResult
            val name = uri.lastPathSegment?.substringAfterLast('/') ?: uri.toString()
            viewModel.chooseSource(uri, name)
        }

    val destinationLabel = settings.destinationDisplayName
        ?: settings.destinationUri
        ?: tr("Not set — tap to pick a folder")

    ScreenContent(
        appBarText = tr("Save backup"),
        navigateBack = navigateBack,
    ) {
        Toggle(
            label = tr("Automatically back up on close"),
            description = tr("Zip mlc01/usr/save and upload to your chosen folder when a game closes or the app exits."),
            checked = settings.enabled,
            onCheckedChanged = { viewModel.setEnabled(it) },
            enabled = settings.destinationUri != null,
        )
        Button(
            label = tr("Backup destination"),
            description = destinationLabel,
            onClick = { pickFolder.launch(null) },
        )
        if (settings.destinationUri != null) {
            Button(
                label = tr("Clear backup destination"),
                onClick = { viewModel.clearDestination(context) },
            )
        }
        SingleSelection(
            label = tr("Keep last N backups"),
            choice = settings.keepCount,
            onChoiceChanged = { viewModel.setKeepCount(it) },
            choiceToString = { if (it == Int.MAX_VALUE) tr("Unlimited") else it.toString() },
            choices = listOf(1, 3, 5, 10, 20, Int.MAX_VALUE),
        )
        SingleSelection(
            label = tr("Minimum interval between backups"),
            choice = settings.minIntervalMinutes,
            onChoiceChanged = { viewModel.setMinInterval(it) },
            choiceToString = { minutesToString(it) },
            choices = listOf(0, 1, 5, 15, 30, 60),
        )
        Button(
            label = tr("Back up now"),
            description = if (settings.destinationUri == null)
                tr("Pick a destination folder first")
            else
                lastBackupSubtitle(settings.lastBackupEpochMillis),
            onClick = {
                if (settings.destinationUri != null) {
                    SaveBackupService.start(context, force = true)
                }
            },
        )
        Button(
            label = tr("Restore from backup"),
            description = tr("Replace mlc01/usr/save with the contents of a backup zip. Best done from the launcher with no game running."),
            onClick = { viewModel.openRestorePicker(context) },
        )
    }

    when (val state = restoreState) {
        SaveBackupSettingsViewModel.RestoreUiState.Idle -> Unit
        SaveBackupSettingsViewModel.RestoreUiState.Loading -> RestoreLoadingDialog()
        is SaveBackupSettingsViewModel.RestoreUiState.ListReady ->
            RestoreListDialog(
                entries = state.entries,
                onPick = { e -> viewModel.chooseSource(e.uri, e.name) },
                onBrowse = { pickZip.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) },
                onDismiss = { viewModel.dismissRestore() },
            )
        is SaveBackupSettingsViewModel.RestoreUiState.Confirming ->
            RestoreConfirmDialog(
                displayName = state.displayName,
                hasDestination = settings.destinationUri != null,
                onConfirm = { snapshotFirst -> viewModel.runRestore(context, state.sourceUri, snapshotFirst) },
                onDismiss = { viewModel.dismissRestore() },
            )
        is SaveBackupSettingsViewModel.RestoreUiState.InProgress ->
            RestoreProgressDialog(phase = state.phase)
        is SaveBackupSettingsViewModel.RestoreUiState.Done ->
            RestoreDoneDialog(
                filesRestored = state.filesRestored,
                bytesWritten = state.bytesWritten,
                onDismiss = { viewModel.dismissRestore() },
            )
        is SaveBackupSettingsViewModel.RestoreUiState.Error ->
            RestoreErrorDialog(message = state.message, onDismiss = { viewModel.dismissRestore() })
    }
}

@Composable
private fun RestoreLoadingDialog() {
    AlertDialog(
        onDismissRequest = {},
        title = { Text(tr("Loading backups…")) },
        text = { LinearProgressIndicator(modifier = Modifier.fillMaxWidth()) },
        confirmButton = {},
    )
}

@Composable
private fun RestoreListDialog(
    entries: List<SaveBackupManager.BackupEntry>,
    onPick: (SaveBackupManager.BackupEntry) -> Unit,
    onBrowse: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(tr("Restore from backup")) },
        text = {
            Column {
                if (entries.isEmpty()) {
                    Text(tr("No backups found in your destination folder. Pick a zip to restore from elsewhere."))
                } else {
                    LazyColumn(modifier = Modifier.heightIn(max = 320.dp)) {
                        items(entries, key = { it.uri.toString() }) { entry ->
                            BackupEntryRow(entry = entry, onClick = { onPick(entry) })
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onBrowse) {
                Text(tr("Pick another zip…"))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(tr("Cancel")) }
        },
    )
}

@Composable
private fun BackupEntryRow(entry: SaveBackupManager.BackupEntry, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(end = 8.dp),
        ) {
            Text(entry.name, fontSize = 16.sp)
            val sub = "${formatBytes(entry.sizeBytes)} · ${
                if (entry.lastModifiedMillis > 0)
                    DateUtils.getRelativeTimeSpanString(entry.lastModifiedMillis, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS)
                else ""
            }"
            Text(sub, fontSize = 12.sp)
        }
        TextButton(onClick = onClick) { Text(tr("Restore")) }
    }
}

@Composable
private fun RestoreConfirmDialog(
    displayName: String,
    hasDestination: Boolean,
    onConfirm: (snapshotFirst: Boolean) -> Unit,
    onDismiss: () -> Unit,
) {
    // Default ON when a destination is configured (we can actually snapshot); forced OFF
    // otherwise because runBackup() has nowhere to put the snapshot.
    var snapshotFirst by rememberSaveable { mutableStateOf(hasDestination) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(tr("Replace current saves?")) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("This will replace mlc01/usr/save with the contents of:"))
                Text(displayName, fontSize = 14.sp)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(
                        checked = snapshotFirst && hasDestination,
                        onCheckedChange = { snapshotFirst = it },
                        enabled = hasDestination,
                    )
                    Text(
                        if (hasDestination) tr("Back up current saves first")
                        else tr("Back up current saves first (set a destination to enable)"),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onConfirm(snapshotFirst && hasDestination) }) {
                Text(tr("Replace"))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(tr("Cancel")) }
        },
    )
}

@Composable
private fun RestoreProgressDialog(phase: String) {
    AlertDialog(
        onDismissRequest = {},
        title = { Text(tr("Restoring…")) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(phase)
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }
        },
        confirmButton = {},
    )
}

@Composable
private fun RestoreDoneDialog(filesRestored: Int, bytesWritten: Long, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(tr("Restore complete")) },
        text = { Text(tr("%d files restored (%s).").format(filesRestored, formatBytes(bytesWritten))) },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(tr("OK")) }
        },
    )
}

@Composable
private fun RestoreErrorDialog(message: String, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(tr("Restore failed")) },
        text = { Text(message) },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(tr("OK")) }
        },
    )
}

private fun minutesToString(m: Int): String = when (m) {
    0 -> tr("No limit")
    1 -> tr("1 minute")
    else -> tr("%d minutes").format(m)
}

private fun lastBackupSubtitle(epochMs: Long): String {
    if (epochMs <= 0L) return tr("No backup yet")
    val rel = DateUtils.getRelativeTimeSpanString(
        epochMs,
        System.currentTimeMillis(),
        DateUtils.MINUTE_IN_MILLIS,
    )
    return tr("Last backup: %s").format(rel)
}

private fun formatBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val kb = bytes / 1024.0
    if (kb < 1024) return "%.1f KB".format(kb)
    val mb = kb / 1024.0
    return "%.1f MB".format(mb)
}
