/*
 * jni_bridge.c
 *
 * Thin JNI wrapper around the original xvoc.c vocoder engine. The engine
 * file is included as-is (unmodified DSP code) and compiled into this same
 * translation unit, so its `static` helpers (defaults, read_wav, write_wav,
 * run_vocoder, randomize_cfg, ...) are directly callable here.
 *
 * We don't touch xvoc's CLI (argv parsing, --mutate batch, etc). We just
 * populate a Cfg struct straight from values handed over by Kotlin, call
 * read_wav() / run_vocoder() the same way main() does, and report back.
 *
 * xvoc's own file is never modified: it still defines main(), which simply
 * becomes an unused symbol inside this shared library. That's harmless.
 */

#include <jni.h>
#include <android/log.h>
#include <string.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xvoc-jni", __VA_ARGS__)

#include "xvoc_2_.c"

/* ---- index layout shared (by convention) with Kotlin ParamIndex ------- */
/* Doubles, 39 values */
enum {
    D_FMIN, D_FMAX, D_QMUL, D_QSMUL,
    D_ATTACK, D_RELEASE, D_EXPO, D_GATE_DB, D_TILT, D_BOOST_DB,
    D_FORMANT, D_LFO_RATE, D_LFO_DEPTH, D_STRETCH, D_SMEAR, D_QUANT,
    D_PITCH, D_DETUNE, D_PW, D_FM_RATIO, D_FM_INDEX, D_VIB_RATE, D_VIB_DEPTH,
    D_GLIDE, D_NOISE_MIX, D_TRACK_SHIFT,
    D_SIB, D_SIB_FREQ,
    D_DRY, D_WET, D_CRUSH, D_SRR, D_DRIVE, D_RING, D_RING_MIX,
    D_DELAY_MS, D_DELAY_FB, D_DELAY_MIX, D_STEREO,
    D_COUNT
};
/* Ints, 13 values */
enum {
    I_BANDS, I_ORDER, I_SPACING, I_WAVE, I_UNISON, I_REVERSE, I_ROTATE,
    I_SCRAMBLE, I_SKIP, I_SMEAR_W, I_TRACK, I_SNAP, I_BITS,
    I_COUNT
};

static void cfg_from_arrays(Cfg *c, const jdouble *d, const jint *ip) {
    defaults(c);
    c->fmin = d[D_FMIN]; c->fmax = d[D_FMAX]; c->qmul = d[D_QMUL]; c->qsmul = d[D_QSMUL];
    c->attack = d[D_ATTACK]; c->release = d[D_RELEASE]; c->expo = d[D_EXPO];
    c->gate_db = d[D_GATE_DB]; c->tilt = d[D_TILT]; c->boost_db = d[D_BOOST_DB];
    c->formant = d[D_FORMANT]; c->lfo_rate = d[D_LFO_RATE]; c->lfo_depth = d[D_LFO_DEPTH];
    c->stretch = d[D_STRETCH]; c->smear = d[D_SMEAR]; c->quant = d[D_QUANT];
    c->pitch = d[D_PITCH]; c->detune = d[D_DETUNE]; c->pw = d[D_PW];
    c->fm_ratio = d[D_FM_RATIO]; c->fm_index = d[D_FM_INDEX];
    c->vib_rate = d[D_VIB_RATE]; c->vib_depth = d[D_VIB_DEPTH];
    c->glide = d[D_GLIDE]; c->noise_mix = d[D_NOISE_MIX]; c->track_shift = d[D_TRACK_SHIFT];
    c->sib = d[D_SIB]; c->sib_freq = d[D_SIB_FREQ];
    c->dry = d[D_DRY]; c->wet = d[D_WET]; c->crush = d[D_CRUSH]; c->srr = d[D_SRR];
    c->drive = d[D_DRIVE]; c->ring = d[D_RING]; c->ring_mix = d[D_RING_MIX];
    c->delay_ms = d[D_DELAY_MS]; c->delay_fb = d[D_DELAY_FB]; c->delay_mix = d[D_DELAY_MIX];
    c->stereo = d[D_STEREO];

    c->bands = ip[I_BANDS]; c->order = ip[I_ORDER]; c->spacing = ip[I_SPACING];
    c->wave = ip[I_WAVE]; c->unison = ip[I_UNISON]; c->reverse = ip[I_REVERSE];
    c->rotate = ip[I_ROTATE]; c->scramble = ip[I_SCRAMBLE]; c->skip = ip[I_SKIP];
    c->smear_w = ip[I_SMEAR_W]; c->track = ip[I_TRACK]; c->snap = ip[I_SNAP];
    c->bits = ip[I_BITS];

    c->nnotes = 1; c->notes[0] = 0;
    if (c->bands < 1) c->bands = 1;
    if (c->bands > 200) c->bands = 200;
    if (c->order < 1) c->order = 1;
    if (c->order > MAX_STAGES) c->order = MAX_STAGES;
}

static void arrays_from_cfg(const Cfg *c, jdouble *d, jint *ip) {
    d[D_FMIN] = c->fmin; d[D_FMAX] = c->fmax; d[D_QMUL] = c->qmul; d[D_QSMUL] = c->qsmul;
    d[D_ATTACK] = c->attack; d[D_RELEASE] = c->release; d[D_EXPO] = c->expo;
    d[D_GATE_DB] = c->gate_db; d[D_TILT] = c->tilt; d[D_BOOST_DB] = c->boost_db;
    d[D_FORMANT] = c->formant; d[D_LFO_RATE] = c->lfo_rate; d[D_LFO_DEPTH] = c->lfo_depth;
    d[D_STRETCH] = c->stretch; d[D_SMEAR] = c->smear; d[D_QUANT] = c->quant;
    d[D_PITCH] = c->pitch; d[D_DETUNE] = c->detune; d[D_PW] = c->pw;
    d[D_FM_RATIO] = c->fm_ratio; d[D_FM_INDEX] = c->fm_index;
    d[D_VIB_RATE] = c->vib_rate; d[D_VIB_DEPTH] = c->vib_depth;
    d[D_GLIDE] = c->glide; d[D_NOISE_MIX] = c->noise_mix; d[D_TRACK_SHIFT] = c->track_shift;
    d[D_SIB] = c->sib; d[D_SIB_FREQ] = c->sib_freq;
    d[D_DRY] = c->dry; d[D_WET] = c->wet; d[D_CRUSH] = c->crush; d[D_SRR] = c->srr;
    d[D_DRIVE] = c->drive; d[D_RING] = c->ring; d[D_RING_MIX] = c->ring_mix;
    d[D_DELAY_MS] = c->delay_ms; d[D_DELAY_FB] = c->delay_fb; d[D_DELAY_MIX] = c->delay_mix;
    d[D_STEREO] = c->stereo;

    ip[I_BANDS] = c->bands; ip[I_ORDER] = c->order; ip[I_SPACING] = c->spacing;
    ip[I_WAVE] = c->wave; ip[I_UNISON] = c->unison; ip[I_REVERSE] = c->reverse;
    ip[I_ROTATE] = c->rotate; ip[I_SCRAMBLE] = c->scramble; ip[I_SKIP] = c->skip;
    ip[I_SMEAR_W] = c->smear_w; ip[I_TRACK] = c->track; ip[I_SNAP] = c->snap;
    ip[I_BITS] = c->bits;
}

/* jint Java_com_xvoc_app_NativeVocoder_process(String inPath, String outPath,
 *                                               double[39] params, int[13] iparams)
 * Returns 0 on success, negative on failure. */
JNIEXPORT jint JNICALL
Java_com_xvoc_app_NativeVocoder_process(JNIEnv *env, jobject thiz,
                                         jstring jin, jstring jout,
                                         jdoubleArray jd, jintArray ji) {
    (void)thiz;
    if ((*env)->GetArrayLength(env, jd) < D_COUNT || (*env)->GetArrayLength(env, ji) < I_COUNT) {
        LOGE("param array too small");
        return -10;
    }
    const char *inPath = (*env)->GetStringUTFChars(env, jin, NULL);
    const char *outPath = (*env)->GetStringUTFChars(env, jout, NULL);
    jdouble *d = (*env)->GetDoubleArrayElements(env, jd, NULL);
    jint *ip = (*env)->GetIntArrayElements(env, ji, NULL);

    Cfg c;
    cfg_from_arrays(&c, d, ip);
    c.in = inPath;
    c.out = outPath;

    Audio mod = {0}, car = {0};
    int rc = read_wav(c.in, &mod);
    if (rc) {
        LOGE("read_wav failed for %s", inPath);
        rc = -1;
    } else {
        rc = run_vocoder(c, &mod, &car);
        if (rc) LOGE("run_vocoder failed, rc=%d", rc);
        free(mod.d);
    }

    (*env)->ReleaseStringUTFChars(env, jin, inPath);
    (*env)->ReleaseStringUTFChars(env, jout, outPath);
    (*env)->ReleaseDoubleArrayElements(env, jd, d, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, ji, ip, JNI_ABORT);
    return rc;
}

/* jint Java_com_xvoc_app_NativeVocoder_randomize(long seed, double[39] outD, int[13] outI)
 * Fills outD/outI (must already be allocated at the right length) with a
 * randomized patch, same algorithm as `xvoc --randomize`. Returns 0. */
JNIEXPORT jint JNICALL
Java_com_xvoc_app_NativeVocoder_randomize(JNIEnv *env, jobject thiz,
                                           jlong seed, jdoubleArray jd, jintArray ji) {
    (void)thiz;
    Cfg c;
    defaults(&c);
    randomize_cfg(&c, (unsigned)seed);

    jdouble d[D_COUNT];
    jint ip[I_COUNT];
    arrays_from_cfg(&c, d, ip);
    (*env)->SetDoubleArrayRegion(env, jd, 0, D_COUNT, d);
    (*env)->SetIntArrayRegion(env, ji, 0, I_COUNT, ip);
    return 0;
}

/* long[2] Java_com_xvoc_app_NativeVocoder_probe(String path) -> {frames, sampleRate} or null */
JNIEXPORT jlongArray JNICALL
Java_com_xvoc_app_NativeVocoder_probe(JNIEnv *env, jobject thiz, jstring jpath) {
    (void)thiz;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    Audio a = {0};
    int rc = read_wav(path, &a);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (rc) return NULL;
    jlong out[2] = { (jlong)a.n, (jlong)a.sr };
    free(a.d);
    jlongArray res = (*env)->NewLongArray(env, 2);
    (*env)->SetLongArrayRegion(env, res, 0, 2, out);
    return res;
}
