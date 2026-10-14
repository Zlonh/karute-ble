package com.karute.ble

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import com.karute.ble.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var b: ActivityMainBinding
    private lateinit var ble: BleManager

    private val permissions: Array<String>
        get() = when {
            Build.VERSION.SDK_INT >= 31 -> arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
            else -> arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityMainBinding.inflate(layoutInflater)
        setContentView(b.root)

        ble = BleManager(this)
        ble.onState = { st -> runOnUiThread { renderState(st) } }
        ble.onLine = { line -> runOnUiThread { onDeviceLine(line) } }

        val modes = listOf("GBK（Alt+小键盘，打中文）" to "4",
                           "ENG（直敲 ASCII，英文）" to "2",
                           "VUC（vuc+十六进制，依赖输入法）" to "1")
        b.mode.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item,
            modes.map { it.first })

        b.scan.setOnClickListener { ensurePermissions { ble.scan { d -> ble.connect(d) } } }
        b.disconnect.setOnClickListener { ble.disconnect() }

        b.send.setOnClickListener { onSend() }
        b.pause.setOnClickListener { ble.enqueue(Protocol.pause()) }
        b.resume.setOnClickListener { ble.enqueue(Protocol.resume()) }
        b.stop.setOnClickListener { ble.enqueue(Protocol.stop()) }
        b.clear.setOnClickListener {
            b.text.setText("")
            ble.enqueue(Protocol.reset())
        }

        renderState(BleManager.State.IDLE)
    }

    private fun renderState(st: BleManager.State) {
        b.status.text = when (st) {
            BleManager.State.IDLE -> "未连接"
            BleManager.State.SCANNING -> "正在扫描 karute-ble …"
            BleManager.State.CONNECTING -> "正在连接 …"
            BleManager.State.CONNECTED -> "已连接"
        }
        b.send.isEnabled = st == BleManager.State.CONNECTED
        b.pause.isEnabled = st == BleManager.State.CONNECTED
        b.resume.isEnabled = st == BleManager.State.CONNECTED
        b.stop.isEnabled = st == BleManager.State.CONNECTED
        b.clear.isEnabled = st == BleManager.State.CONNECTED
    }

    private fun onSend() {
        val raw = b.text.text.toString()
        val modeValue = listOf("4", "2", "1")[b.mode.selectedItemPosition]
        val down = b.down.text.toString().ifBlank { "5" }
        val interval = b.interval.text.toString().ifBlank { "20" }
        val delaySec = b.startDelay.text.toString().ifBlank { "0" }

        // 1) Reset device draft.
        ble.enqueue(Protocol.reset())

        // 2) Push the text in base64 chunks.
        Protocol.encodeTextLines(raw).forEach { ble.enqueue(it) }

        // 3) Apply settings for the chosen mode.
        val pairs = HashMap<String, String>()
        pairs["mode"] = modeValue
        when (modeValue) {
            "4" -> { pairs["altDownMs"] = down; pairs["altIntervalMs"] = interval }
            "2" -> { pairs["engDownMs"] = down; pairs["engIntervalMs"] = interval }
            "1" -> { pairs["vucDownMs"] = down; pairs["vucIntervalMs"] = interval }
        }
        pairs["startDelayMs"] = (delaySec.toLongOrNull()?.times(1000L) ?: 0L).toString()
        ble.enqueue(Protocol.settingsLine(pairs))

        // 4) Go.
        ble.enqueue(Protocol.start())
        toast("已下发，等待开始打字")
    }

    private fun onDeviceLine(line: String) {
        val state = Protocol.parseState(line)
        if (state != null) {
            b.progress.progress = state.progressPct
            b.progressText.text = "${state.state}  ${state.progressPct}%  剩余 ${state.remainingMs / 1000}s" +
                (if (state.error.isNotEmpty()) "  ⚠ ${state.error}" else "")
            return
        }
        when {
            line.startsWith("ERR") -> toast(line.removePrefix("ERR ").trim())
            line == "OK" -> Unit
            line == "PONG" -> Unit
            line.startsWith("STATE") -> Unit
            else -> toast(line)
        }
    }

    private fun ensurePermissions(block: () -> Unit) {
        val missing = permissions.filter {
            ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) block()
        else ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
            ble.scan { d -> ble.connect(d) }
        } else {
            toast("需要蓝牙权限才能扫描")
        }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}
