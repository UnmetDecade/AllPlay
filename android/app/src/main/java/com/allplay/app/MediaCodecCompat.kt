package com.allplay.app

import android.media.MediaCodec
import android.os.Build
import java.nio.ByteBuffer

/**
 * API 19 compatibility layer for MediaCodec input/output buffer access.
 * API 21+ uses getInputBuffer(idx)/getOutputBuffer(idx); Android 4.4 (API 19) only has
 * the array forms getInputBuffers()/getOutputBuffers() (deprecated but still usable).
 */
internal fun MediaCodec.compatInputBuffer(idx: Int): ByteBuffer? =
    if (Build.VERSION.SDK_INT >= 21) getInputBuffer(idx)
    else @Suppress("DEPRECATION") getInputBuffers()[idx]

internal fun MediaCodec.compatOutputBuffer(idx: Int): ByteBuffer? =
    if (Build.VERSION.SDK_INT >= 21) getOutputBuffer(idx)
    else @Suppress("DEPRECATION") getOutputBuffers()[idx]
