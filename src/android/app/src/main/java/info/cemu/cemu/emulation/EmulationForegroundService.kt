package info.cemu.cemu.emulation

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import info.cemu.cemu.R
import info.cemu.cemu.nativeinterface.NativeEmulation
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File

/**
 * Keeps the emulator process resident in the foreground while a title is running so
 * Android's Low Memory Killer doesn't reclaim us during long backgrounding sessions
 * (screen off, Home button, app switcher, etc.). The persistent notification is the
 * cost of being promoted from "cached" to "perceptible" priority.
 *
 * No work happens here — emulation runs in [EmulationActivity] / native code. This
 * service only owns the notification + foreground promotion.
 */
class EmulationForegroundService : Service() {
    // Service-scoped coroutine for the periodic stats update. SupervisorJob so a single
    // failure (e.g. transient /proc read error) doesn't tear down the whole scope.
    private val scope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    private var statsUpdateJob: Job? = null
    private var sessionStartElapsedMs: Long = 0

    override fun onCreate() {
        super.onCreate()
        ensureNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (sessionStartElapsedMs == 0L) {
            sessionStartElapsedMs = SystemClock.elapsedRealtime()
        }
        val notification = buildNotification(currentStats())
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        // Tick the stats every ~2s. Re-issued notify() with the same channel + setSilent
        // won't disturb the user; the FGS-required visibility just keeps refreshing.
        statsUpdateJob?.cancel()
        statsUpdateJob = scope.launch {
            val manager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
            while (isActive) {
                delay(STATS_INTERVAL_MS)
                runCatching {
                    manager.notify(NOTIFICATION_ID, buildNotification(currentStats()))
                }
            }
        }
        // START_NOT_STICKY: if Android does kill the service (rare under FGS), don't try
        // to restart the emulator without an Activity context — the game is gone either way.
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        statsUpdateJob?.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private data class Stats(val playtimeMs: Long, val ramMb: Long, val width: Int, val height: Int)

    private fun currentStats(): Stats {
        val packed = runCatching { NativeEmulation.getCurrentRenderResolutionPacked() }.getOrDefault(0L)
        val width = (packed ushr 32).toInt()
        val height = (packed and 0xFFFFFFFFL).toInt()
        return Stats(
            playtimeMs = SystemClock.elapsedRealtime() - sessionStartElapsedMs,
            ramMb = readVmRssMb(),
            width = width,
            height = height,
        )
    }

    /** Reads VmRSS (resident set size) from /proc/self/status. -1 if unreadable. */
    private fun readVmRssMb(): Long {
        return runCatching {
            File("/proc/self/status").bufferedReader().useLines { lines ->
                for (line in lines) {
                    if (line.startsWith("VmRSS:")) {
                        // "VmRSS:    123456 kB"
                        val kb = line.substringAfter(":").trim().substringBefore(' ').toLong()
                        return@useLines kb / 1024L
                    }
                }
                -1L
            }
        }.getOrDefault(-1L)
    }

    private fun formatDuration(ms: Long): String {
        if (ms < 0) return "00:00:00"
        val totalSec = ms / 1000
        val h = totalSec / 3600
        val m = (totalSec % 3600) / 60
        val s = totalSec % 60
        return "%02d:%02d:%02d".format(h, m, s)
    }

    private fun buildNotification(stats: Stats): Notification {
        // Tapping the notification body returns the user to the running EmulationActivity.
        val resumeIntent = Intent(this, EmulationActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val resumePendingIntent = PendingIntent.getActivity(
            this,
            REQUEST_RESUME,
            resumeIntent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        // "Quit" action: routes back through EmulationActivity with a flag so onCreate /
        // onNewIntent recognize it and call onQuit() (which stops the service and finishes).
        val quitIntent = Intent(this, EmulationActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            putExtra(EmulationActivity.EXTRA_QUIT_FROM_NOTIFICATION, true)
        }
        val quitPendingIntent = PendingIntent.getActivity(
            this,
            REQUEST_QUIT,
            quitIntent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        val playtimeStr = formatDuration(stats.playtimeMs)
        val ramStr = if (stats.ramMb >= 0) "${stats.ramMb} MB" else "?"
        val resStr = if (stats.width > 0 && stats.height > 0) "${stats.width}×${stats.height}" else "—"
        val collapsedText = "$playtimeStr  •  $ramStr  •  $resStr"
        val expandedText = "Playtime: $playtimeStr\nRAM: $ramStr\nResolution: $resStr\n\nTap to return to the game."

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(collapsedText)
            .setStyle(NotificationCompat.BigTextStyle().bigText(expandedText))
            .setOngoing(true)
            .setSilent(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setContentIntent(resumePendingIntent)
            .addAction(0, "Quit", quitPendingIntent)
            .build()
    }

    private fun ensureNotificationChannel() {
        // The channel needs to exist before the notification is shown; idempotent so
        // creating it on every onCreate is fine.
        val manager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Emulation",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "Notifies you while a game is running so Android keeps Cemu alive in the background."
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    companion object {
        private const val CHANNEL_ID = "emulation_foreground"
        private const val NOTIFICATION_ID = 1
        private const val REQUEST_RESUME = 0
        private const val REQUEST_QUIT = 1
        // ~2 Hz feels live without flashing the icon area; setOnlyAlertOnce(true) is
        // already keeping the alert quiet, this just limits redraw work.
        private const val STATS_INTERVAL_MS = 2000L

        fun start(context: Context) {
            val intent = Intent(context, EmulationForegroundService::class.java)
            ContextCompat.startForegroundService(context, intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, EmulationForegroundService::class.java)
            context.stopService(intent)
        }
    }
}
