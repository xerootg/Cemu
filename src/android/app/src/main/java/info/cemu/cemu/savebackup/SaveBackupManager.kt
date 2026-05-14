package info.cemu.cemu.savebackup

import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import info.cemu.cemu.common.android.context.internalFolder
import info.cemu.cemu.common.settings.AppSettingsStore
import kotlinx.coroutines.flow.first
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Runs the actual save backup: zip [mlc01/usr/save] into a temp file, copy the temp
 * file's bytes into a SAF-picked tree (provider can be Google Drive, Dropbox, OneDrive,
 * a local folder, ...), then prune older archives to [keepCount].
 *
 * SAF is the only abstraction layer — we never touch a provider SDK. The Uri stored in
 * [SaveBackupSettings.destinationUri] was obtained via ACTION_OPEN_DOCUMENT_TREE and the
 * Activity called takePersistableUriPermission so the grant survives reboots.
 *
 * Designed to be called from a foreground Service so it survives Activity death — the
 * call itself is a single suspending function and reports progress via [progress].
 */
object SaveBackupManager {
    private const val TAG = "SaveBackup"
    private const val ARCHIVE_PREFIX = "cemu-saves-"
    private const val ARCHIVE_SUFFIX = ".zip"
    private const val ARCHIVE_MIME = "application/zip"

    sealed class Result {
        object Skipped : Result()
        data class Success(val bytesWritten: Long, val archiveName: String) : Result()
        data class Failure(val message: String, val cause: Throwable? = null) : Result()
    }

    sealed class RestoreResult {
        data class Success(val filesRestored: Int, val bytesWritten: Long) : RestoreResult()
        data class Failure(val message: String, val cause: Throwable? = null) : RestoreResult()
    }

    data class Progress(val phase: String, val current: Long = 0, val total: Long = 0)

    data class BackupEntry(
        val uri: Uri,
        val name: String,
        val sizeBytes: Long,
        val lastModifiedMillis: Long,
    )

    /**
     * Performs one backup cycle. Honors the min-interval debounce unless [force] is true.
     * Returns immediately if no destination is picked.
     */
    suspend fun runBackup(
        context: Context,
        force: Boolean = false,
        onProgress: (Progress) -> Unit = {},
    ): Result {
        val store = AppSettingsStore.dataStore
        val settings = store.data.first().saveBackupSettings

        val uriStr = settings.destinationUri
        if (uriStr.isNullOrEmpty()) {
            return Result.Failure("No backup destination configured")
        }
        if (!force && settings.enabled.not()) {
            return Result.Skipped
        }
        val now = System.currentTimeMillis()
        val minIntervalMs = settings.minIntervalMinutes.coerceAtLeast(0) * 60_000L
        if (!force && minIntervalMs > 0 && now - settings.lastBackupEpochMillis < minIntervalMs) {
            return Result.Skipped
        }

        val saveDir = File(context.internalFolder(), "mlc01/usr/save")
        if (!saveDir.isDirectory) {
            return Result.Failure("No saves to back up yet (${saveDir.absolutePath} missing)")
        }

        val treeUri = Uri.parse(uriStr)
        val tree = DocumentFile.fromTreeUri(context, treeUri)
            ?: return Result.Failure("Backup folder unreachable — re-pick it in settings")
        if (!tree.canWrite()) {
            return Result.Failure("Backup folder not writable — re-pick it in settings")
        }

        val timestamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).apply {
            timeZone = TimeZone.getTimeZone("UTC")
        }.format(Date(now))
        val archiveName = "$ARCHIVE_PREFIX$timestamp$ARCHIVE_SUFFIX"

        // Write to an in-app cache file first, then stream that file into the SAF target.
        // Two-phase write protects against partial uploads if the user pulls the destination
        // mid-flight; the target file is created in SAF only after the zip is fully on disk.
        val tmpZip = File(context.cacheDir, "save-backup-pending.zip")
        tmpZip.delete()
        try {
            onProgress(Progress(phase = "Compressing saves"))
            val bytes = zipDirectory(saveDir, tmpZip) { current, total ->
                onProgress(Progress("Compressing saves", current, total))
            }

            onProgress(Progress(phase = "Uploading to destination", current = 0, total = bytes))

            // If a file with the same name already exists (unlikely with seconds precision
            // but possible on re-runs), remove it first — DocumentFile.createFile would add
            // a "(1)" suffix.
            tree.findFile(archiveName)?.delete()

            val outFile = tree.createFile(ARCHIVE_MIME, archiveName)
                ?: return Result.Failure("Provider refused to create file")

            val resolver = context.contentResolver
            val written = resolver.openOutputStream(outFile.uri, "w")?.use { out ->
                tmpZip.inputStream().use { input ->
                    val buf = ByteArray(64 * 1024)
                    var total = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        out.write(buf, 0, n)
                        total += n
                        onProgress(Progress("Uploading to destination", total, bytes))
                    }
                    out.flush()
                    total
                }
            } ?: run {
                outFile.delete()
                return Result.Failure("Could not open output stream for ${outFile.uri}")
            }

            onProgress(Progress("Pruning old backups"))
            pruneOld(tree, settings.keepCount)

            store.updateData { it.copy(saveBackupSettings = it.saveBackupSettings.copy(lastBackupEpochMillis = now)) }

            Log.i(TAG, "Backup complete: $archiveName ($written bytes)")
            return Result.Success(written, archiveName)
        } catch (t: Throwable) {
            Log.e(TAG, "Backup failed", t)
            return Result.Failure(t.message ?: t.javaClass.simpleName, t)
        } finally {
            tmpZip.delete()
        }
    }

    /** Writes a deterministic zip of [src] into [dst]. Returns the byte size of [dst]. */
    private fun zipDirectory(
        src: File,
        dst: File,
        onProgress: (current: Long, total: Long) -> Unit,
    ): Long {
        // Count first so the UI can show "x of y" without a second walk during write.
        val files = src.walkTopDown().filter { it.isFile }.sortedBy { it.relativeTo(src).path }.toList()
        val totalBytes = files.sumOf { it.length() }
        var done = 0L

        ZipOutputStream(dst.outputStream().buffered()).use { zip ->
            // STORED would skip CPU work but blow up the wire size; DEFLATED default is fine —
            // Wii U save data is mostly small binaries (account.dat, common.dat, *.bin) where
            // deflate is essentially free on modern ARM cores and the network savings are real.
            for (f in files) {
                val rel = f.relativeTo(src).path.replace('\\', '/')
                zip.putNextEntry(ZipEntry(rel).apply { time = f.lastModified() })
                f.inputStream().use { input ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        zip.write(buf, 0, n)
                        done += n
                        onProgress(done, totalBytes)
                    }
                }
                zip.closeEntry()
            }
        }
        return dst.length()
    }

    /**
     * Returns the archives currently in the configured destination tree, newest first.
     * Empty if no destination is set or the tree is unreachable.
     */
    suspend fun listAvailableBackups(context: Context): List<BackupEntry> {
        val uriStr = AppSettingsStore.dataStore.data.first().saveBackupSettings.destinationUri
            ?: return emptyList()
        val tree = DocumentFile.fromTreeUri(context, Uri.parse(uriStr)) ?: return emptyList()
        return tree.listFiles().asSequence()
            .filter { it.isFile }
            .filter { (it.name ?: "").startsWith(ARCHIVE_PREFIX) && (it.name ?: "").endsWith(ARCHIVE_SUFFIX) }
            .map { BackupEntry(uri = it.uri, name = it.name ?: "", sizeBytes = it.length(), lastModifiedMillis = it.lastModified()) }
            .sortedByDescending { it.name }
            .toList()
    }

    /**
     * Replaces the contents of mlc01/usr/save with the contents of [sourceZipUri]. The
     * zip is expected to have been produced by [runBackup] — entry paths are relative to
     * the save root. If [snapshotFirst] is true, runs an immediate forced backup before
     * touching anything so the prior state is recoverable.
     */
    suspend fun restoreFrom(
        context: Context,
        sourceZipUri: Uri,
        snapshotFirst: Boolean,
        onProgress: (Progress) -> Unit = {},
    ): RestoreResult {
        try {
            if (snapshotFirst) {
                onProgress(Progress("Snapshotting current saves"))
                // Forced + bypasses interval check; failures here are surfaced so the user
                // can decide to bail rather than overwriting without a safety net.
                when (val pre = runBackup(context, force = true, onProgress = onProgress)) {
                    is Result.Failure -> return RestoreResult.Failure(
                        "Pre-restore snapshot failed: ${pre.message} (aborted — current saves untouched)",
                        pre.cause,
                    )
                    else -> Unit
                }
            }

            val saveRoot = File(context.internalFolder(), "mlc01/usr/save")

            // Extract to a sibling staging dir first, then atomically swap. If the zip is
            // truncated or the device runs out of room mid-extract, the original saves are
            // untouched.
            val staging = File(context.internalFolder(), "mlc01/usr/.save-restore-staging")
            if (staging.exists()) staging.deleteRecursively()
            staging.mkdirs()

            onProgress(Progress("Reading backup"))
            var filesRestored = 0
            var bytesWritten = 0L
            context.contentResolver.openInputStream(sourceZipUri).use { input ->
                if (input == null) return RestoreResult.Failure("Could not open backup zip")
                ZipInputStream(input.buffered()).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        if (entry.isDirectory) {
                            File(staging, entry.name).mkdirs()
                            zip.closeEntry()
                            continue
                        }
                        // Path-traversal guard: refuse entries that resolve outside staging.
                        val out = File(staging, entry.name).canonicalFile
                        if (!out.path.startsWith(staging.canonicalPath + File.separator)) {
                            return RestoreResult.Failure("Refusing suspicious zip entry: ${entry.name}")
                        }
                        out.parentFile?.mkdirs()
                        out.outputStream().buffered().use { os ->
                            val buf = ByteArray(64 * 1024)
                            while (true) {
                                val n = zip.read(buf)
                                if (n <= 0) break
                                os.write(buf, 0, n)
                                bytesWritten += n
                            }
                        }
                        if (entry.time > 0) out.setLastModified(entry.time)
                        filesRestored++
                        onProgress(Progress("Extracting", current = filesRestored.toLong()))
                        zip.closeEntry()
                    }
                }
            }

            onProgress(Progress("Applying restore"))
            // Replace the live save tree with the staged copy. Keep the old tree under a
            // temp name first so we can roll back if the rename fails halfway.
            val oldBackup = File(context.internalFolder(), "mlc01/usr/.save-restore-old")
            if (oldBackup.exists()) oldBackup.deleteRecursively()
            if (saveRoot.exists()) {
                if (!saveRoot.renameTo(oldBackup)) {
                    staging.deleteRecursively()
                    return RestoreResult.Failure("Could not move current saves out of the way")
                }
            }
            saveRoot.parentFile?.mkdirs()
            if (!staging.renameTo(saveRoot)) {
                // Roll back.
                oldBackup.renameTo(saveRoot)
                return RestoreResult.Failure("Could not move restored saves into place")
            }
            oldBackup.deleteRecursively()

            Log.i(TAG, "Restore complete: $filesRestored files, $bytesWritten bytes")
            return RestoreResult.Success(filesRestored, bytesWritten)
        } catch (t: Throwable) {
            Log.e(TAG, "Restore failed", t)
            return RestoreResult.Failure(t.message ?: t.javaClass.simpleName, t)
        }
    }

    /** Keep the [keepCount] newest archives (by filename, which is timestamp-sorted). */
    private fun pruneOld(tree: DocumentFile, keepCount: Int) {
        if (keepCount <= 0) return
        val archives = tree.listFiles().asSequence()
            .filter { it.isFile }
            .filter { it.name?.startsWith(ARCHIVE_PREFIX) == true && it.name?.endsWith(ARCHIVE_SUFFIX) == true }
            .sortedByDescending { it.name }
            .toList()
        if (archives.size <= keepCount) return
        for (old in archives.drop(keepCount)) {
            runCatching { old.delete() }.onFailure { Log.w(TAG, "Failed to prune ${old.name}: ${it.message}") }
        }
    }
}
