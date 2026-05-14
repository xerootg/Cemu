package info.cemu.cemu.savebackup

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import info.cemu.cemu.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Foreground service that runs a single save-backup pass and stops itself. Used so the
 * backup survives the death of whatever Activity scheduled it (the user pressing Back to
 * exit the app, or the system reclaiming the Activity right after onStop).
 *
 * Foreground type = dataSync: appropriate for "uploads/downloads of user data" per the
 * Android docs. Notification is intentionally low priority and silent.
 */
class SaveBackupService : Service() {
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var currentJob: Job? = null

    override fun onCreate() {
        super.onCreate()
        ensureChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val force = intent?.getBooleanExtra(EXTRA_FORCE, false) == true

        // Promote BEFORE doing any work — Android will ANR/crash a service that's not in
        // the foreground within ~5s of startForegroundService().
        val initial = buildNotification("Backing up saves…", "Preparing", indeterminate = true)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                initial,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC,
            )
        } else {
            startForeground(NOTIFICATION_ID, initial)
        }

        // If a previous request is still running, ignore this one — a second call from
        // EmulationActivity.onStop while the prior MainActivity-triggered backup is in
        // flight would otherwise corrupt the temp file.
        if (currentJob?.isActive == true) {
            Log.i(TAG, "Backup already in progress — ignoring duplicate start")
            return START_NOT_STICKY
        }

        currentJob = scope.launch {
            val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
            try {
                val result = SaveBackupManager.runBackup(this@SaveBackupService, force = force) { p ->
                    nm.notify(NOTIFICATION_ID, buildProgressNotification(p))
                }
                val final = when (result) {
                    is SaveBackupManager.Result.Success ->
                        buildNotification(
                            "Save backup complete",
                            "${result.archiveName} (${formatBytes(result.bytesWritten)})",
                            indeterminate = false,
                        )
                    is SaveBackupManager.Result.Failure ->
                        buildNotification(
                            "Save backup failed",
                            result.message,
                            indeterminate = false,
                        )
                    SaveBackupManager.Result.Skipped -> null
                }
                if (final != null) {
                    // Replace the foreground notification with a one-shot result that the
                    // user can dismiss. Use a different ID so it survives stopForeground.
                    nm.notify(RESULT_NOTIFICATION_ID, final)
                }
            } finally {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelfFlag.set(true)
                stopSelf()
            }
        }

        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        currentJob?.cancel()
        super.onDestroy()
    }

    private fun buildProgressNotification(p: SaveBackupManager.Progress): Notification {
        val indeterminate = p.total <= 0
        val text = if (indeterminate) p.phase else "${p.phase} — ${formatBytes(p.current)} / ${formatBytes(p.total)}"
        return buildNotification("Backing up saves…", text, indeterminate = true, progressCur = p.current, progressMax = p.total)
    }

    private fun buildNotification(
        title: String,
        text: String,
        indeterminate: Boolean,
        progressCur: Long = 0,
        progressMax: Long = 0,
    ): Notification {
        val b = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(title)
            .setContentText(text)
            .setOngoing(indeterminate)
            .setSilent(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setPriority(NotificationCompat.PRIORITY_LOW)
        if (indeterminate) {
            if (progressMax > 0 && progressCur > 0) {
                val cur = (progressCur.coerceAtMost(progressMax) * 100 / progressMax).toInt()
                b.setProgress(100, cur, false)
            } else {
                b.setProgress(0, 0, true)
            }
        }
        return b.build()
    }

    private fun ensureChannel() {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Save backup",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "Progress and results for automatic save backups."
            setShowBadge(false)
        }
        nm.createNotificationChannel(channel)
    }

    private fun formatBytes(bytes: Long): String {
        if (bytes < 1024) return "$bytes B"
        val kb = bytes / 1024.0
        if (kb < 1024) return "%.1f KB".format(kb)
        val mb = kb / 1024.0
        return "%.1f MB".format(mb)
    }

    companion object {
        private const val TAG = "SaveBackupService"
        private const val CHANNEL_ID = "save_backup"
        private const val NOTIFICATION_ID = 2
        private const val RESULT_NOTIFICATION_ID = 3
        private const val EXTRA_FORCE = "force"

        // Suppress duplicate start storms triggered by onStop+onDestroy firing back-to-back.
        private val stopSelfFlag = AtomicBoolean(false)

        fun start(context: Context, force: Boolean = false) {
            val intent = Intent(context, SaveBackupService::class.java).apply {
                putExtra(EXTRA_FORCE, force)
            }
            ContextCompat.startForegroundService(context, intent)
        }
    }
}
