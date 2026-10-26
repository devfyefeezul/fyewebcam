package com.fyewebcam

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.DataInputStream
import java.io.IOException

class ProtocolTest {

    @Test
    fun parsesHandshake() {
        val hello = byteArrayOf(
            'P'.code.toByte(), 'W'.code.toByte(), 'C'.code.toByte(), 'M'.code.toByte(),
            1, 0x00, 0x05, 0xD0.toByte(), 0x02, 30, 1,
        )
        val req = Protocol.readHandshake(ByteArrayInputStream(hello))
        assertEquals(Protocol.StreamRequest(1280, 720, 30, Protocol.CAMERA_FRONT), req)
    }

    @Test(expected = IOException::class)
    fun rejectsBadMagic() {
        Protocol.parseHandshake(ByteArray(Protocol.HANDSHAKE_SIZE))
    }

    @Test
    fun buildsReplyAndFrameHeader() {
        val reply = Protocol.handshakeReply(Protocol.STATUS_OK, 1920, 1080, 30)
        assertArrayEquals(
            byteArrayOf(0x50, 0x57, 0x43, 0x4D, 0, 0x80.toByte(), 0x07, 0x38, 0x04, 30, 1),
            reply,
        )
        val header = Protocol.frameHeader(Protocol.FRAME_KEY, 0x0102, 5)
        assertArrayEquals(byteArrayOf(2, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0), header)
    }

    @Test
    fun readsControlMessages() {
        val bytes = byteArrayOf(0x11, 5, 0, 0x00, 0x05, 0xD0.toByte(), 0x02, 30)
        val control = Protocol.readControl(DataInputStream(ByteArrayInputStream(bytes)))
        assertEquals(Protocol.CTRL_SET_RESOLUTION, control.type)
        assertEquals(Triple(1280, 720, 30), Protocol.parseResolution(control.payload))
    }
}
