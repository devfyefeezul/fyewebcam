package com.fyewebcam

import android.util.Log
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * TCP server that the Windows client connects to. One viewer at a time: a new connection that
 * completes the handshake replaces the old one. Frames are queued per client and dropped (until the next keyframe) when the
 * client falls behind, so latency stays low instead of building up.
 */
class StreamServer(private val port: Int, private val controller: Controller) {

    interface Controller {
        /** Applies the client's request and blocks until the encoder runs. Returns width, height, fps or null. */
        fun configure(request: Protocol.StreamRequest): Triple<Int, Int, Int>?
        fun onControl(control: Protocol.Control)
        fun requestKeyFrame()
        fun onViewersChanged(count: Int)
    }

    private class Packet(val type: Int, val ptsUs: Long, val data: ByteArray)

    private inner class Client(val socket: Socket) {
        val queue = ArrayBlockingQueue<Packet>(QUEUE_SIZE)
        @Volatile var streaming = false
        @Volatile var needKeyFrame = true
        @Volatile var closed = false

        fun close() {
            closed = true
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }
    }

    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null
    @Volatile private var client: Client? = null
    @Volatile private var lastConfig: ByteArray? = null

    fun start() {
        val ss = ServerSocket()
        ss.reuseAddress = true
        ss.bind(InetSocketAddress(port))
        serverSocket = ss
        acceptThread = Thread({ acceptLoop(ss) }, "stream-accept").apply { start() }
    }

    fun stop() {
        try {
            serverSocket?.close()
        } catch (_: IOException) {
        }
        serverSocket = null
        client?.close()
        client = null
        controller.onViewersChanged(0)
    }

    /** Called by the encoder for every encoded buffer. */
    fun send(type: Int, ptsUs: Long, data: ByteArray) {
        if (type == Protocol.FRAME_CONFIG) lastConfig = data
        val c = client ?: return
        if (!c.streaming) return
        if (c.needKeyFrame && type == Protocol.FRAME_DELTA) return
        if (type == Protocol.FRAME_KEY) c.needKeyFrame = false
        if (!c.queue.offer(Packet(type, ptsUs, data))) {
            // The client is too slow: drop what is queued and resync on the next keyframe.
            c.queue.clear()
            c.needKeyFrame = true
            lastConfig?.let { c.queue.offer(Packet(Protocol.FRAME_CONFIG, ptsUs, it)) }
            controller.requestKeyFrame()
        }
    }

    private fun acceptLoop(ss: ServerSocket) {
        while (!ss.isClosed) {
            val socket = try {
                ss.accept()
            } catch (e: IOException) {
                break
            }
            socket.tcpNoDelay = true
            // The current viewer is only replaced once the newcomer passes the handshake, so a stray
            // connection (another webcam app probing the port, say) can't kick it off.
            val c = Client(socket)
            Thread({ handle(c) }, "stream-client").start()
        }
    }

    private fun handle(c: Client) {
        try {
            c.socket.soTimeout = HANDSHAKE_TIMEOUT_MS
            val input = DataInputStream(c.socket.getInputStream())
            val out = BufferedOutputStream(c.socket.getOutputStream(), 64 * 1024)
            val request = try {
                Protocol.readHandshake(input)
            } catch (e: IOException) {
                out.write(Protocol.handshakeReply(Protocol.STATUS_BAD_REQUEST, 0, 0, 0))
                out.flush()
                throw e
            }
            client?.close()
            client = c
            val actual = controller.configure(request)
            if (actual == null) {
                out.write(Protocol.handshakeReply(Protocol.STATUS_CAMERA_ERROR, 0, 0, 0))
                out.flush()
                return
            }
            out.write(Protocol.handshakeReply(Protocol.STATUS_OK, actual.first, actual.second, actual.third))
            out.flush()
            c.socket.soTimeout = 0

            Thread({ writeLoop(c, out) }, "stream-writer").start()
            lastConfig?.let { c.queue.offer(Packet(Protocol.FRAME_CONFIG, 0, it)) }
            c.streaming = true
            controller.requestKeyFrame()
            controller.onViewersChanged(1)

            while (!c.closed) {
                val control = Protocol.readControl(input)
                if (control.type == Protocol.CTRL_BYE) break
                if (control.type == Protocol.CTRL_REQUEST_KEYFRAME) {
                    c.needKeyFrame = true
                    controller.requestKeyFrame()
                } else {
                    controller.onControl(control)
                }
            }
        } catch (e: IOException) {
            if (e !is SocketException) Log.i(TAG, "Client disconnected: ${e.message}")
        } finally {
            c.close()
            if (client === c) {
                client = null
                controller.onViewersChanged(0)
            }
        }
    }

    private fun writeLoop(c: Client, out: BufferedOutputStream) {
        try {
            while (!c.closed) {
                val p = c.queue.poll(500, TimeUnit.MILLISECONDS) ?: continue
                out.write(Protocol.frameHeader(p.type, p.ptsUs, p.data.size))
                out.write(p.data)
                if (c.queue.isEmpty()) out.flush()
            }
        } catch (_: IOException) {
        } catch (_: InterruptedException) {
        } finally {
            c.close()
        }
    }

    companion object {
        private const val TAG = "StreamServer"
        private const val QUEUE_SIZE = 30
        private const val HANDSHAKE_TIMEOUT_MS = 5000
    }
}
