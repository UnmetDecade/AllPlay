package com.allplay.app

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Android implementation of IVideoOutput: MediaCodec hardware decode → Surface.
 * The core screen-stream thread downcalls configure/feedAnnexB/reset via JNI.
 * Follows the protocol (AndroidMediaSink behavior): H264 csd uses SPS/PPS split out of avcC, HEVC uses hvcC.
 */
class MediaCodecSink {
    private var codec: MediaCodec? = null
    private var mime = MediaFormat.MIMETYPE_VIDEO_AVC
    private var width = 1280
    private var height = 720
    private var surface: Surface? = null

    // Stash the config while the surface is not ready; apply it after setSurface
    private var pendingCodecType = 0
    private var pendingCodecData: ByteArray? = null
    // Last successful config, used to rebuild the decoder when the Surface changes
    private var lastCodecType = 0
    private var lastCodecData: ByteArray? = null
    // configure failure backoff: avoid frequent retries while the Surface is flapping; wait for the system to settle
    private var configureFailUntil = 0L

    /// codec: 0=H264, 1=HEVC; codecData = ISO BMFF avcC/hvcC record (without the fourcc)
    fun configure(codecType: Int, width: Int, height: Int, codecData: ByteArray) {
        val now = System.currentTimeMillis()
        // Backoff window: record only the latest config and rebuild for real after it ends
        if (now < configureFailUntil) {
            this.width = width; this.height = height
            lastCodecType = codecType; lastCodecData = codecData
            pendingCodecType = codecType; pendingCodecData = codecData
            return
        }
        // The iPhone re-sends the same avcC in response to forceKeyFrame; if the config is unchanged, do not rebuild the decoder, avoiding periodic black frames/stutter
        if (codec != null && lastCodecType == codecType && this.width == width && this.height == height
            && lastCodecData != null && lastCodecData!!.contentEquals(codecData)) {
            Log.i("AllPlay", "MediaCodec config unchanged, skipping rebuild")
            return
        }
        this.width = width
        this.height = height
        this.mime = if (codecType == 1) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        lastCodecType = codecType
        lastCodecData = codecData
        release()
        val s = surface
        if (s == null) {
            pendingCodecType = codecType
            pendingCodecData = codecData
            return
        }
        try {
            this.codec = MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            this.codec = null
            return
        }
        val format = MediaFormat.createVideoFormat(mime, width, height)
        val csd = buildCsd(codecData)
        csd.forEachIndexed { i, bytes -> format.setByteBuffer("csd-${i}", ByteBuffer.wrap(bytes)) }
        try {
            this.codec!!.configure(format, s, null, 0)
            this.codec!!.start()
        } catch (e: Exception) {
            // When backgrounding/rotating, the Surface may already be released and configure/start fail;
            // do not let the exception cross JNI and crash the native thread. Back off 1s, retry, and stash the config for the new Surface
            Log.w("AllPlay", "MediaCodec configure failed (Surface may be released): ${e.message}")
            configureFailUntil = System.currentTimeMillis() + 1000
            this.codec = null
            pendingCodecType = codecType
            pendingCodecData = codecData
            return
        }
        Log.i("AllPlay", "MediaCodec configured and started mime=$mime ${width}x${height} csd=${csd.size} surface=${s != null}")
    }

    /// Called when the Surface is destroyed on backgrounding: drop the reference to the released Surface to avoid a later configure crash
    fun releaseSurface() {
        Log.i("AllPlay", "MediaCodec releaseSurface")
        surface = null
        release()
    }

    fun setSurface(s: Surface) {
        surface = s
        Log.i("AllPlay", "MediaCodec setSurface surface=${s != null}")
        // Late surface: apply the config that was stashed earlier because the surface was not ready
        val d = pendingCodecData
        if (d != null) {
            pendingCodecData = null
            configure(pendingCodecType, width, height, d)
            return
        }
        // A decoder exists but the Surface changed (background→foreground rebuild): prefer hot-swapping via setOutputSurface,
        // falling back to rebuilding from the last config (as in setOutputSurface → reconfigure)
        val c = codec
        if (c != null) {
            // setOutputSurface is API 23; on 4.4, rebuild the decoder directly from the last config
            if (Build.VERSION.SDK_INT >= 23) {
                try {
                    c.setOutputSurface(s)
                } catch (e: Exception) {
                    lastCodecData?.let { configure(lastCodecType, width, height, it) }
                }
            } else {
                lastCodecData?.let { configure(lastCodecType, width, height, it) }
            }
        } else {
            // The decoder was released when the Surface was destroyed on backgrounding; rebuild it from the last config when the Surface returns
            lastCodecData?.let { configure(lastCodecType, width, height, it) }
        }
    }

    fun feedAnnexB(data: ByteArray): Boolean {
        val c = codec ?: return false
        try {
            // VideoInputPump: drain output while waiting for an input buffer (mirroring the VideoInputPump mode).
            // If the output buffers fill up, input starves → dequeueInputBuffer never succeeds → silent frame drops → broken reference chain and artifacts.
            // So repeatedly drain output within a 500ms window to free up input, rather than dropping this frame outright.
            var idx = -1
            val deadline = System.nanoTime() + 500_000_000L
            while (idx < 0 && System.nanoTime() < deadline) {
                idx = c.dequeueInputBuffer(0)
                if (idx >= 0) break
                drain()
            }
            if (idx < 0) return false
            val buf = c.compatInputBuffer(idx) ?: return false
            buf.clear()
            if (data.size > buf.remaining()) return false   // oversized frame: the reference chain is broken; let the upper layer gate playback + reconfigure
            buf.put(data)
            // Timestamps must increase monotonically, otherwise the decoder renders out of order (garbage). The protocol uses System.nanoTime()/1000
            c.queueInputBuffer(idx, 0, data.size, System.nanoTime() / 1000, 0)
            drain()
            return true
        } catch (e: Exception) {
            // Surface destruction on backgrounding puts MediaCodec into an illegal state and dequeueInput/Output throws IllegalStateException;
            // letting the exception cross JNI would crash the native screen-stream thread → drop all of CarPlay. Swallow it, release the decoder, return false;
            // core then gates playback until a keyframe, and after the Surface returns setSurface rebuilds from the last config.
            Log.w("AllPlay", "Video feed error, releasing decoder and waiting for Surface rebuild: ${e.message}")
            release()
            return false
        }
    }

    fun reset() {
        release()
    }

    fun flush() {
        try { codec?.flush() } catch (_: Exception) {}
    }

    private var decodedFrames = 0
    @Volatile var renderedFrames = 0
        private set

    private fun drain() {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            val out = c.dequeueOutputBuffer(info, 0)
            when {
                out >= 0 -> {
                    // The Surface may already be destroyed in the background, so render=true can throw; on failure degrade to not rendering
                    // to keep the exception from crossing JNI and killing the native thread (one cause of losing all of CarPlay after backgrounding)
                    var rendered = false
                    try {
                        c.releaseOutputBuffer(out, true)   // hand to the Surface for composition
                        rendered = true
                    } catch (e: Exception) {
                        try { c.releaseOutputBuffer(out, false) } catch (_: Exception) {}
                    }
                    if (rendered) renderedFrames++
                    decodedFrames++
                    if (decodedFrames == 1 || decodedFrames % 300 == 0)
                        Log.i("AllPlay", "MediaCodec output frame #$decodedFrames size=${info.size}")
                }
                out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED ->
                    Log.i("AllPlay", "MediaCodec output format changed ${c.outputFormat}")
                out == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                else -> Unit
            }
            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return
        }
    }

    private fun release() {
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
    }

    // avcC/hvcC → MediaCodec csd-0/csd-1 (AnnexB, with start codes). Per the protocol's MediaCodecSupport.
    private fun buildCsd(codecData: ByteArray): List<ByteArray> {
        if (mime == MediaFormat.MIMETYPE_VIDEO_AVC) {
            // avcC: SPS count is [5]&0x1f; from offset 6, u16BE length + SPS; then PPS
            if (codecData.size < 7) return emptyList()
            val spsCount = codecData[5].toInt() and 0x1f
            var off = 6
            val spsList = ArrayList<ByteArray>()
            repeat(spsCount) {
                if (off + 2 > codecData.size) return@repeat
                val len = ((codecData[off].toInt() and 0xff) shl 8) or (codecData[off + 1].toInt() and 0xff)
                off += 2
                spsList.add(codecData.copyOfRange(off, off + len))
                off += len
            }
            if (off >= codecData.size) return emptyList()
            val ppsCount = codecData[off++].toInt() and 0xff
            val ppsList = ArrayList<ByteArray>()
            repeat(ppsCount) {
                if (off + 2 > codecData.size) return@repeat
                val len = ((codecData[off].toInt() and 0xff) shl 8) or (codecData[off + 1].toInt() and 0xff)
                off += 2
                ppsList.add(codecData.copyOfRange(off, off + len))
                off += len
            }
            val start = byteArrayOf(0, 0, 0, 1)
            return listOfNotNull(
                spsList.firstOrNull()?.let { start + it },
                ppsList.firstOrNull()?.let { start + it },
            )
        } else {
            // HEVC hvcC: separate VPS/SPS/PPS arrays (array count at offset 22). The first version takes only the first NAL set.
            return listOf(byteArrayOf(0, 0, 0, 1) + codecData)   // TODO(on-device): split VPS/SPS/PPS per array from hvcC
        }
    }
}
