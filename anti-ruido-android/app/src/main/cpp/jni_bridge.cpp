// Ponte JNI: br.com.antiruido.audio.NativeAnc <-> AncEngine / AncProcessor.
#include <jni.h>

#include <vector>

#include "AncEngine.h"

namespace {

using anc::AncProcessor;

// Layout do vetor de status (espelhado em AncStatus.kt).
constexpr int kStatusHeader = 27;
constexpr int kHarmonicFields = 10;
constexpr int kStatusSize = kStatusHeader + anc::AncController::kMaxHarmonics * kHarmonicFields;

AncEngine& engine() { return AncEngine::instance(); }

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeStart(JNIEnv* env, jobject, jint inputPreset,
                                                                            jboolean lowLatency) {
    AncEngine::Options o;
    o.inputPreset = inputPreset;
    o.lowLatency = lowLatency == JNI_TRUE;
    const std::string err = engine().start(o);
    if (err.empty()) return nullptr;
    return env->NewStringUTF(err.c_str());
}

JNIEXPORT void JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeStop(JNIEnv*, jobject) { engine().stop(); }

JNIEXPORT jboolean JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeIsRunning(JNIEnv*, jobject) {
    return engine().running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeRequestMode(JNIEnv*, jobject, jint mode) {
    engine().requestMode(static_cast<AncProcessor::Mode>(mode));
}

JNIEXPORT void JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeSetSettings(
    JNIEnv*, jobject, jint harmonics, jfloat convergence, jfloat maxOutput, jfloat fmin, jfloat fmax,
    jfloat minProminenceDb, jboolean autoLatency, jfloat burstLevel, jfloat calBurstLevel, jfloat probeLevel) {
    AncProcessor::Settings s;
    s.harmonics = harmonics;
    s.convergence = convergence;
    s.maxOutput = maxOutput;
    s.fmin = fmin;
    s.fmax = fmax;
    s.minProminenceDb = minProminenceDb;
    s.autoLatency = autoLatency == JNI_TRUE;
    s.burstLevel = burstLevel;
    s.calBurstLevel = calBurstLevel;
    s.probeLevel = probeLevel;
    engine().processor().setSettings(s);
}

JNIEXPORT void JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeSetHints(JNIEnv* env, jobject, jfloatArray freqs) {
    if (!freqs) {
        engine().processor().setHints(nullptr, 0);
        return;
    }
    const jsize n = env->GetArrayLength(freqs);
    std::vector<float> v(n);
    env->GetFloatArrayRegion(freqs, 0, n, v.data());
    engine().processor().setHints(v.data(), static_cast<int>(n));
}

JNIEXPORT void JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeNotifyPathChanged(JNIEnv*, jobject) {
    engine().processor().notifyDiscontinuity();
}

JNIEXPORT jboolean JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeLoadModel(JNIEnv* env, jobject,
                                                                                 jfloatArray data) {
    if (!data) return JNI_FALSE;
    const jsize n = env->GetArrayLength(data);
    std::vector<float> v(n);
    env->GetFloatArrayRegion(data, 0, n, v.data());
    return engine().processor().loadModel(v.data(), static_cast<int>(n)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeExportModel(JNIEnv* env, jobject) {
    auto& p = engine().processor();
    const int size = p.exportModel(nullptr, 0);
    if (size <= 0) return nullptr;
    std::vector<float> v(size);
    const int n = p.exportModel(v.data(), size);
    if (n <= 0) return nullptr;
    jfloatArray arr = env->NewFloatArray(n);
    env->SetFloatArrayRegion(arr, 0, n, v.data());
    return arr;
}

JNIEXPORT jint JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeGetStatus(JNIEnv* env, jobject, jfloatArray out) {
    const AncProcessor::Status s = engine().processor().status();
    float v[kStatusSize] = {};
    int k = 0;
    v[k++] = static_cast<float>(s.mode);
    v[k++] = static_cast<float>(s.error);
    v[k++] = static_cast<float>(s.calStage);
    v[k++] = s.calProgress;
    v[k++] = static_cast<float>(s.calStep);
    v[k++] = static_cast<float>(s.calSteps);
    v[k++] = s.latencyMs;
    v[k++] = s.pathGainDb;
    v[k++] = s.latencyConfidence;
    v[k++] = s.latencyVerified ? 1.0f : 0.0f;
    v[k++] = s.toneFound ? 1.0f : 0.0f;
    v[k++] = s.toneHz;
    v[k++] = s.toneProminenceDb;
    v[k++] = s.toneLevelDb;
    v[k++] = s.locked ? 1.0f : 0.0f;
    v[k++] = s.ancFreq;
    v[k++] = s.micDb;
    v[k++] = s.outDb;
    v[k++] = static_cast<float>(s.divergences);
    v[k++] = static_cast<float>(s.limiterHits);
    v[k++] = static_cast<float>(s.discontinuities);
    v[k++] = s.modelValid ? 1.0f : 0.0f;
    v[k++] = static_cast<float>(s.modelRate);
    v[k++] = static_cast<float>(s.modelPoints);
    v[k++] = static_cast<float>(s.modelSerial);
    v[k++] = static_cast<float>(s.sampleRate);
    v[k++] = s.lag;
    for (const auto& h : s.harm) {
        v[k++] = h.active ? 1.0f : 0.0f;
        v[k++] = h.freq;
        v[k++] = h.errorAmp;
        v[k++] = h.disturbanceAmp;
        v[k++] = h.outputAmp;
        v[k++] = h.reductionDb;
        v[k++] = h.verified ? 1.0f : 0.0f;
        v[k++] = h.corrDb;
        v[k++] = h.corrDeg;
        v[k++] = h.pll ? 1.0f : 0.0f;
    }
    const jsize n = std::min<jsize>(env->GetArrayLength(out), kStatusSize);
    env->SetFloatArrayRegion(out, 0, n, v);
    return kStatusSize;
}

JNIEXPORT jint JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeGetSpectrum(JNIEnv* env, jobject,
                                                                               jfloatArray estimate, jfloatArray mic,
                                                                               jfloatArray meta) {
    const jsize cap = std::min(env->GetArrayLength(estimate), env->GetArrayLength(mic));
    std::vector<float> a(cap), b(cap);
    float binHz = 0.0f;
    const int n = engine().processor().spectrum(a.data(), b.data(), cap, &binHz);
    env->SetFloatArrayRegion(estimate, 0, n, a.data());
    env->SetFloatArrayRegion(mic, 0, n, b.data());
    if (meta && env->GetArrayLength(meta) >= 1) env->SetFloatArrayRegion(meta, 0, 1, &binHz);
    return n;
}

JNIEXPORT void JNICALL Java_br_com_antiruido_audio_NativeAnc_nativeGetEngineInfo(JNIEnv* env, jobject,
                                                                                 jfloatArray out) {
    const AncEngine::Info i = engine().info();
    const float v[] = {static_cast<float>(i.sampleRate),  static_cast<float>(i.api),
                       static_cast<float>(i.outBurst),    static_cast<float>(i.outBuffer),
                       static_cast<float>(i.inBurst),     static_cast<float>(i.outXruns),
                       static_cast<float>(i.inXruns),     i.outLatencyMs,
                       i.inLatencyMs,                     static_cast<float>(i.restarts),
                       static_cast<float>(i.lastCallbackFrames)};
    const jsize n = std::min<jsize>(env->GetArrayLength(out), static_cast<jsize>(sizeof(v) / sizeof(v[0])));
    env->SetFloatArrayRegion(out, 0, n, v);
}

}  // extern "C"
