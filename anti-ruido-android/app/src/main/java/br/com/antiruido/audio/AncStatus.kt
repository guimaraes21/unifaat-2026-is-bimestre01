package br.com.antiruido.audio

/** Leitura do vetor de status preenchido por jni_bridge.cpp (mesmo layout, mesma ordem). */
class AncStatus(private val v: FloatArray) {
    companion object {
        private const val HEADER = 27
        private const val HARMONIC_FIELDS = 10
        const val HARMONICS = 4
        const val SIZE = HEADER + HARMONICS * HARMONIC_FIELDS

        const val ERR_NONE = 0
        const val ERR_NEED_CALIBRATION = 1
        const val ERR_RATE_MISMATCH = 2
        const val ERR_NO_CORRELATION = 3
        const val ERR_FEW_POINTS = 4
        const val ERR_DELAY_TOO_LARGE = 5
        const val ERR_UNSTABLE = 6
        const val ERR_GLITCH = 7

        const val CAL_BURST = 1
        const val CAL_ANALYZE = 2
        const val CAL_SWEEP = 3
    }

    val mode get() = v[0].toInt()
    val error get() = v[1].toInt()
    val calStage get() = v[2].toInt()
    val calProgress get() = v[3]
    val calStep get() = v[4].toInt()
    val calSteps get() = v[5].toInt()
    val latencyMs get() = v[6]
    val pathGainDb get() = v[7]
    val latencyConfidence get() = v[8]
    val latencyVerified get() = v[9] > 0.5f
    val toneFound get() = v[10] > 0.5f
    val toneHz get() = v[11]
    val toneProminenceDb get() = v[12]
    val toneLevelDb get() = v[13]
    val locked get() = v[14] > 0.5f
    val ancFreq get() = v[15]
    val micDb get() = v[16]
    val outDb get() = v[17]
    val divergences get() = v[18].toInt()
    val limiterHits get() = v[19].toInt()
    val discontinuities get() = v[20].toInt()
    val modelValid get() = v[21] > 0.5f
    val modelRate get() = v[22].toInt()
    val modelPoints get() = v[23].toInt()
    val modelSerial get() = v[24].toInt()
    val sampleRate get() = v[25].toInt()
    val lag get() = v[26]

    fun harmonic(k: Int) = Harmonic(v, HEADER + k * HARMONIC_FIELDS)

    class Harmonic(private val v: FloatArray, private val o: Int) {
        val active get() = v[o] > 0.5f
        val freq get() = v[o + 1]
        val errorAmp get() = v[o + 2]
        val disturbanceAmp get() = v[o + 3]
        val outputAmp get() = v[o + 4]
        val reductionDb get() = v[o + 5]
        val verified get() = v[o + 6] > 0.5f
        val corrDb get() = v[o + 7]
        val corrDeg get() = v[o + 8]
        val pll get() = v[o + 9] > 0.5f
    }
}
