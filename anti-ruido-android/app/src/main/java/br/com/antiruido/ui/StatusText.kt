package br.com.antiruido.ui

import br.com.antiruido.audio.AncStatus
import br.com.antiruido.audio.EngineInfo
import br.com.antiruido.audio.NativeAnc
import java.util.Locale

/** Textos de status em português, usados na tela e na notificação. */
object StatusText {
    private val br: Locale = Locale.Builder().setLanguage("pt").setRegion("BR").build()

    private fun f(fmt: String, vararg args: Any) = String.format(br, fmt, *args)

    fun error(code: Int): String? = when (code) {
        AncStatus.ERR_NONE -> null
        AncStatus.ERR_NEED_CALIBRATION ->
            "Calibração necessária: toque em Calibrar (carro parado, motor desligado, em silêncio)."
        AncStatus.ERR_RATE_MISMATCH ->
            "A taxa de amostragem do áudio mudou desde a calibração. Calibre de novo."
        AncStatus.ERR_NO_CORRELATION ->
            "O microfone não ouviu o chiado de teste. Aumente o volume, confira se o som sai dos " +
                "alto-falantes e se o microfone da multimídia está disponível para apps."
        AncStatus.ERR_FEW_POINTS ->
            "A varredura de tons teve poucos pontos válidos. Aumente o volume ou o nível dos tons " +
                "e calibre em silêncio."
        AncStatus.ERR_DELAY_TOO_LARGE ->
            "Latência de áudio acima de 800 ms. Tente o modo de baixa latência."
        AncStatus.ERR_UNSTABLE ->
            "O cancelamento ficou instável e foi desligado por segurança. Recalibre (volume, " +
                "equalizador, fader ou posição do microfone mudaram?)."
        AncStatus.ERR_GLITCH ->
            "Falha de áudio (xrun) durante a calibração. Feche outros apps pesados e tente de novo."
        else -> "Erro $code"
    }

    private fun calibrationText(s: AncStatus): String = when (s.calStage) {
        AncStatus.CAL_BURST -> "medindo latência (chiado)"
        AncStatus.CAL_ANALYZE -> "calculando"
        AncStatus.CAL_SWEEP -> f("tons graves %d/%d", s.calStep, s.calSteps)
        else -> ""
    }

    fun headline(s: AncStatus): String = when (s.mode) {
        NativeAnc.MODE_CALIBRATE -> f("Calibrando… %d%%", (s.calProgress * 100).toInt())
        NativeAnc.MODE_MEASURE_LATENCY -> "Medindo latência…"
        NativeAnc.MODE_ANC -> {
            val h = s.harmonic(0)
            if (!s.locked) {
                "ANC ligado — procurando o ronco"
            } else if (h.active) {
                f("ANC ligado — %.1f Hz, %.0f dB", s.ancFreq, h.reductionDb)
            } else {
                f("ANC ligado — %.1f Hz (fora da faixa do alto-falante)", s.ancFreq)
            }
        }
        else -> if (s.toneFound) f("Monitorando — ronco em %.1f Hz", s.toneHz) else "Monitorando"
    }

    fun short(s: AncStatus): String {
        error(s.error)?.let { return it }
        return headline(s)
    }

    fun details(s: AncStatus, e: EngineInfo, rpm: Float, obd: String, obdOn: Boolean): String {
        val sb = StringBuilder()
        if (s.mode == NativeAnc.MODE_CALIBRATE) sb.append("Calibração: ").append(calibrationText(s)).append('\n')
        sb.append(f("Microfone %.0f dBFS · saída %.0f dBFS\n", s.micDb, s.outDb))
        if (s.toneFound) {
            sb.append(f("Ronco detectado: %.2f Hz (%.0f dB acima do fundo, %.0f dBFS)\n",
                s.toneHz, s.toneProminenceDb, s.toneLevelDb))
        } else {
            sb.append("Nenhum tom destacado na faixa\n")
        }
        if (obdOn) sb.append(f("OBD-II: %s · %.0f rpm\n", obd, rpm))
        sb.append(f("Latência total %.1f ms%s · correção de ganho %+.1f dB\n", s.latencyMs,
            if (s.latencyVerified) "" else " (não conferida)", s.pathGainDb))
        sb.append(if (s.modelValid) {
            f("Calibração: %d pontos válidos (%d Hz)\n", s.modelPoints, s.modelRate)
        } else {
            "Calibração: nenhuma\n"
        })
        if (s.mode == NativeAnc.MODE_ANC && s.locked) {
            sb.append("Harmônica  freq     redução  saída   modelo\n")
            for (k in 0 until AncStatus.HARMONICS) {
                val h = s.harmonic(k)
                if (h.freq <= 0f || (!h.active && h.outputAmp <= 0f)) continue
                val model = when {
                    !h.active -> "inativa"
                    h.verified -> f("ok %+.0fdB %+.0f°", h.corrDb, h.corrDeg)
                    else -> "a conferir"
                }
                sb.append(f("  %d×     %6.1f Hz  %5.1f dB  %5.3f  %s%s\n", k + 1, h.freq, h.reductionDb,
                    h.outputAmp, model, if (h.pll) " PLL" else ""))
            }
        }
        sb.append(f("Áudio: %s, %d Hz, bloco %d/%d, buffer %d, xruns %d/%d, reinícios %d\n",
            e.api, e.sampleRate, e.outBurst, e.inBurst, e.outBuffer, e.outXruns, e.inXruns, e.restarts))
        if (e.outLatencyMs >= 0f || e.inLatencyMs >= 0f) {
            sb.append(f("Latência informada pelo Android: saída %.0f ms, entrada %.0f ms\n",
                e.outLatencyMs, e.inLatencyMs))
        }
        sb.append(f("Divergências %d · limitador %d · re-medições por falha %d",
            s.divergences, s.limiterHits, s.discontinuities))
        return sb.toString()
    }
}
