package br.com.antiruido.audio

/** Acesso ao motor nativo (C++/Oboe). Um único motor por processo. */
object NativeAnc {
    init {
        System.loadLibrary("antiruido")
    }

    const val MODE_MONITOR = 0
    const val MODE_ANC = 1
    const val MODE_CALIBRATE = 2
    const val MODE_MEASURE_LATENCY = 3

    // Mesmos valores do InputPreset do AAudio/Oboe.
    const val PRESET_GENERIC = 1
    const val PRESET_CAMCORDER = 5
    const val PRESET_VOICE_RECOGNITION = 6
    const val PRESET_VOICE_COMMUNICATION = 7
    const val PRESET_UNPROCESSED = 9

    const val SPECTRUM_BINS = 513

    private val statusBuf = FloatArray(AncStatus.SIZE)
    private val engineBuf = FloatArray(11)

    /** Abre microfone e saída. Retorna null em caso de sucesso ou a mensagem de erro. */
    fun start(inputPreset: Int, lowLatency: Boolean): String? = nativeStart(inputPreset, lowLatency)

    fun stop() = nativeStop()

    fun isRunning(): Boolean = nativeIsRunning()

    fun requestMode(mode: Int) = nativeRequestMode(mode)

    fun apply(s: AncConfig) = nativeSetSettings(
        s.harmonics, s.convergence, s.maxOutput, s.fmin, s.fmax, s.minProminenceDb,
        s.autoLatency, s.burstLevel, s.calBurstLevel, s.probeLevel,
    )

    /** Frequências esperadas do ronco (ex.: ordens do motor calculadas pelo RPM do OBD-II). */
    fun setHints(freqs: FloatArray) = nativeSetHints(freqs)

    /** Volume, saída de áudio ou algo no caminho mudou: no ANC, dispara nova medição. */
    fun notifyPathChanged() = nativeNotifyPathChanged()

    fun loadModel(data: FloatArray): Boolean = nativeLoadModel(data)

    fun exportModel(): FloatArray? = nativeExportModel()

    @Synchronized
    fun status(): AncStatus {
        nativeGetStatus(statusBuf)
        return AncStatus(statusBuf.copyOf())
    }

    /** Preenche os espectros (amplitude linear) e retorna a resolução em Hz por bin. */
    fun spectrum(estimate: FloatArray, mic: FloatArray): Pair<Int, Float> {
        val meta = FloatArray(1)
        val n = nativeGetSpectrum(estimate, mic, meta)
        return n to meta[0]
    }

    @Synchronized
    fun engineInfo(): EngineInfo {
        nativeGetEngineInfo(engineBuf)
        return EngineInfo(engineBuf.copyOf())
    }

    private external fun nativeStart(inputPreset: Int, lowLatency: Boolean): String?
    private external fun nativeStop()
    private external fun nativeIsRunning(): Boolean
    private external fun nativeRequestMode(mode: Int)
    private external fun nativeSetSettings(
        harmonics: Int, convergence: Float, maxOutput: Float, fmin: Float, fmax: Float,
        minProminenceDb: Float, autoLatency: Boolean, burstLevel: Float, calBurstLevel: Float,
        probeLevel: Float,
    )
    private external fun nativeSetHints(freqs: FloatArray?)
    private external fun nativeNotifyPathChanged()
    private external fun nativeLoadModel(data: FloatArray): Boolean
    private external fun nativeExportModel(): FloatArray?
    private external fun nativeGetStatus(out: FloatArray): Int
    private external fun nativeGetSpectrum(estimate: FloatArray, mic: FloatArray, meta: FloatArray): Int
    private external fun nativeGetEngineInfo(out: FloatArray)
}

/** Parâmetros do DSP (subconjunto das configurações do app). */
data class AncConfig(
    val harmonics: Int,
    val convergence: Float,
    val maxOutput: Float,
    val fmin: Float,
    val fmax: Float,
    val minProminenceDb: Float,
    val autoLatency: Boolean,
    val burstLevel: Float,
    val calBurstLevel: Float,
    val probeLevel: Float,
)

class EngineInfo(private val v: FloatArray) {
    val sampleRate get() = v[0].toInt()
    val api get() = when (v[1].toInt()) {
        1 -> "OpenSL ES"
        2 -> "AAudio"
        else -> "?"
    }
    val outBurst get() = v[2].toInt()
    val outBuffer get() = v[3].toInt()
    val inBurst get() = v[4].toInt()
    val outXruns get() = v[5].toInt()
    val inXruns get() = v[6].toInt()
    val outLatencyMs get() = v[7]
    val inLatencyMs get() = v[8]
    val restarts get() = v[9].toInt()
    val callbackFrames get() = v[10].toInt()
}
