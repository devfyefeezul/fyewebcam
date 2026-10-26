package com.fyewebcam

import android.content.Context
import android.hardware.camera2.CaptureRequest
import android.media.Image
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import androidx.annotation.OptIn
import androidx.camera.camera2.interop.Camera2Interop
import androidx.camera.camera2.interop.ExperimentalCamera2Interop
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.AspectRatioStrategy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Captures frames with CameraX (ImageAnalysis, YUV_420_888) and encodes them to H.264 with
 * MediaCodec. Encoded buffers are handed to [onEncoded] on the encoder's callback thread.
 */
class CameraEncoder(
    private val context: Context,
    private val lifecycleOwner: LifecycleOwner,
    private val onEncoded: (type: Int, ptsUs: Long, data: ByteArray) -> Unit,
    private val onConfigured: (width: Int, height: Int, fps: Int) -> Unit,
) {
    data class Settings(val width: Int, val height: Int, val fps: Int, val camera: Int)

    private val analysisExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    private val codecThread = HandlerThread("encoder").apply { start() }
    private val codecHandler = Handler(codecThread.looper)
    private val mainExecutor = ContextCompat.getMainExecutor(context)

    private var cameraProvider: ProcessCameraProvider? = null
    private val preview = Preview.Builder().build()

    @Volatile var settings: Settings? = null
        private set

    // Encoder state, owned by the analysis thread.
    private var codec: MediaCodec? = null
    private var codecSize: Size? = null
    private var codecFps = 0
    private var lastPtsUs = 0L
    @Volatile private var restartCodec = false
    private val freeInputs = ConcurrentLinkedQueue<Int>()

    /** Incremented each time a new encoder is configured; lets callers wait for a restart. */
    @Volatile var generation = 0
        private set

    fun setPreviewSurfaceProvider(provider: Preview.SurfaceProvider?) {
        mainExecutor.execute { preview.setSurfaceProvider(provider) }
    }

    /** Starts or restarts the camera with [newSettings]. Safe to call from any thread. */
    fun start(newSettings: Settings) {
        settings = newSettings
        // Always start a fresh encoder so the viewer gets new config data and a keyframe.
        restartCodec = true
        mainExecutor.execute {
            val future = ProcessCameraProvider.getInstance(context)
            future.addListener({
                val provider = future.get()
                cameraProvider = provider
                bind(provider, newSettings)
            }, mainExecutor)
        }
    }

    @OptIn(ExperimentalCamera2Interop::class)
    private fun bind(provider: ProcessCameraProvider, s: Settings) {
        provider.unbindAll()

        val selector = ResolutionSelector.Builder()
            .setAspectRatioStrategy(AspectRatioStrategy.RATIO_16_9_FALLBACK_AUTO_STRATEGY)
            .setResolutionStrategy(
                ResolutionStrategy(Size(s.width, s.height), ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER)
            )
            .build()
        val builder = ImageAnalysis.Builder()
            .setResolutionSelector(selector)
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
        Camera2Interop.Extender(builder)
            .setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(s.fps, s.fps))
        val analysis = builder.build()
        analysis.setAnalyzer(analysisExecutor) { image -> encode(image) }

        val wanted = if (s.camera == Protocol.CAMERA_FRONT) CameraSelector.DEFAULT_FRONT_CAMERA else CameraSelector.DEFAULT_BACK_CAMERA
        val fallback = if (s.camera == Protocol.CAMERA_FRONT) CameraSelector.DEFAULT_BACK_CAMERA else CameraSelector.DEFAULT_FRONT_CAMERA
        val cameraSelector = if (provider.hasCamera(wanted)) wanted else fallback
        try {
            provider.bindToLifecycle(lifecycleOwner, cameraSelector, preview, analysis)
        } catch (e: Exception) {
            Log.e(TAG, "Camera bind failed", e)
        }
    }

    fun stop() {
        mainExecutor.execute { cameraProvider?.unbindAll() }
        analysisExecutor.execute { releaseCodec() }
    }

    fun release() {
        stop()
        analysisExecutor.shutdown()
        codecThread.quitSafely()
    }

    fun requestKeyFrame() {
        codecHandler.post {
            try {
                codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Keyframe request ignored", e)
            }
        }
    }

    private fun encode(image: ImageProxy) {
        image.use {
            val s = settings ?: return
            val size = Size(image.width, image.height)
            if (restartCodec || codec == null || size != codecSize || s.fps != codecFps) {
                restartCodec = false
                createCodec(size, s.fps)
            }
            val ptsUs = image.imageInfo.timestamp / 1000
            // Some cameras ignore the fps range, so drop frames that arrive too early.
            val minGapUs = 1_000_000L / s.fps * 8 / 10
            if (lastPtsUs != 0L && ptsUs - lastPtsUs < minGapUs) return
            val enc = codec ?: return
            val index = freeInputs.poll() ?: return
            try {
                val input = enc.getInputImage(index) ?: return
                copyYuv(image, input)
                enc.queueInputBuffer(index, 0, size.width * size.height * 3 / 2, ptsUs, 0)
                lastPtsUs = ptsUs
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Dropped frame", e)
            }
        }
    }

    private fun createCodec(size: Size, fps: Int) {
        releaseCodec()
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size.width, size.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            setInteger(MediaFormat.KEY_BIT_RATE, (size.width * size.height * fps * 0.12).toInt())
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
        }
        val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        enc.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
                freeInputs.add(index)
            }

            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                val buffer = codec.getOutputBuffer(index)
                if (buffer != null && info.size > 0) {
                    val data = ByteArray(info.size)
                    buffer.position(info.offset)
                    buffer.get(data)
                    val type = when {
                        info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0 -> Protocol.FRAME_CONFIG
                        info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0 -> Protocol.FRAME_KEY
                        else -> Protocol.FRAME_DELTA
                    }
                    onEncoded(type, info.presentationTimeUs, data)
                }
                codec.releaseOutputBuffer(index, false)
            }

            override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                Log.e(TAG, "Encoder error", e)
            }

            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {}
        }, codecHandler)
        enc.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        enc.start()
        codec = enc
        codecSize = size
        codecFps = fps
        lastPtsUs = 0L
        generation++
        onConfigured(size.width, size.height, fps)
    }

    private fun releaseCodec() {
        val enc = codec ?: return
        codec = null
        codecSize = null
        freeInputs.clear()
        try {
            enc.stop()
        } catch (e: IllegalStateException) {
            Log.w(TAG, "Encoder stop failed", e)
        }
        enc.release()
    }

    companion object {
        private const val TAG = "CameraEncoder"

        /** Copies a YUV_420_888 frame into the encoder's input image, respecting both sides' strides. */
        fun copyYuv(src: ImageProxy, dst: Image) {
            val width = src.width
            val height = src.height
            for (plane in 0 until 3) {
                val s = src.planes[plane]
                val d = dst.planes[plane]
                val w = if (plane == 0) width else width / 2
                val h = if (plane == 0) height else height / 2
                val sBuf = s.buffer
                val dBuf = d.buffer
                val sRow = s.rowStride
                val dRow = d.rowStride
                val sPix = s.pixelStride
                val dPix = d.pixelStride
                if (sPix == 1 && dPix == 1) {
                    val row = ByteArray(w)
                    for (y in 0 until h) {
                        sBuf.position(y * sRow)
                        sBuf.get(row, 0, w)
                        dBuf.position(y * dRow)
                        dBuf.put(row, 0, w)
                    }
                } else {
                    for (y in 0 until h) {
                        val sBase = y * sRow
                        val dBase = y * dRow
                        for (x in 0 until w) {
                            dBuf.put(dBase + x * dPix, sBuf.get(sBase + x * sPix))
                        }
                    }
                }
            }
        }
    }
}
