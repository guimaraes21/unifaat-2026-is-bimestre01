package br.com.antiruido.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.view.View
import kotlin.math.log10
import kotlin.math.max

/**
 * Espectro de 0 a 300 Hz: o ruído estimado sem ANC (laranja) e o que o microfone ouve de
 * fato (azul). A distância entre as curvas nas linhas verdes é o cancelamento.
 */
class SpectrumView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {
    private val density = resources.displayMetrics.density
    private var estimate = FloatArray(0)
    private var mic = FloatArray(0)
    private var bins = 0
    private var binHz = 1f
    private var markers = FloatArray(0)

    private val maxHz = 300f
    private val topDb = -10f
    private val bottomDb = -100f

    private val grid = Paint().apply {
        color = Color.argb(70, 200, 200, 200)
        strokeWidth = 1f * density
    }
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(200, 220, 220, 220)
        textSize = 11f * density
    }
    private val estPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(255, 152, 0)
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
    }
    private val micPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(66, 165, 245)
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
    }
    private val markerPaint = Paint().apply {
        color = Color.argb(160, 102, 187, 106)
        strokeWidth = 2f * density
    }
    private val path = Path()

    fun update(estimate: FloatArray, mic: FloatArray, bins: Int, binHz: Float, markers: FloatArray) {
        this.estimate = estimate
        this.mic = mic
        this.bins = bins
        this.binHz = if (binHz > 0f) binHz else 1f
        this.markers = markers
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        val left = 34f * density
        val bottom = h - 16f * density
        val plotW = w - left - 4f * density
        val plotH = bottom - 4f * density

        fun x(hz: Float) = left + plotW * (hz / maxHz)
        fun y(db: Float) = 4f * density + plotH * ((topDb - db) / (topDb - bottomDb)).coerceIn(0f, 1f)

        var db = topDb
        while (db >= bottomDb) {
            canvas.drawLine(left, y(db), w, y(db), grid)
            canvas.drawText("${db.toInt()}", 2f, y(db) + 4f * density, label)
            db -= 20f
        }
        var hz = 0f
        while (hz <= maxHz) {
            canvas.drawLine(x(hz), 4f * density, x(hz), bottom, grid)
            canvas.drawText("${hz.toInt()} Hz", x(hz) - 10f * density, h - 2f, label)
            hz += 50f
        }
        for (m in markers) {
            if (m in 1f..maxHz) canvas.drawLine(x(m), 4f * density, x(m), bottom, markerPaint)
        }
        drawCurve(canvas, estimate, estPaint, ::x, ::y)
        drawCurve(canvas, mic, micPaint, ::x, ::y)
    }

    private fun drawCurve(
        canvas: Canvas,
        amp: FloatArray,
        paint: Paint,
        x: (Float) -> Float,
        y: (Float) -> Float,
    ) {
        val n = minOf(bins, amp.size)
        if (n < 2) return
        path.reset()
        var first = true
        for (k in 1 until n) {
            val hz = k * binHz
            if (hz > maxHz) break
            val db = 20f * log10(max(amp[k], 1e-6f))
            if (first) path.moveTo(x(hz), y(db)) else path.lineTo(x(hz), y(db))
            first = false
        }
        canvas.drawPath(path, paint)
    }
}
