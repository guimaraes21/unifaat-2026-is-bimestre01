package br.com.antiruido

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import br.com.antiruido.audio.AncStatus
import br.com.antiruido.audio.ModelStore
import br.com.antiruido.audio.NativeAnc
import br.com.antiruido.obd.ObdRpmReader
import br.com.antiruido.settings.AncSettings
import br.com.antiruido.ui.StatusText

/**
 * Serviço em primeiro plano: mantém o motor de áudio vivo com a multimídia mostrando outro app
 * (mapa, música), salva a calibração, observa volume/saída de áudio e lê o OBD-II.
 */
class AncService : Service() {
    companion object {
        const val ACTION_START = "br.com.antiruido.START"
        const val ACTION_ANC_ON = "br.com.antiruido.ANC_ON"
        const val ACTION_ANC_OFF = "br.com.antiruido.ANC_OFF"
        const val ACTION_CALIBRATE = "br.com.antiruido.CALIBRATE"
        const val ACTION_MEASURE_LATENCY = "br.com.antiruido.MEASURE_LATENCY"
        const val ACTION_APPLY_SETTINGS = "br.com.antiruido.APPLY_SETTINGS"
        const val ACTION_QUIT = "br.com.antiruido.QUIT"

        private const val TAG = "AntiRuido"
        private const val CHANNEL_ID = "anc"
        private const val NOTIFICATION_ID = 1

        @Volatile var engineError: String? = null
            private set
        @Volatile var obdState: String = "desligado"
            private set
        @Volatile var lastRpm: Float = 0f
            private set
        @Volatile var running: Boolean = false
            private set

        fun send(context: Context, action: String) {
            val intent = Intent(context, AncService::class.java).setAction(action)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent) else context.startService(intent)
        }
    }

    private lateinit var settings: AncSettings
    private lateinit var store: ModelStore
    private lateinit var audio: AudioManager
    private val handler = Handler(Looper.getMainLooper())
    private var ancWanted = false
    private var savedSerial = -1
    private var lastVolume = -1
    private var volumeChangedAt = 0L
    private var lastNotification = ""
    private var obd: ObdRpmReader? = null
    private var appliedPreset = -1
    private var appliedLowLatency = false
    private var deviceCallbackRegistered = false

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) = NativeAnc.notifyPathChanged()
        override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) = NativeAnc.notifyPathChanged()
    }

    private val tick = object : Runnable {
        override fun run() {
            poll()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate() {
        super.onCreate()
        settings = AncSettings(this)
        store = ModelStore(this)
        audio = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        createChannel()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        goForeground(getString(R.string.notif_starting))
        when (intent?.action) {
            ACTION_QUIT -> {
                shutdown()
                return START_NOT_STICKY
            }
            ACTION_APPLY_SETTINGS -> applySettings()
            ACTION_ANC_ON -> {
                if (ensureEngine()) {
                    ancWanted = true
                    NativeAnc.requestMode(NativeAnc.MODE_ANC)
                }
            }
            ACTION_ANC_OFF -> {
                ancWanted = false
                if (NativeAnc.isRunning()) NativeAnc.requestMode(NativeAnc.MODE_MONITOR)
            }
            ACTION_CALIBRATE -> {
                if (ensureEngine()) {
                    ancWanted = false
                    NativeAnc.requestMode(NativeAnc.MODE_CALIBRATE)
                }
            }
            ACTION_MEASURE_LATENCY -> {
                if (ensureEngine()) NativeAnc.requestMode(NativeAnc.MODE_MEASURE_LATENCY)
            }
            else -> {
                if (ensureEngine() && settings.autoStartAnc) {
                    ancWanted = true
                    NativeAnc.requestMode(NativeAnc.MODE_ANC)
                }
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacks(tick)
        stopObd()
        unregisterDevices()
        if (NativeAnc.isRunning()) NativeAnc.stop()
        running = false
        super.onDestroy()
    }

    private fun unregisterDevices() {
        if (deviceCallbackRegistered) {
            audio.unregisterAudioDeviceCallback(deviceCallback)
            deviceCallbackRegistered = false
        }
    }

    private fun ensureEngine(): Boolean {
        if (NativeAnc.isRunning()) return true
        NativeAnc.apply(settings.toConfig())
        val err = NativeAnc.start(settings.inputPreset, settings.lowLatency)
        engineError = err
        if (err != null) {
            Log.w(TAG, "motor não iniciou: $err")
            goForeground(err)
            return false
        }
        appliedPreset = settings.inputPreset
        appliedLowLatency = settings.lowLatency
        store.load()?.let { if (!NativeAnc.loadModel(it)) Log.w(TAG, "calibração salva inválida") }
        savedSerial = NativeAnc.status().modelSerial
        if (!deviceCallbackRegistered) {
            audio.registerAudioDeviceCallback(deviceCallback, handler)
            deviceCallbackRegistered = true
        }
        running = true
        handler.removeCallbacks(tick)
        handler.post(tick)
        startObdIfEnabled()
        return true
    }

    private fun applySettings() {
        NativeAnc.apply(settings.toConfig())
        if (NativeAnc.isRunning() &&
            (settings.inputPreset != appliedPreset || settings.lowLatency != appliedLowLatency)
        ) {
            // Microfone/modo de desempenho só mudam reabrindo os streams.
            NativeAnc.stop()
            if (ensureEngine() && ancWanted) NativeAnc.requestMode(NativeAnc.MODE_ANC)
        }
        stopObd()
        startObdIfEnabled()
    }

    private fun poll() {
        if (!NativeAnc.isRunning()) return
        val s = NativeAnc.status()

        // Calibração nova: salva para as próximas vezes.
        if (s.modelSerial != savedSerial && s.modelValid) {
            NativeAnc.exportModel()?.let {
                store.save(it)
                savedSerial = s.modelSerial
            }
        }
        // O processador desistiu (instabilidade, sem calibração): não insiste sozinho.
        if (ancWanted && s.mode == NativeAnc.MODE_MONITOR && s.error != AncStatus.ERR_NONE) ancWanted = false

        watchVolume()
        updateNotification(s)
    }

    /** Volume mudou: espera o usuário terminar de girar o botão e pede nova medição de ganho. */
    private fun watchVolume() {
        val v = audio.getStreamVolume(AudioManager.STREAM_MUSIC)
        val now = SystemClock.elapsedRealtime()
        if (lastVolume >= 0 && v != lastVolume) volumeChangedAt = now
        lastVolume = v
        if (volumeChangedAt > 0 && now - volumeChangedAt > 1500) {
            volumeChangedAt = 0
            if (settings.remeasureOnVolume) NativeAnc.notifyPathChanged()
        }
    }

    private fun startObdIfEnabled() {
        if (!settings.obdEnabled || settings.obdAddress.isEmpty() || obd != null) return
        val orders = settings.engineOrders()
        obd = ObdRpmReader(
            settings.obdAddress,
            onRpm = { rpm ->
                lastRpm = rpm
                val base = rpm / 60f
                NativeAnc.setHints(FloatArray(orders.size) { orders[it] * base })
            },
            onState = { state ->
                obdState = state
                if (state != "conectado") NativeAnc.setHints(FloatArray(0))
            },
        ).also { it.start() }
    }

    private fun stopObd() {
        obd?.stop()
        obd = null
        NativeAnc.setHints(FloatArray(0))
        obdState = "desligado"
    }

    private fun shutdown() {
        ancWanted = false
        handler.removeCallbacks(tick)
        stopObd()
        unregisterDevices()
        if (NativeAnc.isRunning()) NativeAnc.stop()
        running = false
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    // ---------------------------------------------------------------------------------------
    // Notificação

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < 26) return
        val ch = NotificationChannel(CHANNEL_ID, getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW)
        ch.setShowBadge(false)
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        fun action(title: String, act: String, code: Int): Notification.Action {
            val pi = PendingIntent.getService(
                this, code, Intent(this, AncService::class.java).setAction(act),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            return Notification.Action.Builder(Icon.createWithResource(this, R.drawable.ic_notification), title, pi)
                .build()
        }
        val b = if (Build.VERSION.SDK_INT >= 26) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        b.setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
        if (ancWanted) {
            b.addAction(action(getString(R.string.btn_anc_off), ACTION_ANC_OFF, 1))
        } else {
            b.addAction(action(getString(R.string.btn_anc_on), ACTION_ANC_ON, 2))
        }
        b.addAction(action(getString(R.string.btn_quit), ACTION_QUIT, 3))
        return b.build()
    }

    private fun goForeground(text: String) {
        val n = buildNotification(text)
        if (Build.VERSION.SDK_INT >= 30) {
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NOTIFICATION_ID, n)
        }
        lastNotification = text
    }

    private fun updateNotification(s: AncStatus) {
        val text = StatusText.short(s)
        if (text == lastNotification) return
        lastNotification = text
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIFICATION_ID, buildNotification(text))
    }
}
