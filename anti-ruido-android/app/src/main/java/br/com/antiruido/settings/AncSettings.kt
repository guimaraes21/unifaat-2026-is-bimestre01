package br.com.antiruido.settings

import android.content.Context
import android.content.SharedPreferences
import br.com.antiruido.audio.AncConfig
import br.com.antiruido.audio.NativeAnc

/** Configurações persistentes do app (SharedPreferences). */
class AncSettings(context: Context) {
    private val p: SharedPreferences =
        context.applicationContext.getSharedPreferences("anti_ruido", Context.MODE_PRIVATE)

    var harmonics: Int
        get() = p.getInt("harmonics", 2)
        set(v) = p.edit().putInt("harmonics", v.coerceIn(1, 4)).apply()

    /** Velocidade de adaptação (ganho de malha x atraso). 0,2 = padrão seguro. */
    var convergence: Float
        get() = p.getFloat("convergence", 0.2f)
        set(v) = p.edit().putFloat("convergence", v.coerceIn(0.05f, 0.5f)).apply()

    /** Amplitude máxima da anti-onda (fundo de escala = 1). Protege alto-falantes e ouvidos. */
    var maxOutput: Float
        get() = p.getFloat("max_output", 0.25f)
        set(v) = p.edit().putFloat("max_output", v.coerceIn(0.02f, 0.6f)).apply()

    var fmin: Float
        get() = p.getFloat("fmin", 25f)
        set(v) = p.edit().putFloat("fmin", v.coerceIn(15f, 200f)).apply()

    var fmax: Float
        get() = p.getFloat("fmax", 250f)
        set(v) = p.edit().putFloat("fmax", v.coerceIn(40f, 300f)).apply()

    var minProminenceDb: Float
        get() = p.getFloat("min_prominence", 10f)
        set(v) = p.edit().putFloat("min_prominence", v.coerceIn(4f, 30f)).apply()

    /** Mede latência e ganho com uma rajada curta a cada vez que o ANC liga. */
    var autoLatency: Boolean
        get() = p.getBoolean("auto_latency", true)
        set(v) = p.edit().putBoolean("auto_latency", v).apply()

    var burstLevel: Float
        get() = p.getFloat("burst_level", 0.05f)
        set(v) = p.edit().putFloat("burst_level", v.coerceIn(0.01f, 0.3f)).apply()

    var probeLevel: Float
        get() = p.getFloat("probe_level", 0.2f)
        set(v) = p.edit().putFloat("probe_level", v.coerceIn(0.03f, 0.5f)).apply()

    var inputPreset: Int
        get() = p.getInt("input_preset", NativeAnc.PRESET_VOICE_RECOGNITION)
        set(v) = p.edit().putInt("input_preset", v).apply()

    var lowLatency: Boolean
        get() = p.getBoolean("low_latency", false)
        set(v) = p.edit().putBoolean("low_latency", v).apply()

    /** 3, 4 ou 6 cilindros (define as ordens do motor quando o RPM vem do OBD-II). */
    var cylinders: Int
        get() = p.getInt("cylinders", 4)
        set(v) = p.edit().putInt("cylinders", v).apply()

    /** V6 Honda com VCM: alterna entre 6, 4 e 3 cilindros. */
    var vcm: Boolean
        get() = p.getBoolean("vcm", false)
        set(v) = p.edit().putBoolean("vcm", v).apply()

    var obdEnabled: Boolean
        get() = p.getBoolean("obd_enabled", false)
        set(v) = p.edit().putBoolean("obd_enabled", v).apply()

    var obdAddress: String
        get() = p.getString("obd_address", "") ?: ""
        set(v) = p.edit().putString("obd_address", v).apply()

    var obdName: String
        get() = p.getString("obd_name", "") ?: ""
        set(v) = p.edit().putString("obd_name", v).apply()

    /** Re-mede latência/ganho quando o volume do Android muda. */
    var remeasureOnVolume: Boolean
        get() = p.getBoolean("remeasure_on_volume", true)
        set(v) = p.edit().putBoolean("remeasure_on_volume", v).apply()

    /** Liga o ANC automaticamente quando o app abre. */
    var autoStartAnc: Boolean
        get() = p.getBoolean("auto_start_anc", false)
        set(v) = p.edit().putBoolean("auto_start_anc", v).apply()

    fun toConfig() = AncConfig(
        harmonics = harmonics,
        convergence = convergence,
        maxOutput = maxOutput,
        fmin = fmin,
        fmax = fmax,
        minProminenceDb = minProminenceDb,
        autoLatency = autoLatency,
        burstLevel = burstLevel,
        calBurstLevel = (burstLevel * 2f).coerceAtMost(0.3f),
        probeLevel = probeLevel,
    )

    /** Ordens do motor (múltiplos da rotação do virabrequim) em que o ronco costuma aparecer. */
    fun engineOrders(): FloatArray = when {
        cylinders == 6 && vcm -> floatArrayOf(1.5f, 2f, 3f)  // VCM: 3, 4 ou 6 cilindros ativos
        cylinders == 6 -> floatArrayOf(3f)
        cylinders == 3 -> floatArrayOf(1.5f)
        else -> floatArrayOf(2f)  // 4 cilindros: 2ª ordem (duas explosões por volta)
    }
}
