package info.cemu.cemu.settings.gamespath

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.basicMarquee
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import info.cemu.cemu.R
import info.cemu.cemu.common.ui.components.ScreenContentLazy
import info.cemu.cemu.common.ui.localization.tr
import java.io.File

private fun hasAllFilesAccess(): Boolean =
    Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && Environment.isExternalStorageManager()

@Composable
fun FolderBrowserScreen(
    navigateBack: () -> Unit,
    onFolderSelected: (String) -> Unit,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current

    var hasPermission by remember { mutableStateOf(hasAllFilesAccess()) }

    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) hasPermission = hasAllFilesAccess()
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    val settingsLauncher =
        rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            hasPermission = hasAllFilesAccess()
        }

    val rootPath = remember { Environment.getExternalStorageDirectory().absolutePath }
    var currentPath by rememberSaveable { mutableStateOf(rootPath) }

    ScreenContentLazy(
        appBarText = tr("Browse folders"),
        navigateBack = navigateBack,
        actions = {
            if (hasPermission) {
                IconButton(onClick = {
                    val current = File(currentPath)
                    if (current.exists() && current.isDirectory) {
                        onFolderSelected(current.absolutePath)
                        navigateBack()
                    }
                }) {
                    Icon(
                        painter = painterResource(R.drawable.ic_check),
                        contentDescription = tr("Use this folder"),
                    )
                }
            }
        },
    ) {
        if (!hasPermission) {
            item("permission") {
                Column(
                    modifier = Modifier
                        .fillParentMaxSize()
                        .padding(24.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(tr("Cemu needs access to all files to browse for game folders. This grants direct read access to your device storage."))
                    Button(onClick = {
                        val intent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                            Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                                data = Uri.parse("package:${context.packageName}")
                            }
                        } else {
                            Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                        }
                        settingsLauncher.launch(intent)
                    }) {
                        Text(tr("Grant access"))
                    }
                }
            }
            return@ScreenContentLazy
        }

        item("breadcrumb") {
            Text(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp)
                    .basicMarquee(),
                text = currentPath,
                maxLines = 1,
            )
        }

        val current = File(currentPath)
        val canGoUp = current.absolutePath != rootPath && current.parentFile != null
        val children = current.listFiles()
            ?.filter { it.isDirectory && !it.name.startsWith(".") }
            ?.sortedBy { it.name.lowercase() }
            ?: emptyList()

        if (canGoUp) {
            item("..") {
                FolderRow(name = "..", onClick = {
                    current.parentFile?.let { currentPath = it.absolutePath }
                })
            }
        }
        items(items = children, key = { it.absolutePath }) { folder ->
            FolderRow(name = folder.name, onClick = { currentPath = folder.absolutePath })
        }
    }
}

@Composable
private fun FolderRow(name: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            painter = painterResource(R.drawable.ic_folder),
            contentDescription = null,
        )
        Text(
            modifier = Modifier
                .padding(start = 16.dp)
                .weight(1f),
            text = name,
        )
    }
}
