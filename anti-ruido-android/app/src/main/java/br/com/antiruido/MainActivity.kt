package br.com.antiruido

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothManager
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import br.com.antiruido.audio.AncStatus
import br.com.antiruido.audio.NativeAnc
import br.com.antiruido.settings.AncSettings
import br.com.antiruido.ui.SpectrumView
import br.com.antiruido.ui.StatusText

class MainActivity : Activity() {
    private companion object {
        const val REQ_AUDIO = 1
        const val REQ_BLUETOOTH = 2
    }

    private lateinit var settings: AncSettings
    private val handler = Handler(Looper.getMainLooper())
    private val estimate = FloatArray(NativeAnc.SPECTRUM_BINS)
    private val mic = FloatArray(NativeAnc.SPECTRUM_BINS)
    private var pendingAction: String? = null
    private var lastMode = NativeAnc.MODE_MONITOR

    private lateinit var headline: TextView
    private lateinit var errorText: TextView
    private lateinit var calProgress: ProgressBar
    private lateinit var btnAnc: Button
    private lateinit var spectrum: SpectrumView
    private lateinit var details: TextView
    private lateinit var spHarmonics: Spinner
    private lateinit var sbConvergence: SeekBar
    private lateinit var lblConvergence: TextView
    private lateinit var sbMaxOut: SeekBar
    private lateinit var lblMaxOut: TextView
    private lateinit var etFmin: EditText
    private lateinit var etFmax: EditText
    private lateinit var spCylinders: Spinner
    private lateinit var cbVcm: CheckBox
    private lateinit var cbObd: CheckBox
    private lateinit var btnObdDevice: Button
    private lateinit var spPreset: Spinner
    private lateinit var cbAutoLatency: CheckBox
    private lateinit var sbBurst: SeekBar
    private lateinit var lblBurst: TextView
    private lateinit var sbProbe: SeekBar
    private lateinit var lblProbe: TextView
    private lateinit var cbVolume: CheckBox
    private lateinit var cbLowLatency: CheckBox
    private lateinit var cbAutoStart: CheckBox

    private val presets = listOf(
        "Reconhecimento de voz (recomendado)" to NativeAnc.PRESET_VOICE_RECOGNITION,
        "Sem processamento (UNPROCESSED)" to NativeAnc.PRESET_UNPROCESSED,
        "Câmera (CAMCORDER)" to NativeAnc.PRESET_CAMCORDER,
        "Genérico (MIC)" to NativeAnc.PRESET_GENERIC,
        "Chamada de voz (tem cancelamento de eco: evite)" to NativeAnc.PRESET_VOICE_COMMUNICATION,
    )
    private val cylinderOptions = listOf(3, 4, 6)

    private val poll = object : Runnable {
        override fun run() {
            refresh()
            handler.postDelayed(this, 250)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        settings = AncSettings(this)
        bindViews()
        loadSettingsIntoViews()

        btnAnc.setOnClickListener {
            val on = lastMode == NativeAnc.MODE_ANC || lastMode == NativeAnc.MODE_MEASURE_LATENCY
            runWithPermission(if (on) AncService.ACTION_ANC_OFF else AncService.ACTION_ANC_ON)
        }
        view<Button>(R.id.btnCalibrate).setOnClickListener { confirmCalibration() }
        view<Button>(R.id.btnLatency).setOnClickListener {
            runWithPermission(AncService.ACTION_MEASURE_LATENCY)
        }
        view<Button>(R.id.btnQuit).setOnClickListener {
            if (AncService.running) AncService.send(this, AncService.ACTION_QUIT)
            finish()
        }
        view<Button>(R.id.btnApply).setOnClickListener { saveSettingsFromViews() }
        btnObdDevice.setOnClickListener { pickObdDevice() }

        runWithPermission(AncService.ACTION_START)
    }

    override fun onResume() {
        super.onResume()
        handler.post(poll)
    }

    override fun onPause() {
        handler.removeCallbacks(poll)
        super.onPause()
    }

    // -----------------------------------------------------------------------------------------
    // Permissões e ações

    private fun runWithPermission(action: String) {
        val needed = mutableListOf<String>()
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            needed += Manifest.permission.RECORD_AUDIO
        }
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            needed += Manifest.permission.POST_NOTIFICATIONS
        }
        if (needed.isEmpty()) {
            AncService.send(this, action)
        } else {
            pendingAction = action
            requestPermissions(needed.toTypedArray(), REQ_AUDIO)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        when (requestCode) {
            REQ_AUDIO -> {
                if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                    pendingAction?.let { AncService.send(this, it) }
                } else {
                    Toast.makeText(this, R.string.perm_needed, Toast.LENGTH_LONG).show()
                }
                pendingAction = null
            }
            REQ_BLUETOOTH -> {
                if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) pickObdDevice()
            }
        }
    }

    private fun confirmCalibration() {
        AlertDialog.Builder(this)
            .setTitle(R.string.cal_title)
            .setMessage(R.string.cal_message)
            .setPositiveButton(R.string.cal_start) { _, _ -> runWithPermission(AncService.ACTION_CALIBRATE) }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    @SuppressLint("MissingPermission")  // conferida logo abaixo (API 31+)
    private fun pickObdDevice() {
        if (Build.VERSION.SDK_INT >= 31 &&
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_CONNECT), REQ_BLUETOOTH)
            return
        }
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null) {
            Toast.makeText(this, R.string.obd_unavailable, Toast.LENGTH_LONG).show()
            return
        }
        val devices = try {
            adapter.bondedDevices.toList()
        } catch (e: SecurityException) {
            emptyList()
        }
        if (devices.isEmpty()) {
            Toast.makeText(this, R.string.obd_none, Toast.LENGTH_LONG).show()
            return
        }
        val names = devices.map { "${it.name ?: "?"}  (${it.address})" }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle(R.string.obd_pick)
            .setItems(names) { _, i ->
                settings.obdAddress = devices[i].address
                settings.obdName = devices[i].name ?: devices[i].address
                cbObd.isChecked = true
                updateObdButton()
            }
            .show()
    }

    // -----------------------------------------------------------------------------------------
    // Tela

    private fun refresh() {
        if (!NativeAnc.isRunning()) {
            headline.text = if (AncService.running) "Abrindo o áudio…" else "Parado"
            val err = AncService.engineError
            errorText.visibility = if (err != null) View.VISIBLE else View.GONE
            errorText.text = err ?: ""
            calProgress.visibility = View.GONE
            return
        }
        val s = NativeAnc.status()
        lastMode = s.mode
        headline.text = StatusText.headline(s)
        val err = StatusText.error(s.error)
        errorText.visibility = if (err != null) View.VISIBLE else View.GONE
        errorText.text = err ?: ""
        val calibrating = s.mode == NativeAnc.MODE_CALIBRATE || s.mode == NativeAnc.MODE_MEASURE_LATENCY
        calProgress.visibility = if (calibrating) View.VISIBLE else View.GONE
        calProgress.progress = (s.calProgress * 1000).toInt()
        val ancOn = s.mode == NativeAnc.MODE_ANC || s.mode == NativeAnc.MODE_MEASURE_LATENCY
        btnAnc.setText(if (ancOn) R.string.btn_anc_off else R.string.btn_anc_on)

        val (bins, binHz) = NativeAnc.spectrum(estimate, mic)
        val markers = if (s.mode == NativeAnc.MODE_ANC && s.locked) {
            (0 until AncStatus.HARMONICS).map { s.harmonic(it) }.filter { it.active }.map { it.freq }.toFloatArray()
        } else {
            FloatArray(0)
        }
        spectrum.update(estimate, mic, bins, binHz, markers)
        details.text = StatusText.details(s, NativeAnc.engineInfo(), AncService.lastRpm, AncService.obdState,
            settings.obdEnabled)
    }

    private fun bindViews() {
        headline = view(R.id.headline)
        errorText = view(R.id.errorText)
        calProgress = view(R.id.calProgress)
        btnAnc = view(R.id.btnAnc)
        spectrum = view(R.id.spectrum)
        details = view(R.id.details)
        spHarmonics = view(R.id.spHarmonics)
        sbConvergence = view(R.id.sbConvergence)
        lblConvergence = view(R.id.lblConvergence)
        sbMaxOut = view(R.id.sbMaxOut)
        lblMaxOut = view(R.id.lblMaxOut)
        etFmin = view(R.id.etFmin)
        etFmax = view(R.id.etFmax)
        spCylinders = view(R.id.spCylinders)
        cbVcm = view(R.id.cbVcm)
        cbObd = view(R.id.cbObd)
        btnObdDevice = view(R.id.btnObdDevice)
        spPreset = view(R.id.spPreset)
        cbAutoLatency = view(R.id.cbAutoLatency)
        sbBurst = view(R.id.sbBurst)
        lblBurst = view(R.id.lblBurst)
        sbProbe = view(R.id.sbProbe)
        lblProbe = view(R.id.lblProbe)
        cbVolume = view(R.id.cbVolume)
        cbLowLatency = view(R.id.cbLowLatency)
        cbAutoStart = view(R.id.cbAutoStart)

        spHarmonics.adapter = spinnerAdapter(listOf("1 (só a fundamental)", "2", "3", "4"))
        spCylinders.adapter = spinnerAdapter(cylinderOptions.map { "$it cilindros" })
        spPreset.adapter = spinnerAdapter(presets.map { it.first })

        val labels = object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) = updateSeekLabels()
            override fun onStartTrackingTouch(bar: SeekBar) {}
            override fun onStopTrackingTouch(bar: SeekBar) {}
        }
        for (b in listOf(sbConvergence, sbMaxOut, sbBurst, sbProbe)) b.setOnSeekBarChangeListener(labels)
        spCylinders.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                cbVcm.isEnabled = cylinderOptions[position] == 6
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
    }

    private fun <T : View> view(id: Int): T = checkNotNull(findViewById<T>(id)) { "view ausente: $id" }

    private fun spinnerAdapter(items: List<String>) =
        ArrayAdapter(this, android.R.layout.simple_spinner_item, items).apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }

    // SeekBars em passos de 0,01 a partir de um mínimo (ver limites em AncSettings).
    private fun convergenceValue() = 0.05f + sbConvergence.progress * 0.01f
    private fun maxOutValue() = 0.02f + sbMaxOut.progress * 0.01f
    private fun burstValue() = 0.01f + sbBurst.progress * 0.01f
    private fun probeValue() = 0.03f + sbProbe.progress * 0.01f

    private fun updateSeekLabels() {
        lblConvergence.text = getString(R.string.label_convergence, convergenceValue())
        lblMaxOut.text = getString(R.string.label_max_out, maxOutValue() * 100f)
        lblBurst.text = getString(R.string.label_burst, burstValue() * 100f)
        lblProbe.text = getString(R.string.label_probe, probeValue() * 100f)
    }

    private fun updateObdButton() {
        val name = settings.obdName
        btnObdDevice.text = if (name.isEmpty()) getString(R.string.btn_obd_device) else "OBD-II: $name"
    }

    private fun loadSettingsIntoViews() {
        spHarmonics.setSelection(settings.harmonics - 1)
        sbConvergence.progress = ((settings.convergence - 0.05f) / 0.01f).toInt()
        sbMaxOut.progress = ((settings.maxOutput - 0.02f) / 0.01f).toInt()
        etFmin.setText(settings.fmin.toInt().toString())
        etFmax.setText(settings.fmax.toInt().toString())
        spCylinders.setSelection(cylinderOptions.indexOf(settings.cylinders).coerceAtLeast(0))
        cbVcm.isChecked = settings.vcm
        cbVcm.isEnabled = settings.cylinders == 6
        cbObd.isChecked = settings.obdEnabled
        spPreset.setSelection(presets.indexOfFirst { it.second == settings.inputPreset }.coerceAtLeast(0))
        cbAutoLatency.isChecked = settings.autoLatency
        sbBurst.progress = ((settings.burstLevel - 0.01f) / 0.01f).toInt()
        sbProbe.progress = ((settings.probeLevel - 0.03f) / 0.01f).toInt()
        cbVolume.isChecked = settings.remeasureOnVolume
        cbLowLatency.isChecked = settings.lowLatency
        cbAutoStart.isChecked = settings.autoStartAnc
        updateSeekLabels()
        updateObdButton()
    }

    private fun saveSettingsFromViews() {
        settings.harmonics = spHarmonics.selectedItemPosition + 1
        settings.convergence = convergenceValue()
        settings.maxOutput = maxOutValue()
        val fmin = etFmin.text.toString().replace(',', '.').toFloatOrNull() ?: settings.fmin
        val fmax = etFmax.text.toString().replace(',', '.').toFloatOrNull() ?: settings.fmax
        settings.fmin = minOf(fmin, fmax - 10f)
        settings.fmax = fmax
        settings.cylinders = cylinderOptions[spCylinders.selectedItemPosition]
        settings.vcm = cbVcm.isChecked
        settings.obdEnabled = cbObd.isChecked && settings.obdAddress.isNotEmpty()
        settings.inputPreset = presets[spPreset.selectedItemPosition].second
        settings.autoLatency = cbAutoLatency.isChecked
        settings.burstLevel = burstValue()
        settings.probeLevel = probeValue()
        settings.remeasureOnVolume = cbVolume.isChecked
        settings.lowLatency = cbLowLatency.isChecked
        settings.autoStartAnc = cbAutoStart.isChecked
        loadSettingsIntoViews()
        if (AncService.running) AncService.send(this, AncService.ACTION_APPLY_SETTINGS)
        Toast.makeText(this, R.string.applied, Toast.LENGTH_SHORT).show()
    }
}
