package br.com.antiruido.obd

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothSocket
import android.os.SystemClock
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

/**
 * Lê a rotação do motor (PID 01 0C) de um adaptador ELM327 Bluetooth clássico (SPP).
 *
 * Opcional: sem ele o app acha o ronco só pelo microfone. Com ele, a busca fica restrita às
 * ordens do motor e o rastreamento não se confunde com música ou ruído de pista.
 */
class ObdRpmReader(
    private val address: String,
    private val onRpm: (Float) -> Unit,
    private val onState: (String) -> Unit,
) {
    @Volatile private var running = false
    @Volatile private var socket: BluetoothSocket? = null
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({ loop() }, "obd-rpm").apply {
            isDaemon = true
            start()
        }
    }

    fun stop() {
        running = false
        try {
            socket?.close()
        } catch (_: IOException) {
        }
        thread?.interrupt()
        thread = null
    }

    private fun loop() {
        var backoffMs = 2000L
        while (running) {
            try {
                onState("conectando…")
                connect().use { s ->
                    socket = s
                    val inp = s.inputStream
                    val out = s.outputStream
                    // Reset e formato de resposta compacto: sem eco, sem espaços, sem cabeçalhos.
                    for (cmd in listOf("ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0")) {
                        command(out, inp, cmd, 3000)
                    }
                    onState("conectado")
                    backoffMs = 2000L
                    var failures = 0
                    while (running) {
                        val t0 = SystemClock.elapsedRealtime()
                        val rpm = parseRpm(command(out, inp, "010C", 1000))
                        if (rpm != null) {
                            failures = 0
                            onRpm(rpm)
                        } else if (++failures > 20) {
                            throw IOException("sem resposta de RPM (ignição desligada?)")
                        }
                        // ~10 leituras por segundo: o rastreador só precisa da faixa aproximada.
                        val wait = 100 - (SystemClock.elapsedRealtime() - t0)
                        if (wait > 0) Thread.sleep(wait)
                    }
                }
            } catch (e: InterruptedException) {
                break
            } catch (e: Exception) {
                onState("erro: ${e.message ?: e.javaClass.simpleName}")
            } finally {
                socket = null
            }
            if (!running) break
            try {
                Thread.sleep(backoffMs)
            } catch (e: InterruptedException) {
                break
            }
            backoffMs = (backoffMs * 2).coerceAtMost(30_000L)
        }
        onState("desligado")
    }

    @SuppressLint("MissingPermission")  // a Activity pede BLUETOOTH_CONNECT antes de ligar o OBD
    private fun connect(): BluetoothSocket {
        @Suppress("DEPRECATION")
        val adapter = BluetoothAdapter.getDefaultAdapter()
            ?: throw IOException("Bluetooth do Android indisponível nesta multimídia")
        val device = adapter.getRemoteDevice(address)
        adapter.cancelDiscovery()
        val s = try {
            device.createRfcommSocketToServiceRecord(SPP_UUID).also { it.connect() }
        } catch (e: IOException) {
            // Vários clones de ELM327 só aceitam conexão "insegura".
            device.createInsecureRfcommSocketToServiceRecord(SPP_UUID).also { it.connect() }
        }
        return s
    }

    private fun command(out: OutputStream, inp: InputStream, cmd: String, timeoutMs: Long): String {
        out.write((cmd + "\r").toByteArray(Charsets.US_ASCII))
        out.flush()
        val sb = StringBuilder()
        val deadline = SystemClock.elapsedRealtime() + timeoutMs
        while (SystemClock.elapsedRealtime() < deadline) {
            if (inp.available() > 0) {
                val c = inp.read()
                if (c < 0) throw IOException("conexão fechada")
                if (c == '>'.code) break  // prompt do ELM327: resposta completa
                sb.append(c.toChar())
            } else {
                Thread.sleep(5)
            }
        }
        return sb.toString()
    }

    companion object {
        private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

        /** "410C1AF8" -> (0x1A * 256 + 0xF8) / 4 = 1726 rpm. */
        fun parseRpm(response: String): Float? {
            val clean = response.uppercase().replace(" ", "").replace("\r", "\n")
            for (line in clean.split("\n")) {
                val i = line.indexOf("410C")
                if (i >= 0 && line.length >= i + 8) {
                    val a = line.substring(i + 4, i + 6).toIntOrNull(16) ?: continue
                    val b = line.substring(i + 6, i + 8).toIntOrNull(16) ?: continue
                    return (a * 256 + b) / 4f
                }
            }
            return null
        }
    }
}
