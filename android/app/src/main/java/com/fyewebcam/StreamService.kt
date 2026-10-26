package com.fyewebcam

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.camera.core.Preview
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.lifecycle.LifecycleService
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.IOException

/** Resolution presets offered in the UI. Sizes are 16:9 and multiples of 16. */
enum class Preset(val label: String, val width: Int, val height: Int) {
    P480("480p", 848, 480),
    P720("720p", 1280, 720),
    P1080("1080p", 1920, 1080);

    companion object {
        fun closest(height: Int): Preset = entries.minBy { kotlin.math.abs(it.height - height) }
    }
}

/** UI-facing state of the stream, updated by [StreamService]. */
object StreamState {
    data class Value(
        val running: Boolean = false,
        val viewers: Int = 0,
        val preset: Preset = Preset.P720,
        val camera: Int = Protocol.CAMERA_BACK,
        val width: Int = 0,
        val height: Int = 0,
        val fps: Int = 30,
        val error: String? = null,
    )

    private val _state = MutableStateFlow(Value())
    val state: StateFlow<Value> = _state.asStateFlow()

    fun update(block: (Value) -> Value) {
        synchronized(this) { _state.value = block(_state.value) }
    }
}

/**
 * Foreground service that owns the camera, the encoder and the TCP server, so streaming keeps
 * going while the screen is off or the app is in the background.
 */
class StreamService : LifecycleService(), StreamServer.Controller {

    inner class LocalBinder : Binder() {
        val service: StreamService get() = this@StreamService
    }

    private val binder = LocalBinder()
    private var encoder: CameraEncoder? = null
    private var server: StreamServer? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var previewProvider: Preview.SurfaceProvider? = null

    override fun onBind(intent: Intent): IBinder {
        super.onBind(intent)
        return binder
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        when (intent?.action) {
            ACTION_STOP -> stopStreaming()
            else -> startStreaming()
        }
        return START_NOT_STICKY
    }

    private fun startStreaming() {
        if (encoder != null) return
        createChannel()
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA else 0
        ServiceCompat.startForeground(this, NOTIFICATION_ID, buildNotification(), type)
        acquireLocks()

        val enc = CameraEncoder(
            context = this,
            lifecycleOwner = this,
            onEncoded = { t, pts, data -> server?.send(t, pts, data) },
            onConfigured = { w, h, fps -> StreamState.update { it.copy(width = w, height = h, fps = fps) } },
        )
        encoder = enc
        enc.setPreviewSurfaceProvider(previewProvider)
        val srv = StreamServer(Protocol.DEFAULT_PORT, this)
        try {
            srv.start()
        } catch (e: IOException) {
            Log.e(TAG, "Could not open port ${Protocol.DEFAULT_PORT}", e)
            StreamState.update { it.copy(error = "Port ${Protocol.DEFAULT_PORT} is in use") }
        }
        server = srv
        val s = StreamState.state.value
        enc.start(CameraEncoder.Settings(s.preset.width, s.preset.height, s.fps, s.camera))
        StreamState.update { it.copy(running = true, error = null) }
    }

    private fun stopStreaming() {
        server?.stop()
        server = null
        encoder?.release()
        encoder = null
        releaseLocks()
        StreamState.update { it.copy(running = false, viewers = 0, width = 0, height = 0) }
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        if (encoder != null) stopStreaming()
        super.onDestroy()
    }

    fun setPreviewSurfaceProvider(provider: Preview.SurfaceProvider?) {
        previewProvider = provider
        encoder?.setPreviewSurfaceProvider(provider)
    }

    /** Called from the phone UI. */
    fun applySettings(preset: Preset = StreamState.state.value.preset, camera: Int = StreamState.state.value.camera) {
        StreamState.update { it.copy(preset = preset, camera = camera) }
        val fps = StreamState.state.value.fps
        encoder?.start(CameraEncoder.Settings(preset.width, preset.height, fps, camera))
    }

    // StreamServer.Controller

    override fun configure(request: Protocol.StreamRequest): Triple<Int, Int, Int>? {
        val enc = encoder ?: return null
        val current = StreamState.state.value
        val preset = if (request.height > 0) Preset.closest(request.height) else current.preset
        val fps = if (request.fps in 1..60) request.fps else current.fps
        val camera = if (request.camera == Protocol.CAMERA_FRONT) Protocol.CAMERA_FRONT else Protocol.CAMERA_BACK
        val changed = preset != current.preset || fps != current.fps || camera != current.camera || enc.settings == null
        val before = enc.generation
        if (changed) {
            StreamState.update { it.copy(preset = preset, fps = fps, camera = camera) }
            enc.start(CameraEncoder.Settings(preset.width, preset.height, fps, camera))
        }
        // Wait for the encoder to (re)start so the reply carries the real frame size.
        val deadline = System.currentTimeMillis() + 5000
        while (System.currentTimeMillis() < deadline) {
            val s = StreamState.state.value
            val ready = s.width > 0 && (!changed || enc.generation > before)
            if (ready) return Triple(s.width, s.height, s.fps)
            Thread.sleep(50)
        }
        return null
    }

    override fun onControl(control: Protocol.Control) {
        when (control.type) {
            Protocol.CTRL_SWITCH_CAMERA -> {
                val camera = control.payload.firstOrNull()?.toInt()
                    ?: (1 - StreamState.state.value.camera)
                applySettings(camera = if (camera == Protocol.CAMERA_FRONT) Protocol.CAMERA_FRONT else Protocol.CAMERA_BACK)
            }
            Protocol.CTRL_SET_RESOLUTION -> {
                val (_, h, fps) = Protocol.parseResolution(control.payload) ?: return
                if (fps in 1..60) StreamState.update { it.copy(fps = fps) }
                applySettings(preset = Preset.closest(h))
            }
        }
    }

    override fun requestKeyFrame() {
        encoder?.requestKeyFrame()
    }

    override fun onViewersChanged(count: Int) {
        StreamState.update { it.copy(viewers = count) }
    }

    @SuppressLint("WakelockTimeout")
    private fun acquireLocks() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "fyewebcam:stream").apply { acquire() }
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wm.createWifiLock(mode, "fyewebcam:stream").apply { acquire() }
    }

    private fun releaseLocks() {
        wakeLock?.takeIf { it.isHeld }?.release()
        wakeLock = null
        wifiLock?.takeIf { it.isHeld }?.release()
        wifiLock = null
    }

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL_ID, "Streaming", NotificationManager.IMPORTANCE_LOW))
    }

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, StreamService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_camera)
            .setContentTitle("fyewebcam is streaming")
            .setContentText("Port ${Protocol.DEFAULT_PORT}")
            .setOngoing(true)
            .setContentIntent(open)
            .addAction(0, "Stop", stop)
            .build()
    }

    companion object {
        private const val TAG = "StreamService"
        private const val CHANNEL_ID = "stream"
        private const val NOTIFICATION_ID = 1
        const val ACTION_START = "com.fyewebcam.START"
        const val ACTION_STOP = "com.fyewebcam.STOP"

        fun start(context: Context) {
            context.startForegroundService(Intent(context, StreamService::class.java).setAction(ACTION_START))
        }

        fun stop(context: Context) {
            context.startService(Intent(context, StreamService::class.java).setAction(ACTION_STOP))
        }
    }
}
