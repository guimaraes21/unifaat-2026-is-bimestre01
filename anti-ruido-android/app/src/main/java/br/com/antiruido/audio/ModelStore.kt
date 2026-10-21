package br.com.antiruido.audio

import android.content.Context
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException

/** Guarda o modelo do caminho secundário (resultado da calibração) num arquivo privado. */
class ModelStore(context: Context) {
    private val file = File(context.filesDir, "caminho_secundario.bin")

    fun save(model: FloatArray) {
        val tmp = File(file.parentFile, file.name + ".tmp")
        DataOutputStream(tmp.outputStream().buffered()).use { out ->
            out.writeInt(MAGIC)
            out.writeInt(model.size)
            for (f in model) out.writeFloat(f)
        }
        if (!tmp.renameTo(file)) {
            tmp.copyTo(file, overwrite = true)
            tmp.delete()
        }
    }

    fun load(): FloatArray? {
        if (!file.exists()) return null
        return try {
            DataInputStream(file.inputStream().buffered()).use { inp ->
                if (inp.readInt() != MAGIC) return null
                val n = inp.readInt()
                if (n <= 0 || n > 100_000) return null
                FloatArray(n) { inp.readFloat() }
            }
        } catch (e: IOException) {
            null
        }
    }

    fun clear() {
        file.delete()
    }

    private companion object {
        const val MAGIC = 0x414E4331  // "ANC1"
    }
}
