package com.allplay.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.util.Log
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue

/**
 * Android implementation of IBluetooth: paired device enumeration + SPP/RFCOMM.
 * Bluetooth socket I/O stays in this class (Android does not expose the socket fd); native downcalls
 * read/write/close via JNI, returning byte arrays.
 */
class BluetoothService {
    private val adapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()

    // handle → connection
    private val sockets = ConcurrentHashMap<Int, BluetoothSocket>()
    private val inStreams = ConcurrentHashMap<Int, InputStream>()
    private val outStreams = ConcurrentHashMap<Int, OutputStream>()
    private val queues = ConcurrentHashMap<Int, LinkedBlockingQueue<ByteArray>>()
    private val nextHandle = java.util.concurrent.atomic.AtomicInteger(1)

    @SuppressLint("MissingPermission")
    fun pairedDevices(): Array<String> {
        val devs = adapter?.bondedDevices ?: return emptyArray()
        return devs.map { "${it.name ?: ""}|${it.address}" }.toTypedArray()
    }

    /// Client side: connect to the phone's SPP service and return a handle (<0 on failure).
    @SuppressLint("MissingPermission")
    fun connectSpp(address: String, uuid: String): Int {
        val dev: BluetoothDevice = adapter?.getRemoteDevice(address) ?: return -1
        return try {
            val socket = dev.createRfcommSocketToServiceRecord(UUID.fromString(uuid))
            socket.connect()
            register(socket)
        } catch (e: Exception) {
            Log.w("AllPlay", "connectSpp failed ${e.javaClass.simpleName}: ${e.message}")
            -1
        }
    }

    /// Server side: register SDP and wait for the phone to connect, returning a handle (<0 on failure).
    @SuppressLint("MissingPermission")
    fun listenSpp(uuid: String, timeoutMs: Int): Int {
        return try {
            val server = adapter?.listenUsingRfcommWithServiceRecord("Wireless iAPv2", UUID.fromString(uuid)) ?: return -1
            val socket = server.accept(timeoutMs) ?: return -1
            register(socket)
        } catch (e: Exception) {
            -1
        }
    }

    /// Native read: null = disconnected, empty array = timeout, otherwise data (≤ maxLen).
    fun read(handle: Int, maxLen: Int, timeoutMs: Int): ByteArray? {
        val q = queues[handle] ?: return null
        val data = q.poll(timeoutMs.toLong(), java.util.concurrent.TimeUnit.MILLISECONDS) ?: return ByteArray(0)
        return if (data.size <= maxLen) data else data.copyOf(maxLen)
    }

    fun write(handle: Int, data: ByteArray): Boolean {
        val out = outStreams[handle] ?: return false
        return try {
            out.write(data); out.flush(); true
        } catch (e: Exception) {
            false
        }
    }

    fun close(handle: Int) {
        try { sockets.remove(handle)?.close() } catch (_: Exception) {}
        inStreams.remove(handle)
        outStreams.remove(handle)
        queues.remove(handle)
    }

    private fun register(socket: BluetoothSocket): Int {
        val handle = nextHandle.getAndIncrement()
        val input = socket.inputStream
        val output = socket.outputStream
        val queue = LinkedBlockingQueue<ByteArray>()
        sockets[handle] = socket
        inStreams[handle] = input
        outStreams[handle] = output
        queues[handle] = queue

        // Dedicated read thread: put received bytes on a queue for native read to poll
        Thread {
            val buf = ByteArray(4096)
            try {
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    queue.put(buf.copyOf(n))
                }
            } catch (_: Exception) {
            } finally {
                queue.put(ByteArray(0))   // sentinel: disconnected
            }
        }.start()
        return handle
    }
}
