package com.karute.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.ArrayDeque
import java.util.UUID

/**
 * Owns the BLE connection: scans for a device advertising "karute-ble",
 * connects, subscribes to the NUS TX characteristic, and serializes writes to
 * the RX characteristic through a queue (one outstanding write at a time).
 */
@SuppressLint("MissingPermission")
class BleManager(private val context: Context) {

    enum class State { IDLE, SCANNING, CONNECTING, CONNECTED }

    var onState: ((State) -> Unit)? = null
    var onLine: ((String) -> Unit)? = null

    private val app = context.applicationContext
    private val manager = app.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter = manager.adapter
    private val handler = Handler(Looper.getMainLooper())

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null

    private val queue = ArrayDeque<String>()
    private var writing = false
    private var pendingRx = ""

    val isConnected: Boolean get() = gatt != null && rxChar != null

    fun scan(timeoutMs: Long = 8000, onFound: (BluetoothDevice) -> Unit) {
        val scanner = adapter.bluetoothLeScanner ?: return
        onState?.invoke(State.SCANNING)
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val name = result.device.name ?: return
                if (name.startsWith("karute-ble")) {
                    handler.post {
                        scanner.stopScan(this)
                        onState?.invoke(State.CONNECTING)
                        onFound(result.device)
                    }
                }
            }
        }
        scanner.startScan(cb)
        handler.postDelayed({
            scanner.stopScan(cb)
            onState?.invoke(State.IDLE)
        }, timeoutMs)
    }

    fun connect(device: BluetoothDevice) {
        gatt = device.connectGatt(app, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rxChar = null
        queue.clear()
        writing = false
        onState?.invoke(State.IDLE)
    }

    /** Append one command line; writes are serialized one at a time. */
    fun enqueue(line: String) {
        queue.addLast(line)
        pump()
    }

    private fun pump() {
        val g = gatt ?: return
        val rx = rxChar ?: return
        if (writing) return
        val next = queue.removeFirstOrNull() ?: return
        writing = true
        val data = next.toByteArray(Charsets.UTF_8)
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(rx, data, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
        } else {
            @Suppress("DEPRECATION")
            rx.value = data
            rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            g.writeCharacteristic(rx)
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> g.discoverServices()
                BluetoothProfile.STATE_DISCONNECTED -> {
                    g.close()
                    if (gatt === g) {
                        gatt = null
                        rxChar = null
                    }
                    writing = false
                    onState?.invoke(State.IDLE)
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(UUID.fromString(Protocol.SERVICE_UUID)) ?: return
            rxChar = svc.getCharacteristic(UUID.fromString(Protocol.RX_UUID))
            val tx = svc.getCharacteristic(UUID.fromString(Protocol.TX_UUID)) ?: return
            g.setCharacteristicNotification(tx, true)
            val ccc = tx.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"))
            if (ccc != null) {
                if (Build.VERSION.SDK_INT >= 33) {
                    g.writeDescriptor(ccc, byteArrayOf(1, 0), BluetoothGatt.WRITE_TYPE_DEFAULT)
                } else {
                    @Suppress("DEPRECATION")
                    ccc.value = byteArrayOf(1, 0)
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(ccc)
                }
            }
            onState?.invoke(State.CONNECTED)
            pump()
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) {
            writing = false
            handler.post { pump() }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            val chunk = ch.value ?: return
            pendingRx += String(chunk, Charsets.UTF_8)
            while (pendingRx.contains("\n")) {
                val i = pendingRx.indexOf('\n')
                val line = pendingRx.substring(0, i)
                pendingRx = pendingRx.substring(i + 1)
                handler.post { onLine?.invoke(line) }
            }
        }
    }
}
