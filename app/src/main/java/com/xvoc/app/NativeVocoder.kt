package com.xvoc.app

/**
 * Thin Kotlin façade over jni_bridge.c, which itself wraps the untouched
 * xvoc DSP engine (xvoc_2_.c). All three functions here are blocking /
 * synchronous native calls — call them from a background thread.
 */
object NativeVocoder {
    init {
        System.loadLibrary("xvocjni")
    }

    /**
     * Renders [inPath] (a WAV file) through the vocoder with the given
     * params and writes the result to [outPath] (also WAV).
     * @return 0 on success, non-zero on failure.
     */
    external fun process(
        inPath: String,
        outPath: String,
        d: DoubleArray,
        i: IntArray
    ): Int

    /**
     * Fills [d]/[i] (must be pre-sized to Params.D_COUNT / Params.I_COUNT)
     * with a randomized patch, same algorithm as the CLI's --randomize.
     */
    external fun randomize(seed: Long, d: DoubleArray, i: IntArray): Int

    /** Returns [frameCount, sampleRate] for a WAV file, or null if unreadable. */
    external fun probe(path: String): LongArray?
}
