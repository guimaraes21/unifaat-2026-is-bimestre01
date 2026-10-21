#include "AncEngine.h"

#include <android/log.h>

#include <chrono>

#define LOG_TAG "AntiRuido"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {
constexpr int kInputChunk = 4096;
}  // namespace

AncEngine& AncEngine::instance() {
    static AncEngine engine;
    return engine;
}

std::string AncEngine::start(const Options& options) {
    std::lock_guard<std::mutex> guard(lock_);
    if (running_.load()) return "";
    options_ = options;
    const std::string err = openStreams();
    if (!err.empty()) {
        closeStreams();
        return err;
    }
    running_.store(true);
    {
        std::lock_guard<std::mutex> wl(workerMutex_);
        workerStop_ = false;
    }
    worker_ = std::thread(&AncEngine::workerLoop, this);
    return "";
}

void AncEngine::stop() {
    {
        std::lock_guard<std::mutex> wl(workerMutex_);
        workerStop_ = true;
    }
    workerCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> guard(lock_);
    closeStreams();
    running_.store(false);
}

void AncEngine::requestMode(anc::AncProcessor::Mode mode) {
    // Lembrado para reaplicar se os streams precisarem ser reabertos (troca de dispositivo).
    if (mode == anc::AncProcessor::Mode::Anc || mode == anc::AncProcessor::Mode::Monitor) {
        desiredMode_.store(static_cast<int>(mode));
    }
    processor_.requestMode(mode);
}

std::string AncEngine::openStreams() {
    const auto perf = options_.lowLatency ? oboe::PerformanceMode::LowLatency : oboe::PerformanceMode::None;

    // Saída primeiro: ela dita a taxa de amostragem (a nativa do aparelho, sem reamostrar).
    oboe::AudioStreamBuilder ob;
    ob.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(perf)
        ->setSharingMode(oboe::SharingMode::Shared)  // a música dos outros apps continua tocando
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setUsage(oboe::Usage::Media)
        ->setContentType(oboe::ContentType::Music)
        ->setFormatConversionAllowed(true)
        ->setChannelConversionAllowed(true)
        ->setDataCallback(this)
        ->setErrorCallback(this);
    oboe::Result r = ob.openStream(out_);
    if (r != oboe::Result::OK) return std::string("Falha ao abrir a saída de áudio: ") + oboe::convertToText(r);

    const int fs = out_->getSampleRate();
    outChannels_ = out_->getChannelCount();
    // Buffer folgado: latência maior não atrapalha o algoritmo, mas xrun obriga a re-medir.
    const int burst = std::max(1, out_->getFramesPerBurst());
    const int wanted = std::max(burst * 6, fs / 25);
    out_->setBufferSizeInFrames(std::min(wanted, out_->getBufferCapacityInFrames()));

    oboe::AudioStreamBuilder ib;
    ib.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(perf)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(fs)
        ->setInputPreset(static_cast<oboe::InputPreset>(options_.inputPreset))
        ->setFormatConversionAllowed(true)
        ->setChannelConversionAllowed(true)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setBufferCapacityInFrames(std::max(8192, fs / 4));
    r = ib.openStream(in_);
    if (r != oboe::Result::OK) {
        return std::string("Falha ao abrir o microfone: ") + oboe::convertToText(r) +
               ". Verifique a permissão e se o microfone da multimídia está disponível para apps.";
    }

    processor_.configure(fs);
    mono_.assign(std::max(out_->getBufferCapacityInFrames(), 8192), 0.0f);
    inBuf_.assign(kInputChunk, 0.0f);
    inputFlushed_ = false;
    callbacks_ = 0;
    lastOutXruns_ = lastInXruns_ = 0;

    r = in_->requestStart();
    if (r != oboe::Result::OK) return std::string("Falha ao iniciar o microfone: ") + oboe::convertToText(r);
    r = out_->requestStart();
    if (r != oboe::Result::OK) return std::string("Falha ao iniciar a saída: ") + oboe::convertToText(r);

    processor_.requestMode(static_cast<anc::AncProcessor::Mode>(desiredMode_.load()));
    LOGI("streams abertos: %d Hz, burst saída %d, buffer %d, api %d", fs, burst, out_->getBufferSizeInFrames(),
         static_cast<int>(out_->getAudioApi()));
    return "";
}

void AncEngine::closeStreams() {
    if (out_) {
        out_->stop();
        out_->close();
        out_.reset();
    }
    if (in_) {
        in_->stop();
        in_->close();
        in_.reset();
    }
}

oboe::DataCallbackResult AncEngine::onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) {
    auto* out = static_cast<float*>(audioData);
    const int ch = std::max(1, stream->getChannelCount());
    lastCallbackFrames_.store(numFrames, std::memory_order_relaxed);

    oboe::AudioStream* in = in_.get();
    if (!inputFlushed_ && in) {
        // O microfone começou antes da saída: descarta o acumulado para não somar latência à toa.
        for (int guard = 0; guard < 64; ++guard) {
            auto rr = in->read(inBuf_.data(), kInputChunk, 0);
            if (!rr || rr.value() < kInputChunk) break;
        }
        inputFlushed_ = true;
    }

    int done = 0;
    while (done < numFrames) {
        const int len = std::min(numFrames - done, static_cast<int>(mono_.size()));
        processor_.render(mono_.data(), len);
        float* o = out + static_cast<size_t>(done) * ch;
        for (int i = 0; i < len; ++i) {
            for (int c = 0; c < ch; ++c) o[i * ch + c] = mono_[i];
        }
        done += len;
    }

    // Lê tudo o que o microfone já entregou (sem bloquear).
    if (in) {
        for (int guard = 0; guard < 16; ++guard) {
            auto rr = in->read(inBuf_.data(), kInputChunk, 0);
            if (!rr) {
                if (rr.error() == oboe::Result::ErrorDisconnected) {
                    restartRequested_.store(true);
                    workerCv_.notify_all();
                    return oboe::DataCallbackResult::Stop;
                }
                break;
            }
            const int got = rr.value();
            if (got > 0) processor_.capture(inBuf_.data(), got);
            if (got < kInputChunk) break;
        }
    }

    // Xrun desloca a relação entre amostra escrita e lida: o DSP precisa re-medir a latência.
    if (++callbacks_ % 32 == 0) {
        auto xo = stream->getXRunCount();
        if (xo && xo.value() != lastOutXruns_) {
            lastOutXruns_ = xo.value();
            processor_.notifyDiscontinuity();
        }
        if (in) {
            auto xi = in->getXRunCount();
            if (xi && xi.value() != lastInXruns_) {
                lastInXruns_ = xi.value();
                processor_.notifyDiscontinuity();
            }
        }
    }
    return oboe::DataCallbackResult::Continue;
}

void AncEngine::onErrorAfterClose(oboe::AudioStream* /*stream*/, oboe::Result error) {
    // Ex.: saída de áudio trocada. Reabre tudo na thread de trabalho.
    LOGW("stream fechado: %s", oboe::convertToText(error));
    restartRequested_.store(true);
    workerCv_.notify_all();
}

void AncEngine::workerLoop() {
    std::unique_lock<std::mutex> wl(workerMutex_);
    while (!workerStop_) {
        workerCv_.wait_for(wl, std::chrono::milliseconds(10));
        if (workerStop_) break;
        wl.unlock();
        if (processor_.hasOfflineWork()) processor_.doOfflineWork();
        if (restartRequested_.exchange(false)) {
            std::lock_guard<std::mutex> guard(lock_);
            closeStreams();
            const std::string err = openStreams();
            restarts_.fetch_add(1);
            if (!err.empty()) {
                LOGW("falha ao reabrir: %s", err.c_str());
                closeStreams();
                // Tenta de novo em breve (ex.: dispositivo ainda trocando).
                restartRequested_.store(true);
                wl.lock();
                workerCv_.wait_for(wl, std::chrono::milliseconds(500));
                continue;
            }
        }
        wl.lock();
    }
}

AncEngine::Info AncEngine::info() {
    Info i;
    std::lock_guard<std::mutex> guard(lock_);
    i.restarts = restarts_.load();
    i.lastCallbackFrames = lastCallbackFrames_.load();
    if (!out_) return i;
    i.sampleRate = out_->getSampleRate();
    switch (out_->getAudioApi()) {
        case oboe::AudioApi::OpenSLES:
            i.api = 1;
            break;
        case oboe::AudioApi::AAudio:
            i.api = 2;
            break;
        default:
            i.api = 0;
    }
    i.outBurst = out_->getFramesPerBurst();
    i.outBuffer = out_->getBufferSizeInFrames();
    auto xo = out_->getXRunCount();
    if (xo) i.outXruns = xo.value();
    auto lo = out_->calculateLatencyMillis();
    if (lo) i.outLatencyMs = static_cast<float>(lo.value());
    if (in_) {
        i.inBurst = in_->getFramesPerBurst();
        auto xi = in_->getXRunCount();
        if (xi) i.inXruns = xi.value();
        auto li = in_->calculateLatencyMillis();
        if (li) i.inLatencyMs = static_cast<float>(li.value());
    }
    return i;
}
