package com.xvoc.app

import android.content.Context
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * xvoc's native engine only reads/writes WAV. To let people import "any"
 * audio file (mp3, m4a, ogg, flac, ...), we decode it with Android's own
 * MediaExtractor/MediaCodec into 16-bit PCM and wrap that as a WAV file
 * in the app's cache dir before handing it to the native engine.
 */
object AudioIO {

    /** Decodes the audio at [uri] to a mono or stereo 16-bit PCM WAV file in cache. */
    fun importToWav(context: Context, uri: Uri, outFile: File): Boolean {
        val pfd = context.contentResolver.openFileDescriptor(uri, "r") ?: return false
        pfd.use { pf ->
            val extractor = MediaExtractor()
            extractor.setDataSource(pf.fileDescriptor)

            var trackIndex = -1
            var format: MediaFormat? = null
            for (i in 0 until extractor.trackCount) {
                val f = extractor.getTrackFormat(i)
                val mime = f.getString(MediaFormat.KEY_MIME) ?: continue
                if (mime.startsWith("audio/")) { trackIndex = i; format = f; break }
            }
            if (trackIndex < 0 || format == null) return false
            extractor.selectTrack(trackIndex)

            val mime = format.getString(MediaFormat.KEY_MIME)!!
            val sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            val channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)

            val codec = MediaCodec.createDecoderByType(mime)
            codec.configure(format, null, null, 0)
            codec.start()

            val pcm = ByteArrayOutputStream()
            val bufferInfo = MediaCodec.BufferInfo()
            var sawInputEOS = false
            var sawOutputEOS = false

            while (!sawOutputEOS) {
                if (!sawInputEOS) {
                    val inIndex = codec.dequeueInputBuffer(10_000)
                    if (inIndex >= 0) {
                        val inBuf = codec.getInputBuffer(inIndex)!!
                        val sampleSize = extractor.readSampleData(inBuf, 0)
                        if (sampleSize < 0) {
                            codec.queueInputBuffer(inIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            sawInputEOS = true
                        } else {
                            codec.queueInputBuffer(inIndex, 0, sampleSize, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                var outIndex = codec.dequeueOutputBuffer(bufferInfo, 10_000)
                while (outIndex >= 0) {
                    if (bufferInfo.size > 0) {
                        val outBuf = codec.getOutputBuffer(outIndex)!!
                        outBuf.position(bufferInfo.offset)
                        outBuf.limit(bufferInfo.offset + bufferInfo.size)
                        val chunk = ByteArray(bufferInfo.size)
                        outBuf.get(chunk)
                        pcm.write(chunk)
                    }
                    codec.releaseOutputBuffer(outIndex, false)
                    if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        sawOutputEOS = true
                        break
                    }
                    outIndex = codec.dequeueOutputBuffer(bufferInfo, 0)
                }
            }
            codec.stop()
            codec.release()
            extractor.release()

            writeWavPcm16(outFile, pcm.toByteArray(), sampleRate, channels)
        }
        return true
    }

    /** Writes raw little-endian 16-bit PCM bytes as a standard WAV file. */
    private fun writeWavPcm16(file: File, pcm: ByteArray, sampleRate: Int, channels: Int) {
        val bitsPerSample = 16
        val byteRate = sampleRate * channels * bitsPerSample / 8
        val blockAlign = channels * bitsPerSample / 8
        val dataLen = pcm.size
        val header = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
        header.put("RIFF".toByteArray())
        header.putInt(36 + dataLen)
        header.put("WAVE".toByteArray())
        header.put("fmt ".toByteArray())
        header.putInt(16)
        header.putShort(1) // PCM
        header.putShort(channels.toShort())
        header.putInt(sampleRate)
        header.putInt(byteRate)
        header.putShort(blockAlign.toShort())
        header.putShort(bitsPerSample.toShort())
        header.put("data".toByteArray())
        header.putInt(dataLen)

        FileOutputStream(file).use { fos ->
            fos.write(header.array())
            fos.write(pcm)
        }
    }

    /** Copies [srcWav] (produced by the native engine) to a user-picked SAF destination. */
    fun exportTo(context: Context, srcWav: File, destUri: Uri): Boolean {
        return try {
            context.contentResolver.openOutputStream(destUri)?.use { out ->
                srcWav.inputStream().use { it.copyTo(out) }
            }
            true
        } catch (e: Exception) {
            false
        }
    }
}
