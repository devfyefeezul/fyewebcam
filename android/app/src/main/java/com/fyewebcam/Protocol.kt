package com.fyewebcam

import java.io.DataInputStream
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Wire format shared with the Windows client. All integers are little-endian.
 * See docs/PROTOCOL.md for the full description.
 */
object Protocol {
    const val DEFAULT_PORT = 4747
    const val VERSION: Int = 1
    val MAGIC = byteArrayOf('P'.code.toByte(), 'W'.code.toByte(), 'C'.code.toByte(), 'M'.code.toByte())

    const val HANDSHAKE_SIZE = 11
    const val FRAME_HEADER_SIZE = 13

    const val STATUS_OK = 0
    const val STATUS_BAD_REQUEST = 1
    const val STATUS_CAMERA_ERROR = 2

    const val CODEC_H264 = 1

    const val CAMERA_BACK = 0
    const val CAMERA_FRONT = 1
    /** Handshake only: keep the camera picked on the phone. */
    const val CAMERA_KEEP = 0xFF

    const val FRAME_CONFIG = 1
    const val FRAME_KEY = 2
    const val FRAME_DELTA = 3

    const val CTRL_SWITCH_CAMERA = 0x10
    const val CTRL_SET_RESOLUTION = 0x11
    const val CTRL_REQUEST_KEYFRAME = 0x12
    const val CTRL_BYE = 0x1F

    data class StreamRequest(val width: Int, val height: Int, val fps: Int, val camera: Int)

    data class Control(val type: Int, val payload: ByteArray)

    /** Reads the 11-byte client hello. Throws [IOException] if the magic or version is wrong. */
    fun readHandshake(input: InputStream): StreamRequest {
        val buf = ByteArray(HANDSHAKE_SIZE)
        DataInputStream(input).readFully(buf)
        return parseHandshake(buf)
    }

    fun parseHandshake(buf: ByteArray): StreamRequest {
        require(buf.size >= HANDSHAKE_SIZE) { "handshake too short" }
        if (!buf.copyOfRange(0, 4).contentEquals(MAGIC)) throw IOException("bad magic")
        val bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN)
        val version = bb.get(4).toInt() and 0xFF
        if (version != VERSION) throw IOException("unsupported version $version")
        return StreamRequest(
            width = bb.getShort(5).toInt() and 0xFFFF,
            height = bb.getShort(7).toInt() and 0xFFFF,
            fps = bb.get(9).toInt() and 0xFF,
            camera = bb.get(10).toInt() and 0xFF,
        )
    }

    fun handshakeReply(status: Int, width: Int, height: Int, fps: Int): ByteArray {
        val bb = ByteBuffer.allocate(HANDSHAKE_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        bb.put(MAGIC)
        bb.put(status.toByte())
        bb.putShort(width.toShort())
        bb.putShort(height.toShort())
        bb.put(fps.toByte())
        bb.put(CODEC_H264.toByte())
        return bb.array()
    }

    fun frameHeader(type: Int, ptsUs: Long, length: Int): ByteArray {
        val bb = ByteBuffer.allocate(FRAME_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        bb.put(type.toByte())
        bb.putLong(ptsUs)
        bb.putInt(length)
        return bb.array()
    }

    /** Reads one control message (type:u8 | length:u16 | payload). */
    fun readControl(input: DataInputStream): Control {
        val type = input.readUnsignedByte()
        val lo = input.readUnsignedByte()
        val hi = input.readUnsignedByte()
        val payload = ByteArray(lo or (hi shl 8))
        input.readFully(payload)
        return Control(type, payload)
    }

    /** Parses a [CTRL_SET_RESOLUTION] payload: width:u16 | height:u16 | fps:u8. */
    fun parseResolution(payload: ByteArray): Triple<Int, Int, Int>? {
        if (payload.size < 5) return null
        val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        return Triple(bb.getShort(0).toInt() and 0xFFFF, bb.getShort(2).toInt() and 0xFFFF, bb.get(4).toInt() and 0xFF)
    }
}
