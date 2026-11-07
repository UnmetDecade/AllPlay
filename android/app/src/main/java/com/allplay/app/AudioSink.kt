package com.allplay.app

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import java.nio.ByteBuffer

/**
 * IAudioOutput 的 Android 实现：解码 CarPlay 音频（Opus / AAC-LC / LPCM）→ AudioTrack。
 * 由 core 的 AudioStream 线程经 JNI 下调 configure/feed/reset。
 */
class AudioSink {
    private var codec: MediaCodec? = null
    private var track: AudioTrack? = null
    private var mime = MediaFormat.MIMETYPE_AUDIO_OPUS
    private var sampleRate = 48000
    private var channels = 1
    private var lpcm = false
    private var firstAacLogged = false
    private var firstPcmLogged = false
    private var firstWriteLogged = false
    private var firstFeedLogged = false
    private var firstDrainLogged = false

    /// codec: 0=AAC-LC, 1=Opus, 2=LPCM
    fun configure(codecType: Int, sampleRate: Int, channels: Int) {
        this.sampleRate = sampleRate
        this.channels = channels
        release()
        lpcm = codecType == 2
        if (lpcm) {
            startTrack()
            Log.i("AllPlay", "AudioSink LPCM ${sampleRate}Hz ${channels}ch")
            return
        }
        mime = if (codecType == 0) MediaFormat.MIMETYPE_AUDIO_AAC else MediaFormat.MIMETYPE_AUDIO_OPUS
        try {
            codec = MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            Log.w("AllPlay", "AudioSink 解码器创建失败 $mime: ${e.message}")
            codec = null
            return
        }
        val format = MediaFormat.createAudioFormat(mime, sampleRate, channels)
        if (mime == MediaFormat.MIMETYPE_AUDIO_AAC) {
            // RTP 里的 AAC 是裸帧，MediaCodec 用 ADTS 头识别；csd-0 = AudioSpecificConfig（协议要求两者都设）
            format.setInteger(MediaFormat.KEY_IS_ADTS, 1)
            format.setByteBuffer("csd-0", ByteBuffer.wrap(aacAudioSpecificConfig(sampleRate, channels)))
        }
        codec!!.configure(format, null, null, 0)
        codec!!.start()
        startTrack()
        Log.i("AllPlay", "AudioSink 配置 $mime ${sampleRate}Hz ${channels}ch")
    }

    fun feed(payload: ByteArray, sample: Int) {
        if (lpcm) {
            // AirPlay 的 LPCM 是**大端** s16（网络序），AudioTrack 按小端播 —— 逐样本换字节。
            // 不换的话每个样本的高低字节颠倒，听感是满耳噪音（macOS 宿主上已实测确认）。
            val n = payload.size and 1.inv()
            var i = 0
            while (i < n) {
                val b = payload[i]
                payload[i] = payload[i + 1]
                payload[i + 1] = b
                i += 2
            }
            track?.write(payload, 0, n)
            return
        }
        // 小包（config 包）在抢 input buffer 之前就跳过，否则会 claim buffer 又不 queue 导致泄漏
        if (mime == MediaFormat.MIMETYPE_AUDIO_AAC && payload.size <= 12) return
        val c = codec ?: return
        val idx = c.dequeueInputBuffer(100_000)   // 100ms；拿不到就丢这一帧，别让 10s 超时卡死整条音频线程
        if (!firstFeedLogged) {
            firstFeedLogged = true
            Log.i("AllPlay", "dequeueInputBuffer 返回=$idx")
        }
        if (idx < 0) return
        val buf = c.compatInputBuffer(idx) ?: return
        buf.clear()
        val data = if (mime == MediaFormat.MIMETYPE_AUDIO_AAC) {
            if (!firstAacLogged) {
                firstAacLogged = true
                val h = payload.take(8).joinToString("") { "%02x".format(it) }
                Log.i("AllPlay", "AAC 帧 head=$h size=${payload.size}")
            }
            adtsWrap(payload)
        } else payload
        buf.put(data)
        c.queueInputBuffer(idx, 0, data.size, 0, 0)
        drain()
    }

    fun reset() { release() }

    private fun drain() {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            // 非阻塞：10s 超时会卡死整条音频线程（解码器没吐帧时 UDP 收包停滞→丢包→卡顿）
            val out = c.dequeueOutputBuffer(info, 0)
            if (!firstDrainLogged) {
                firstDrainLogged = true
                Log.i("AllPlay", "dequeueOutputBuffer 返回=$out")
            }
            when {
                out >= 0 -> {
                    val buf = c.compatOutputBuffer(out)
                    if (buf != null && info.size > 0) {
                        val pcm = ByteArray(info.size)
                        buf.position(info.offset)
                        buf.get(pcm, 0, info.size)
                        if (!firstPcmLogged) {
                            firstPcmLogged = true
                            Log.i("AllPlay", "AAC 解码出 PCM ${pcm.size} 字节")
                        }
                        val w = track?.write(pcm, 0, pcm.size) ?: -1
                        if (!firstWriteLogged) {
                            firstWriteLogged = true
                            Log.i("AllPlay", "AudioTrack.write 返回=$w（期望 ${pcm.size}）")
                        }
                    }
                    c.releaseOutputBuffer(out, false)
                }
                out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    Log.i("AllPlay", "音频解码输出格式: ${c.outputFormat}")
                }
                out == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                else -> Unit
            }
        }
    }

    private fun startTrack() {
        val ch = if (channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val min = AudioTrack.getMinBufferSize(sampleRate, ch, AudioFormat.ENCODING_PCM_16BIT)
        // 缓冲放大到 4×min，吸收 Wi-Fi 抖动的网络抖动，避免解码快但播放缓冲耗尽造成的卡顿
        track = if (Build.VERSION.SDK_INT >= 21) {
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()
            val format = AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setChannelMask(ch)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .build()
            AudioTrack(attrs, format, min * 4, AudioTrack.MODE_STREAM,
                AudioManager.AUDIO_SESSION_ID_GENERATE)
        } else {
            // Android 4.4：无 AudioAttributes/AudioFormat.Builder，走旧 5 参构造
            @Suppress("DEPRECATION")
            AudioTrack(AudioManager.STREAM_MUSIC, sampleRate, ch,
                AudioFormat.ENCODING_PCM_16BIT, min * 4, AudioTrack.MODE_STREAM)
        }
        track?.setVolume(1.0f)
        Log.i("AllPlay", "AudioTrack 创建 state=${track?.state} ch=$ch rate=$sampleRate")
        track?.play()
        Log.i("AllPlay", "AudioTrack play 后 playState=${track?.playState}")
    }

    /// AudioSpecificConfig（2 字节）：AAC-LC + 采样率索引 + 声道（参照协议 AAC AudioSpecificConfig）
    private fun aacAudioSpecificConfig(sampleRate: Int, channels: Int): ByteArray {
        val freqIndex = when (sampleRate) {
            8000 -> 11; 16000 -> 8; 32000 -> 5; 44100 -> 4; 48000 -> 3
            else -> 3
        }
        val value = (2 shl 11) or (freqIndex shl 7) or (channels.coerceIn(1, 7) shl 3)
        return byteArrayOf((value ushr 8).toByte(), value.toByte())
    }

    /// 裸 AAC 帧 → 加 7 字节 ADTS 头（MediaCodec 需要）
    private fun adtsWrap(aac: ByteArray): ByteArray {
        val freqIndex = when (sampleRate) {
            8000 -> 11; 16000 -> 8; 32000 -> 5; 44100 -> 4; 48000 -> 3
            else -> 3
        }
        val ch = if (channels == 2) 2 else 1
        val frameLen = aac.size + 7
        val h = ByteArray(7)
        h[0] = 0xFF.toByte()
        h[1] = 0xF1.toByte()                                        // MPEG-4, 无 CRC, AAC-LC
        h[2] = (((1 shl 6) or (freqIndex shl 2) or (ch shr 2))).toByte()
        h[3] = (((ch shl 6) or (frameLen shr 11))).toByte()
        h[4] = ((frameLen shr 3) and 0xFF).toByte()
        h[5] = (((frameLen and 0x7) shl 5) or 0x1F).toByte()
        h[6] = 0xFC.toByte()
        return h + aac
    }

    private fun release() {
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
        try { track?.stop() } catch (_: Exception) {}
        try { track?.release() } catch (_: Exception) {}
        track = null
    }
}
