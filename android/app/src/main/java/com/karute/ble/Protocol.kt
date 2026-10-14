package com.karute.ble

import android.util.Base64

/**
 * Wire protocol — must stay in lockstep with src/ble_protocol.h on the device.
 *
 * Each command is one '\n'-terminated line written to the NUS RX characteristic.
 */
object Protocol {
    const val SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
    const val RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
    const val TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

    /** Max bytes per BLE write; must stay under the negotiated MTU. */
    const val MAX_WRITE = 180

    fun cmd(name: String) = "C $name\n"

    fun reset() = cmd("RESET")
    fun start() = cmd("START")
    fun pause() = cmd("PAUSE")
    fun resume() = cmd("RESUME")
    fun stop() = cmd("STOP")

    /** Encode a whole UTF-8 string into as many T lines as needed. */
    fun encodeTextLines(raw: String): List<String> {
        val b64 = Base64.encodeToString(raw.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
        val out = ArrayList<String>()
        var i = 0
        while (i < b64.length) {
            val end = minOf(i + MAX_WRITE - 2, b64.length)
            out.add("T " + b64.substring(i, end) + "\n")
            i = end
        }
        if (out.isEmpty()) out.add("T \n")  // empty text marker
        return out
    }

    /** Build one S line from key=value pairs. */
    fun settingsLine(pairs: Map<String, String>): String {
        val joined = pairs.entries.joinToString(" ") { "${it.key}=${it.value}" }
        return "S $joined\n"
    }

    /** Parse a STATE line back into fields. */
    data class State(
        val state: String,
        val bytes: Int,
        val characters: Int,
        val progressPct: Int,
        val remainingMs: Long,
        val error: String
    )

    fun parseState(line: String): State? {
        if (!line.startsWith("STATE ")) return null
        val parts = line.removePrefix("STATE ").split("|", limit = 6)
        if (parts.size < 6) return null
        return State(
            state = parts[0],
            bytes = parts[1].toIntOrNull() ?: 0,
            characters = parts[2].toIntOrNull() ?: 0,
            progressPct = parts[3].toIntOrNull() ?: 0,
            remainingMs = parts[4].toLongOrNull() ?: 0,
            error = parts[5]
        )
    }
}
