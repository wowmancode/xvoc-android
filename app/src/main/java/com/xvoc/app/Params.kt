package com.xvoc.app

/**
 * Index layout MUST stay in lockstep with the enums in jni_bridge.c
 * (D_* / I_*). If you add a slider, add it in both places in the same
 * position (or append at the end).
 */
object Idx {
    // doubles
    const val FMIN = 0; const val FMAX = 1; const val QMUL = 2; const val QSMUL = 3
    const val ATTACK = 4; const val RELEASE = 5; const val EXPO = 6; const val GATE_DB = 7
    const val TILT = 8; const val BOOST_DB = 9
    const val FORMANT = 10; const val LFO_RATE = 11; const val LFO_DEPTH = 12; const val STRETCH = 13
    const val SMEAR = 14; const val QUANT = 15
    const val PITCH = 16; const val DETUNE = 17; const val PW = 18; const val FM_RATIO = 19
    const val FM_INDEX = 20; const val VIB_RATE = 21; const val VIB_DEPTH = 22
    const val GLIDE = 23; const val NOISE_MIX = 24; const val TRACK_SHIFT = 25
    const val SIB = 26; const val SIB_FREQ = 27
    const val DRY = 28; const val WET = 29; const val CRUSH = 30; const val SRR = 31
    const val DRIVE = 32; const val RING = 33; const val RING_MIX = 34
    const val DELAY_MS = 35; const val DELAY_FB = 36; const val DELAY_MIX = 37
    const val STEREO = 38
    const val D_COUNT = 39

    // ints
    const val BANDS = 0; const val ORDER = 1; const val SPACING = 2; const val WAVE = 3
    const val UNISON = 4; const val REVERSE = 5; const val ROTATE = 6; const val SCRAMBLE = 7
    const val SKIP = 8; const val SMEAR_W = 9; const val TRACK = 10; const val SNAP = 11
    const val BITS = 12
    const val I_COUNT = 13
}

enum class Kind { SLIDER_D, SLIDER_I, BOOL, ENUM }

data class Param(
    val key: String,
    val label: String,
    val kind: Kind,
    val index: Int,           // into the D or I array
    val min: Float = 0f,
    val max: Float = 1f,
    val steps: Int = 0,       // 0 = continuous
    val unit: String = "",
    val options: List<String> = emptyList()
)

data class Section(val title: String, val params: List<Param>)

val WAVE_NAMES = listOf("saw", "square", "pulse", "tri", "sine", "noise", "impulse", "fm", "organ", "super")
val SPACING_NAMES = listOf("log", "lin", "mel", "bark", "rand")
val BIT_OPTIONS = listOf("16", "24")

/** Default double array, matching xvoc's own `defaults()` in xvoc_2_.c. */
fun defaultDoubles(): DoubleArray {
    val d = DoubleArray(Idx.D_COUNT)
    d[Idx.FMIN] = 80.0; d[Idx.FMAX] = 8000.0; d[Idx.QMUL] = 1.0; d[Idx.QSMUL] = 0.0
    d[Idx.ATTACK] = 5.0; d[Idx.RELEASE] = 30.0; d[Idx.EXPO] = 1.0; d[Idx.GATE_DB] = -100.0
    d[Idx.TILT] = 0.0; d[Idx.BOOST_DB] = 0.0
    d[Idx.FORMANT] = 0.0; d[Idx.LFO_RATE] = 0.0; d[Idx.LFO_DEPTH] = 0.0; d[Idx.STRETCH] = 1.0
    d[Idx.SMEAR] = 0.0; d[Idx.QUANT] = 0.0
    d[Idx.PITCH] = 110.0; d[Idx.DETUNE] = 10.0; d[Idx.PW] = 0.3; d[Idx.FM_RATIO] = 2.0
    d[Idx.FM_INDEX] = 3.0; d[Idx.VIB_RATE] = 0.0; d[Idx.VIB_DEPTH] = 0.0
    d[Idx.GLIDE] = 0.0; d[Idx.NOISE_MIX] = 0.0; d[Idx.TRACK_SHIFT] = 0.0
    d[Idx.SIB] = 0.0; d[Idx.SIB_FREQ] = 4500.0
    d[Idx.DRY] = 0.0; d[Idx.WET] = 1.0; d[Idx.CRUSH] = 0.0; d[Idx.SRR] = 0.0
    d[Idx.DRIVE] = 0.0; d[Idx.RING] = 0.0; d[Idx.RING_MIX] = 0.0
    d[Idx.DELAY_MS] = 0.0; d[Idx.DELAY_FB] = 0.0; d[Idx.DELAY_MIX] = 0.0
    d[Idx.STEREO] = 0.0
    return d
}

fun defaultInts(): IntArray {
    val i = IntArray(Idx.I_COUNT)
    i[Idx.BANDS] = 24; i[Idx.ORDER] = 2; i[Idx.SPACING] = 0; i[Idx.WAVE] = 0
    i[Idx.UNISON] = 1; i[Idx.REVERSE] = 0; i[Idx.ROTATE] = 0; i[Idx.SCRAMBLE] = 0
    i[Idx.SKIP] = 1; i[Idx.SMEAR_W] = 1; i[Idx.TRACK] = 0; i[Idx.SNAP] = 0
    i[Idx.BITS] = 16
    return i
}

val SECTIONS: List<Section> = listOf(
    Section("Carrier", listOf(
        Param("wave", "Waveform", Kind.ENUM, Idx.WAVE, options = WAVE_NAMES),
        Param("pitch", "Pitch", Kind.SLIDER_D, Idx.PITCH, 30f, 1000f, unit = "Hz"),
        Param("unison", "Unison voices", Kind.SLIDER_I, Idx.UNISON, 1f, 7f),
        Param("detune", "Detune", Kind.SLIDER_D, Idx.DETUNE, 0f, 50f, unit = "cents"),
        Param("pw", "Pulse width", Kind.SLIDER_D, Idx.PW, 0.05f, 0.95f),
        Param("fm_ratio", "FM ratio", Kind.SLIDER_D, Idx.FM_RATIO, 0.25f, 8f),
        Param("fm_index", "FM index", Kind.SLIDER_D, Idx.FM_INDEX, 0f, 15f),
        Param("vib_rate", "Vibrato rate", Kind.SLIDER_D, Idx.VIB_RATE, 0f, 10f, unit = "Hz"),
        Param("vib_depth", "Vibrato depth", Kind.SLIDER_D, Idx.VIB_DEPTH, 0f, 100f, unit = "cents"),
        Param("glide", "Glide", Kind.SLIDER_D, Idx.GLIDE, 0f, 300f, unit = "ms"),
        Param("noise_mix", "Noise mix", Kind.SLIDER_D, Idx.NOISE_MIX, 0f, 1f),
        Param("track", "Pitch-track carrier", Kind.BOOL, Idx.TRACK),
        Param("snap", "Snap to semitone", Kind.BOOL, Idx.SNAP),
        Param("track_shift", "Track shift", Kind.SLIDER_D, Idx.TRACK_SHIFT, -24f, 24f, unit = "st"),
    )),
    Section("Filter bank", listOf(
        Param("bands", "Bands", Kind.SLIDER_I, Idx.BANDS, 4f, 64f),
        Param("order", "Filter order", Kind.SLIDER_I, Idx.ORDER, 1f, 4f),
        Param("spacing", "Band spacing", Kind.ENUM, Idx.SPACING, options = SPACING_NAMES),
        Param("fmin", "Min frequency", Kind.SLIDER_D, Idx.FMIN, 20f, 2000f, unit = "Hz"),
        Param("fmax", "Max frequency", Kind.SLIDER_D, Idx.FMAX, 1000f, 20000f, unit = "Hz"),
        Param("qmul", "Q multiplier", Kind.SLIDER_D, Idx.QMUL, 0.2f, 4f),
        Param("qsmul", "Synth-side Q mult", Kind.SLIDER_D, Idx.QSMUL, 0f, 4f),
    )),
    Section("Envelope", listOf(
        Param("attack", "Attack", Kind.SLIDER_D, Idx.ATTACK, 0.3f, 60f, unit = "ms"),
        Param("release", "Release", Kind.SLIDER_D, Idx.RELEASE, 2f, 400f, unit = "ms"),
        Param("expo", "Envelope curve", Kind.SLIDER_D, Idx.EXPO, 0.3f, 2.5f),
        Param("gate_db", "Gate threshold", Kind.SLIDER_D, Idx.GATE_DB, -120f, -10f, unit = "dB"),
        Param("tilt", "Spectral tilt", Kind.SLIDER_D, Idx.TILT, -8f, 8f, unit = "dB/oct"),
        Param("boost_db", "Boost", Kind.SLIDER_D, Idx.BOOST_DB, -12f, 12f, unit = "dB"),
    )),
    Section("Band mapping", listOf(
        Param("formant", "Formant shift", Kind.SLIDER_D, Idx.FORMANT, -24f, 24f, unit = "st"),
        Param("lfo_rate", "Formant LFO rate", Kind.SLIDER_D, Idx.LFO_RATE, 0f, 8f, unit = "Hz"),
        Param("lfo_depth", "Formant LFO depth", Kind.SLIDER_D, Idx.LFO_DEPTH, 0f, 12f, unit = "st"),
        Param("stretch", "Band stretch", Kind.SLIDER_D, Idx.STRETCH, 0.4f, 2.2f),
        Param("reverse", "Reverse bands", Kind.BOOL, Idx.REVERSE),
        Param("rotate", "Rotate bands", Kind.SLIDER_I, Idx.ROTATE, 0f, 32f),
        Param("scramble", "Scramble bands", Kind.BOOL, Idx.SCRAMBLE),
        Param("skip", "Skip every Nth", Kind.SLIDER_I, Idx.SKIP, 1f, 4f),
        Param("smear", "Smear", Kind.SLIDER_D, Idx.SMEAR, 0f, 0.95f),
        Param("smear_w", "Smear width", Kind.SLIDER_I, Idx.SMEAR_W, 1f, 4f),
        Param("quant", "Envelope quantize", Kind.SLIDER_D, Idx.QUANT, 0f, 10f, unit = "dB"),
    )),
    Section("Sibilance", listOf(
        Param("sib", "Amount", Kind.SLIDER_D, Idx.SIB, 0f, 3f),
        Param("sib_freq", "Frequency", Kind.SLIDER_D, Idx.SIB_FREQ, 1000f, 10000f, unit = "Hz"),
    )),
    Section("Mix & post", listOf(
        Param("dry", "Dry", Kind.SLIDER_D, Idx.DRY, 0f, 1f),
        Param("wet", "Wet", Kind.SLIDER_D, Idx.WET, 0f, 1f),
        Param("crush", "Bit crush", Kind.SLIDER_D, Idx.CRUSH, 0f, 16f, unit = "bit"),
        Param("srr", "Sample-rate reduce", Kind.SLIDER_D, Idx.SRR, 0f, 20f, unit = "x"),
        Param("drive", "Drive", Kind.SLIDER_D, Idx.DRIVE, 0f, 24f, unit = "dB"),
        Param("ring", "Ring mod freq", Kind.SLIDER_D, Idx.RING, 0f, 2000f, unit = "Hz"),
        Param("ring_mix", "Ring mod mix", Kind.SLIDER_D, Idx.RING_MIX, 0f, 1f),
        Param("delay_ms", "Delay time", Kind.SLIDER_D, Idx.DELAY_MS, 0f, 1000f, unit = "ms"),
        Param("delay_fb", "Delay feedback", Kind.SLIDER_D, Idx.DELAY_FB, 0f, 0.95f),
        Param("delay_mix", "Delay mix", Kind.SLIDER_D, Idx.DELAY_MIX, 0f, 1f),
        Param("stereo", "Stereo spread", Kind.SLIDER_D, Idx.STEREO, 0f, 1f),
    )),
    Section("Output", listOf(
        Param("bits", "Bit depth", Kind.ENUM, Idx.BITS, options = BIT_OPTIONS),
    )),
)
