// Motor de áudio full-duplex com Oboe: o callback da saída gera a anti-onda e, no mesmo
// callback, lê tudo o que o microfone tiver entregue. O núcleo de DSP (dsp/) não sabe nada de
// Android; este arquivo é só a cola com o sistema.
#pragma once

#include <oboe/Oboe.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/AncProcessor.h"

class AncEngine : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    struct Options {
        int inputPreset = static_cast<int>(oboe::InputPreset::VoiceRecognition);
        bool lowLatency = false;
    };

    struct Info {
        int sampleRate = 0;
        int api = 0;  // 0 = desconhecida, 1 = OpenSL ES, 2 = AAudio
        int outBurst = 0;
        int outBuffer = 0;
        int inBurst = 0;
        int outXruns = 0;
        int inXruns = 0;
        float outLatencyMs = -1.0f;
        float inLatencyMs = -1.0f;
        int restarts = 0;
        int lastCallbackFrames = 0;
    };

    static AncEngine& instance();

    // Retorna string vazia em caso de sucesso ou a mensagem de erro.
    std::string start(const Options& options);
    void stop();
    bool running() const { return running_.load(); }
    void requestMode(anc::AncProcessor::Mode mode);
    anc::AncProcessor& processor() { return processor_; }
    Info info();

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    AncEngine() = default;
    std::string openStreams();
    void closeStreams();
    void workerLoop();

    anc::AncProcessor processor_;
    Options options_;
    std::mutex lock_;
    std::shared_ptr<oboe::AudioStream> out_;
    std::shared_ptr<oboe::AudioStream> in_;
    std::vector<float> mono_;
    std::vector<float> inBuf_;
    int outChannels_ = 2;
    bool inputFlushed_ = false;
    int callbacks_ = 0;
    int lastOutXruns_ = 0;
    int lastInXruns_ = 0;
    std::atomic<int> lastCallbackFrames_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> restartRequested_{false};
    std::atomic<int> restarts_{0};
    std::atomic<int> desiredMode_{0};
    std::thread worker_;
    std::mutex workerMutex_;
    std::condition_variable workerCv_;
    bool workerStop_ = false;
};
