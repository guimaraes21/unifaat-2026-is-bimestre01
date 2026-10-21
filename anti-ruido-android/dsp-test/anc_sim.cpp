// Simulador da cabine + pipeline de áudio do Android para validar o núcleo de DSP sem hardware.
//
// Modela: buffer de saída com callbacks em blocos (pull), entrada entregue em rajadas,
// atraso acústico, alto-falante sem graves profundos, modo acústico da cabine, ronco do motor
// com harmônicas, ruído de rodagem, música, xruns e deriva de clock entre entrada e saída.
//
// Uso: anc_sim            -> roda todos os cenários
//      anc_sim <cenário>  -> roda só um (calibracao, cruzeiro, rampa, xrun, deriva, volume,
//                            erro_modelo, latencia_alta, oscilacao, musica_alta, modelo_errado)
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "AncProcessor.h"
#include "Filters.h"

using namespace anc;

namespace {

struct SimConfig {
    int fs = 48000;
    int outBurst = 960;       // 20 ms por callback (caminho "legado" típico de multimídia chinesa)
    int outLevel = 3840;      // 80 ms de buffer de saída
    int inBurst = 960;        // microfone entregue em rajadas de 20 ms
    int outHwDelay = 2400;    // 50 ms depois do buffer do app (mixer, HAL, DSP da multimídia)
    int inHwDelay = 1440;     // 30 ms antes do microfone chegar ao app (ADC, HAL)
    int acousticDelay = 96;   // ~0,7 m entre alto-falante e microfone
    float cabinGain = 0.3f;
    double rpm = 2000.0;
    int cylinders = 4;        // ordem dominante = cilindros/2
    float a1 = 0.05f;         // fundamental do ronco no microfone
    float a2 = 0.02f;         // 2ª harmônica
    float road = 0.004f;      // ruído de rodagem (banda larga, graves)
    float music = 0.01f;      // música tocando ao mesmo tempo
    double driftPpm = 0.0;    // deriva de clock entrada x saída
    float dcOffset = 0.01f;
};

// Caminho acústico real (desconhecido pelo algoritmo): alto-falante -> microfone.
struct Cabin {
    Biquad speakerHp, mode, lowpass;
    std::vector<float> delay;
    size_t pos = 0;
    float gain = 1.0f;
    int fs = 48000;

    void configure(const SimConfig& c) {
        fs = c.fs;
        speakerHp.setHighpass(c.fs, 45.0f, 0.707f);
        mode.setPeaking(c.fs, 75.0f, 3.0f, 6.0f);
        lowpass.setLowpass(c.fs, 1500.0f, 0.707f);
        delay.assign(c.acousticDelay + 1, 0.0f);
        gain = c.cabinGain;
    }
    float process(float x) {
        float y = lowpass.process(mode.process(speakerHp.process(x))) * gain;
        delay[pos] = y;
        pos = (pos + 1) % delay.size();
        return delay[pos];
    }
    float magnitude(float f) const {
        return std::abs(speakerHp.response(fs, f) * mode.response(fs, f) * lowpass.response(fs, f)) * gain;
    }
};

struct Measure {
    std::complex<double> mic1, mic2, d1, d2;
    double n = 0;
    void reset() { *this = Measure(); }
    float red1() const { return 20.0f * std::log10(static_cast<float>(std::abs(mic1) / std::abs(d1))); }
    float red2() const { return 20.0f * std::log10(static_cast<float>(std::abs(mic2) / std::abs(d2))); }
};

class Sim {
public:
    explicit Sim(const SimConfig& c) : cfg(c), rng(1234) {
        cabin.configure(c);
        roadLp.setLowpass(c.fs, 120.0f, 0.707f);
        outHw.assign(c.outHwDelay + 1, 0.0f);
        inHw.assign(c.inHwDelay + 1, 0.0f);
        proc.configure(c.fs);
        outBuf.resize(c.outBurst);
    }

    SimConfig cfg;
    AncProcessor proc;
    Cabin cabin;
    std::function<double(double)> rpmProfile;
    Measure meas;
    bool measuring = false;
    float maxOut = 0.0f;
    int underruns = 0;
    double t = 0.0;

    void stall(int ticks) { stallTicks = ticks; }
    void dropInput(int frames) { dropTicks = frames; }

    void run(double seconds) {
        const int64_t ticks = static_cast<int64_t>(seconds * cfg.fs);
        for (int64_t i = 0; i < ticks; ++i) tick();
    }

    // Roda até a condição ser verdadeira (checada a cada 10 ms) ou estourar o tempo.
    bool runUntil(const std::function<bool()>& cond, double maxSeconds) {
        const int64_t ticks = static_cast<int64_t>(maxSeconds * cfg.fs);
        for (int64_t i = 0; i < ticks; ++i) {
            tick();
            if (i % (cfg.fs / 100) == 0 && cond()) return true;
        }
        return cond();
    }

    AncProcessor::Status status() { return proc.status(); }

private:
    void callback() {
        proc.render(outBuf.data(), cfg.outBurst);
        for (float v : outBuf) {
            outFifo.push_back(v);
            maxOut = std::max(maxOut, std::fabs(v));
        }
        if (!inFifo.empty()) {
            inRead.assign(inFifo.begin(), inFifo.end());
            inFifo.clear();
            proc.capture(inRead.data(), static_cast<int>(inRead.size()));
        }
        if (proc.hasOfflineWork()) proc.doOfflineWork();
    }

    void tick() {
        if (stallTicks > 0) {
            --stallTicks;
        } else {
            while (static_cast<int>(outFifo.size()) < cfg.outLevel) callback();
        }
        float spk = 0.0f;
        if (!outFifo.empty()) {
            spk = outFifo.front();
            outFifo.pop_front();
        } else {
            ++underruns;
        }
        outHw[outHwPos] = spk;
        outHwPos = (outHwPos + 1) % outHw.size();
        const float s = cabin.process(outHw[outHwPos]);

        const double rpm = rpmProfile ? rpmProfile(t) : cfg.rpm;
        const double f1 = rpm / 60.0 * cfg.cylinders / 2.0;
        theta1 = fracCycles(theta1 + f1 / cfg.fs);
        const double th2 = fracCycles(2.0 * theta1 + 0.13);
        const float d = cfg.a1 * std::cos(2.0 * M_PI * theta1) + cfg.a2 * std::cos(2.0 * M_PI * th2);

        const float road = roadLp.process(rng.uniform()) * cfg.road * 8.0f;
        musicPhase = fracCycles(musicPhase + musicFreq / cfg.fs);
        if (++musicNote >= cfg.fs / 4) {  // nota nova a cada 250 ms
            musicNote = 0;
            musicFreq = 110.0 * std::pow(2.0, (rng.next() % 36) / 12.0);
        }
        const float music = cfg.music * std::sin(2.0 * M_PI * musicPhase) + 0.3f * cfg.music * rng.uniform();
        const float micNow = d + s + road + music + cfg.dcOffset;
        inHw[inHwPos] = micNow;
        inHwPos = (inHwPos + 1) % inHw.size();
        const float mic = inHw[inHwPos];

        if (measuring) {
            const std::complex<double> r1 = std::polar(1.0, -2.0 * M_PI * theta1);
            const std::complex<double> r2 = std::polar(1.0, -2.0 * M_PI * th2);
            meas.mic1 += static_cast<double>(micNow) * r1;
            meas.mic2 += static_cast<double>(micNow) * r2;
            meas.d1 += static_cast<double>(d) * r1;
            meas.d2 += static_cast<double>(d) * r2;
            meas.n += 1;
        }

        // Captura com deriva de clock: o ADC "pula" ou "repete" uma amostra de vez em quando.
        int copies = 1;
        driftAcc += cfg.driftPpm * 1e-6;
        if (driftAcc >= 1.0) {
            driftAcc -= 1.0;
            copies = 0;
        } else if (driftAcc <= -1.0) {
            driftAcc += 1.0;
            copies = 2;
        }
        for (int c = 0; c < copies; ++c) {
            if (dropTicks > 0) {
                --dropTicks;  // overrun: amostra perdida
                continue;
            }
            inPending.push_back(mic);
            if (static_cast<int>(inPending.size()) >= cfg.inBurst) {
                inFifo.insert(inFifo.end(), inPending.begin(), inPending.end());
                inPending.clear();
            }
        }
        t += 1.0 / cfg.fs;
    }

    Random rng;
    Biquad roadLp;
    std::vector<float> outBuf, inRead, outHw, inHw;
    size_t outHwPos = 0, inHwPos = 0;
    std::deque<float> outFifo;
    std::vector<float> inFifo, inPending;
    double theta1 = 0.0;
    double musicPhase = 0.0, musicFreq = 220.0;
    int musicNote = 0;
    int stallTicks = 0, dropTicks = 0;
    double driftAcc = 0.0;
};

const char* modeName(int m) {
    switch (m) {
        case 0: return "monitor";
        case 1: return "ANC";
        case 2: return "calibrando";
        case 3: return "latencia";
        default: return "?";
    }
}

void printStatus(Sim& sim, const char* tag) {
    const auto s = sim.status();
    std::printf("  [%6.1fs %-10s] modo=%s erro=%d tom=%.2f Hz anc=%.2f Hz lock=%d atraso=%.2f ms "
                "red1=%.1f dB red2=%.1f dB saida=%.1f dBFS div=%d\n",
                sim.t, tag, modeName(s.mode), s.error, s.toneHz, s.ancFreq, s.locked, s.latencyMs,
                s.harm[0].reductionDb, s.harm[1].reductionDb, s.outDb, s.divergences);
}

// Calibra com o motor desligado (só ruído de fundo), como o app pede ao usuário.
bool calibrate(Sim& sim) {
    const SimConfig saved = sim.cfg;
    sim.cfg.a1 = sim.cfg.a2 = 0.0f;
    sim.cfg.music = 0.0f;
    sim.cfg.road = 0.001f;
    sim.run(0.5);
    sim.proc.requestMode(AncProcessor::Mode::Calibrate);
    sim.run(0.2);
    const bool ok = sim.runUntil([&] { return sim.status().mode == 0; }, 120.0);
    sim.cfg = saved;
    sim.cfg.a1 = saved.a1;
    const auto s = sim.status();
    std::printf("  calibração: ok=%d erro=%d pontos válidos=%d atraso=%.2f ms (confiança %.1f)\n", ok,
                s.error, s.modelPoints, s.latencyMs, s.latencyConfidence);
    return ok && s.modelValid && s.error == 0;
}

bool startAnc(Sim& sim) {
    sim.proc.requestMode(AncProcessor::Mode::Anc);
    return sim.runUntil([&] { return sim.status().mode == 1; }, 10.0);
}

Measure measureFor(Sim& sim, double seconds) {
    sim.meas.reset();
    sim.measuring = true;
    sim.run(seconds);
    sim.measuring = false;
    return sim.meas;
}

bool expect(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "OK  " : "FALHA", what);
    return cond;
}

// --- cenários -------------------------------------------------------------------------------

bool scenarioCalibration() {
    SimConfig c;
    Sim sim(c);
    bool ok = calibrate(sim);
    std::vector<float> model(sim.proc.exportModel(nullptr, 0));
    sim.proc.exportModel(model.data(), static_cast<int>(model.size()));
    SecondaryPath p;
    ok &= expect(p.deserialize(model.data(), static_cast<int>(model.size())), "modelo exportado e relido");
    // A magnitude de H0 independe do atraso: compara com a acústica simulada.
    float worst = 0.0f;
    int checked = 0;
    for (int i = 0; i < p.numPoints; ++i) {
        if (!p.points[i].valid || p.points[i].freq < 30.0f) continue;
        const float err = std::fabs(ampToDb(std::abs(p.points[i].h0)) - ampToDb(sim.cabin.magnitude(p.points[i].freq)));
        worst = std::max(worst, err);
        ++checked;
    }
    std::printf("  pior erro de magnitude (>=30 Hz, %d pontos): %.2f dB\n", checked, worst);
    ok &= expect(checked >= 40, "pelo menos 40 pontos válidos acima de 30 Hz");
    ok &= expect(worst < 1.0f, "magnitude medida bate com a real (< 1 dB)");
    // Fase: a resposta de H0 deve variar suavemente (atraso bruto removido corretamente).
    float maxStep = 0.0f;
    for (int i = 1; i < p.numPoints; ++i) {
        if (!p.points[i].valid || !p.points[i - 1].valid) continue;
        maxStep = std::max(maxStep, std::fabs(wrapPi(std::arg(p.points[i].h0) - std::arg(p.points[i - 1].h0))));
    }
    std::printf("  maior salto de fase entre pontos vizinhos: %.1f graus\n", maxStep * 180.0f / kPi);
    ok &= expect(maxStep < 0.6f, "fase de H0 suave (atraso bruto bem estimado)");
    return ok;
}

bool scenarioCruise() {
    SimConfig c;
    Sim sim(c);
    // Pequena oscilação de rotação (acelerador nunca fica 100% parado).
    sim.rpmProfile = [](double t) { return 2000.0 + 8.0 * std::sin(2.0 * M_PI * t / 7.0); };
    bool ok = calibrate(sim);
    ok &= expect(startAnc(sim), "ANC ligou (com medição automática de latência)");
    sim.run(20.0);
    printStatus(sim, "cruzeiro");
    const Measure m = measureFor(sim, 5.0);
    std::printf("  redução real no microfone: %.1f dB (66,7 Hz)  %.1f dB (133 Hz)\n", m.red1(), m.red2());
    ok &= expect(m.red1() < -15.0f, "fundamental reduzida > 15 dB");
    ok &= expect(m.red2() < -10.0f, "2ª harmônica reduzida > 10 dB");
    ok &= expect(sim.maxOut <= 0.26f, "saída respeita o limite configurado");
    return ok;
}

bool scenarioRamp() {
    SimConfig c;
    Sim sim(c);
    double rampStart = 1e9;
    sim.rpmProfile = [&](double t) {
        if (t < rampStart) return 1800.0;
        const double u = std::min(1.0, (t - rampStart) / 4.0);
        return 1800.0 + 500.0 * u;  // 1800 -> 2300 rpm em 4 s
    };
    bool ok = calibrate(sim);
    ok &= startAnc(sim);
    sim.run(15.0);
    const Measure before = measureFor(sim, 3.0);
    rampStart = sim.t;
    sim.run(4.0);
    printStatus(sim, "fim rampa");
    sim.run(12.0);
    printStatus(sim, "estável");
    const Measure after = measureFor(sim, 4.0);
    std::printf("  antes da rampa: %.1f dB | depois: %.1f dB (fundamental)\n", before.red1(), after.red1());
    ok &= expect(before.red1() < -15.0f, "reduz a 1800 rpm");
    ok &= expect(after.red1() < -12.0f, "volta a reduzir depois de subir para 2300 rpm");
    return ok;
}

bool scenarioXrun() {
    SimConfig c;
    Sim sim(c);
    bool ok = calibrate(sim);
    ok &= startAnc(sim);
    sim.run(15.0);
    const Measure before = measureFor(sim, 3.0);
    sim.proc.notifyDiscontinuity();
    sim.stall(600);       // underrun de 12,5 ms na saída
    sim.run(5.0);
    sim.proc.notifyDiscontinuity();
    sim.dropInput(240);   // overrun: 5 ms perdidos na entrada
    sim.run(15.0);
    printStatus(sim, "pós-xrun");
    const Measure after = measureFor(sim, 4.0);
    std::printf("  antes: %.1f dB | depois de underrun+overrun: %.1f dB (underruns=%d)\n", before.red1(),
                after.red1(), sim.underruns);
    ok &= expect(after.red1() < -12.0f, "continua cancelando após xruns (rastreamento de defasagem)");
    return ok;
}

bool scenarioDrift() {
    SimConfig c;
    c.driftPpm = 30.0;  // microfone em outro clock (ex.: módulo Bluetooth)
    Sim sim(c);
    bool ok = calibrate(sim);
    ok &= startAnc(sim);
    sim.run(60.0);
    printStatus(sim, "deriva 60s");
    const Measure m = measureFor(sim, 4.0);
    std::printf("  após 60 s com deriva de 30 ppm (~%.1f ms acumulados): %.1f dB\n", 60.0 * 30e-6 * 1000.0 * 1.0,
                m.red1());
    ok &= expect(m.red1() < -12.0f, "compensa deriva de clock");
    return ok;
}

bool scenarioVolume() {
    SimConfig c;
    Sim sim(c);
    bool ok = calibrate(sim);

    // 1) Volume +10 dB antes de ligar: a medição automática na partida corrige o ganho.
    sim.cabin.gain *= 3.16f;
    ok &= startAnc(sim);
    sim.run(20.0);
    printStatus(sim, "vol+10 partida");
    Measure m = measureFor(sim, 4.0);
    std::printf("  volume +10 dB antes de ligar: %.1f dB (ganho medido %+.1f dB)\n", m.red1(), sim.status().pathGainDb);
    ok &= expect(m.red1() < -15.0f, "corrige o ganho na medição de partida");

    // 2) Volume -8 dB com o ANC ligado e o app avisado (observador de volume do Android).
    sim.cabin.gain *= 0.4f;
    sim.proc.notifyDiscontinuity();
    sim.run(20.0);
    printStatus(sim, "vol-8 avisado");
    m = measureFor(sim, 4.0);
    std::printf("  volume -8 dB com aviso: %.1f dB (ganho medido %+.1f dB)\n", m.red1(), sim.status().pathGainDb);
    ok &= expect(m.red1() < -15.0f, "re-mede quando o volume muda");

    // 3) Volume +8 dB sem aviso (controle de volume fora do Android): proteção + nova medição.
    sim.run(30.0);
    sim.cabin.gain *= 2.5f;
    sim.run(60.0);
    printStatus(sim, "vol+8 sem aviso");
    m = measureFor(sim, 4.0);
    const auto s = sim.status();
    std::printf("  volume +8 dB sem aviso: %.1f dB, divergências=%d\n", m.red1(), s.divergences);
    ok &= expect(s.mode == 1, "segue em ANC");
    ok &= expect(m.red1() < -12.0f, "recupera sozinho");
    return ok;
}

// Estraga o modelo salvo de um jeito que a rajada não enxerga (ex.: mudou o equalizador,
// o fader ou a acústica da cabine depois da calibração).
void corruptModel(Sim& sim, float rotDeg, float gain) {
    std::vector<float> model(sim.proc.exportModel(nullptr, 0));
    sim.proc.exportModel(model.data(), static_cast<int>(model.size()));
    SecondaryPath p;
    p.deserialize(model.data(), static_cast<int>(model.size()));
    for (int i = 0; i < p.numPoints; ++i) p.points[i].h0 *= std::polar(1.0f / gain, rotDeg * kPi / 180.0f);
    for (auto& b : p.bandMag) b /= gain;
    std::vector<float> bad(p.serializedSize());
    p.serialize(bad.data(), static_cast<int>(bad.size()));
    sim.proc.loadModel(bad.data(), static_cast<int>(bad.size()));
    sim.run(0.1);
}

bool scenarioModelError() {
    SimConfig c;
    Sim sim(c);
    bool ok = calibrate(sim);
    corruptModel(sim, 60.0f, 1.5f);
    ok &= startAnc(sim);
    sim.run(25.0);
    printStatus(sim, "modelo 60°");
    const Measure m = measureFor(sim, 4.0);
    const auto s = sim.status();
    std::printf("  modelo com 60 graus e 3,5 dB de erro: %.1f dB (correção online %+.1f dB / %+.0f graus)\n",
                m.red1(), s.harm[0].corrDb, s.harm[0].corrDeg);
    ok &= expect(s.harm[0].verified, "verificação online confirmou/corrigiu o modelo");
    ok &= expect(m.red1() < -20.0f, "cancela apesar do erro de modelo");
    return ok;
}

bool scenarioHighLatency() {
    SimConfig c;
    c.outHwDelay = 12000;  // ~330 ms de ida e volta: multimídias mais lentas
    Sim sim(c);
    bool ok = calibrate(sim);
    ok &= startAnc(sim);
    sim.run(30.0);
    printStatus(sim, "latência alta");
    const Measure m = measureFor(sim, 5.0);
    std::printf("  latência total %.0f ms: %.1f dB\n", sim.status().latencyMs, m.red1());
    ok &= expect(m.red1() < -20.0f, "cancela com latência de ~330 ms");
    return ok;
}

bool scenarioRpmWobble() {
    SimConfig c;
    Sim sim(c);
    sim.rpmProfile = [](double t) { return 2000.0 + 20.0 * std::sin(2.0 * M_PI * t / 5.0); };
    bool ok = calibrate(sim);
    ok &= startAnc(sim);
    sim.run(25.0);
    printStatus(sim, "oscilação");
    const Measure m = measureFor(sim, 5.0);
    std::printf("  rotação oscilando +-20 rpm a cada 5 s: %.1f dB\n", m.red1());
    ok &= expect(m.red1() < -12.0f, "acompanha a rotação variando");
    return ok;
}

bool scenarioLoudMusic() {
    SimConfig c;
    c.music = 0.08f;  // música mais alta que o próprio ronco
    Sim sim(c);
    bool ok = calibrate(sim);
    ok &= startAnc(sim);
    sim.run(25.0);
    printStatus(sim, "música alta");
    const Measure m = measureFor(sim, 5.0);
    std::printf("  com música alta: %.1f dB\n", m.red1());
    ok &= expect(m.red1() < -12.0f, "música não atrapalha o cancelamento do ronco");
    return ok;
}

bool scenarioWrongModel() {
    SimConfig c;
    Sim sim(c);
    bool ok = calibrate(sim);
    // Modelo com fase invertida: o pior caso possível.
    corruptModel(sim, 180.0f, 1.0f);
    ok &= startAnc(sim);
    float worst = -100.0f;
    for (int i = 0; i < 12; ++i) {
        const Measure m = measureFor(sim, 5.0);
        worst = std::max(worst, m.red1());
        if (sim.status().mode != 1) break;
    }
    printStatus(sim, "modelo errado");
    const auto s = sim.status();
    std::printf("  pior aumento do ronco: %+.1f dB, divergências=%d, modo final=%s erro=%d\n", worst,
                s.divergences, modeName(s.mode), s.error);
    ok &= expect(sim.maxOut <= 0.26f, "saída limitada mesmo com modelo errado");
    ok &= expect(worst < 8.0f, "proteção evita aumento grande do ronco");
    ok &= expect(s.mode == 0 && s.error == AncProcessor::kErrUnstable, "desiste e avisa (erro de instabilidade)");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    struct Entry {
        const char* name;
        bool (*fn)();
    };
    const Entry all[] = {
        {"calibracao", scenarioCalibration}, {"cruzeiro", scenarioCruise}, {"rampa", scenarioRamp},
        {"xrun", scenarioXrun},              {"deriva", scenarioDrift},    {"volume", scenarioVolume},
        {"erro_modelo", scenarioModelError}, {"latencia_alta", scenarioHighLatency},
        {"oscilacao", scenarioRpmWobble},    {"musica_alta", scenarioLoudMusic},
        {"modelo_errado", scenarioWrongModel},
    };
    int failures = 0, runs = 0;
    for (const auto& e : all) {
        if (argc > 1 && std::strcmp(argv[1], e.name) != 0) continue;
        std::printf("== %s\n", e.name);
        const bool ok = e.fn();
        ++runs;
        if (!ok) ++failures;
        std::printf("-> %s\n\n", ok ? "PASSOU" : "FALHOU");
    }
    std::printf("%d/%d cenários passaram\n", runs - failures, runs);
    return failures == 0 && runs > 0 ? 0 : 1;
}
