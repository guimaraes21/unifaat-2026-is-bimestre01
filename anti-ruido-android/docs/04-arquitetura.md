# 4. Arquitetura do app

```
┌──────────────────────── Kotlin ────────────────────────┐   ┌──────────────── C++ (NDK) ───────────────┐
│ MainActivity  tela, espectro, configurações            │   │ AncEngine (Oboe)                          │
│      │ intents                                         │   │   callback da saída:                      │
│ AncService   serviço em 1º plano                       │   │     processor.render(anti-onda)           │
│      ├─ salva/carrega a calibração (ModelStore)        │JNI│     lê o microfone sem bloquear           │
│      ├─ observa volume e saída de áudio ───────────────┼──►│     processor.capture(microfone)          │
│      ├─ ObdRpmReader (ELM327, opcional) → dicas de Hz  │   │     confere xruns                         │
│      └─ notificação com Ligar/Desligar                 │   │   thread de trabalho: correlação, reabrir │
└────────────────────────────────────────────────────────┘   │ dsp/ (sem Android, testado no simulador)  │
                                                             │   AncProcessor  modos e re-medições       │
                                                             │   Calibrator    rajada + varredura        │
                                                             │   AncController FxLMS + PLL + proteções   │
                                                             │   ToneTracker   FFT do ronco              │
                                                             └───────────────────────────────────────────┘
```

## Índices em vez de relógio

O motor conta as amostras escritas (`n`) e as lidas (`m`). A anti-onda escrita na amostra `n`
aparece no microfone na amostra `m = n + D`. **D**, a latência total, é o que a calibração
mede. Enquanto não houver xrun, D é constante, mesmo com buffers grandes. O controlador
guarda o histórico da fase do oscilador e da anti-onda num anel de ~1 s, então sempre sabe
"o que estava tocando D amostras atrás".

## Calibração (`Calibrator`)

1. **Rajada de ruído branco** (0,8 s) e **GCC-PHAT**: correlação cruzada com o espectro
   normalizado, que dá um pico estreito mesmo com alto-falante e cabine colorindo o som. O
   pico é D. Um trecho da correlação em volta do pico fica salvo como **impressão digital**.
   O mesmo espectro cruzado também fornece |H| em 10 bandas de 1/3 de oitava (100–800 Hz).
2. **Varredura de 65 senoides** de 20 a 320 Hz (16 por oitava). Cada uma toca até
   estabilizar e depois é medida por **lock-in**: correlação com a própria referência, em
   janela de número inteiro de períodos. Disso sai `H(f)`. O app guarda
   `H0(f) = H(f)·e^{+j2πfD/fs}`: sem o atraso bruto, a fase varia devagar e pode ser
   interpolada entre os pontos.
3. **Segunda rajada**: compara com a impressão digital. Se D mudou, ou é deriva de clock
   (estima a taxa e corrige a fase de cada ponto da varredura) ou é xrun (descarta e avisa).

**A cada vez que o ANC liga**, só a rajada (0,8 s, nível baixo) é repetida. A impressão
digital dá o novo D, que muda a cada abertura do áudio, e as bandas dão o ganho atual
(volume).

## Controle (`AncController`)

Para cada harmônica k = 1…4 da fundamental `f0`:

```
saída:     y(n)   = Re{ W_k · e^{j·2π·k·φ(n)} }                φ = fase do oscilador (ciclos)
modelo:    Ĥ_k    = H0(k·f0) · ganho_da_sessão · correção_online
erro:      e(m)   = microfone (com o DC removido)
FxLMS:     W_k   ← W_k·(1−λ) − μ_k · e(m) · conj( Ĥ_k · e^{j·2π·k·φ(m−D)} )
passo:     μ_k    = 2·α / ((D + 480) · |Ĥ_k|²)                 α = "velocidade" (0,2)
```

A normalização faz o ganho de malha vezes atraso valer α para qualquer latência. Isso dá
margem para ~70° de erro de fase e ~3× de erro de ganho.

**Frequência**, em três camadas:

1. `ToneTracker`: FFT de 1024 pontos sobre o sinal decimado para ~1 kHz, a cada 0,25 s, com
   interpolação parabólica. Prefere uma sub-harmônica forte (ex.: 1,5ª ordem do VCM em vez
   da 3ª). Com OBD-II, a busca fica restrita a ±4% das ordens do motor.
2. **Saltos** (troca de marcha, VCM) acima de 1,5 Hz ou 3% exigem duas estimativas
   concordando; então o oscilador pula e os pesos zeram.
3. **PLL tipo 2** sobre a fase do ruído estimado `d̂ = e − Ĥ·anti-onda`: mantém o ronco
   parado em relação ao oscilador, de modo que o LMS (lento por causa da latência) só
   acompanha amplitude. A banda cai com a latência (`ωn = min(2,5; 0,3/(D/fs + 0,06))`).

**Verificação online do modelo.** Após cada reinício, a anti-onda parte de zero e converge.
Nessa janela (~2 constantes de tempo do LMS) o app ajusta por mínimos quadrados
`E = D0 + D1·t + H·W̃`, em que `E` é o erro demodulado e `W̃` a anti-onda que chegou ao
microfone. Isso mede o H **verdadeiro** naquela frequência, independente do modelo.
Condições para aceitar: rotação estável na janela (variação < 0,15 Hz), R² > 0,9, erro-padrão
< 10% e correção dentro de ±80° e ±9,5 dB. A PLL só é liberada depois dessa janela, para não
ser puxada pela própria anti-onda quando o modelo está errado.

**Proteções:**

- **Limitador:** a soma de |W_k| nunca passa do máximo configurado.
- **Linha de base:** cada harmônica nasce com um "descanso" em silêncio (0,25 s + D) que
  mede o erro sem anti-onda.
- **Divergência:** se o erro passar de 1,5× a linha de base, ou de 2× a estimativa do ruído,
  por 1 s, a harmônica zera, descansa 1 s, o passo cai pela metade e a correção é
  descartada.
- **Desistência:** com 3 divergências em 60 s o app re-mede a latência uma vez; se repetir,
  desliga e mostra o erro.

## Orquestração (`AncProcessor`)

Modos: **Monitor** (só escuta e mostra espectro), **Calibrar**, **Medir latência** e **ANC**.
Toda troca passa por rampa de 30 ms; a anti-onda entra em rampa de 0,5 s. Gatilhos de nova
medição durante o ANC:

| Gatilho | Origem |
|---|---|
| xrun | `getXRunCount` no callback |
| volume ou saída de áudio mudou | `AncService` (AudioManager, AudioDeviceCallback) |
| 1ª divergência após 30 s da última medição | controlador |
| a cada 2 min, se houver deriva de clock | taxa estimada entre medições |

## Simulador (`dsp-test/anc_sim.cpp`)

Pipeline modelado: callback de saída de 20 ms puxando um buffer de 80 ms, +50 ms de
"hardware" de saída e +30 ms de entrada, microfone entregue em blocos de 20 ms,
alto-falante com corte em 45 Hz, modo da cabine em 75 Hz (+6 dB), ronco de 66,7 Hz + 133 Hz,
ruído de pista e música.

Resultado da última execução (redução do tom no microfone, medida com a fase exata do
ronco):

| Cenário | Redução |
|---|---|
| Cruzeiro (rotação oscilando ±8 rpm) | −24,6 dB (66,7 Hz), −21,0 dB (133 Hz) |
| Rampa 1800 → 2300 rpm, depois estável | −35,3 dB |
| Underrun real de 25 ms + overrun de 5 ms (latência 82 → 102 ms) | −39,6 dB depois de re-medir |
| Deriva de clock de 30 ppm por 60 s | −40,0 dB |
| Volume +10 dB, −8 dB (avisado) e +8 dB (sem aviso) | −40,5 / −41,3 / −41,4 dB |
| Modelo salvo com 60° e 3,5 dB de erro | −39,8 dB (correção online de −56°) |
| Latência total de 282 ms | −31,7 dB |
| Rotação oscilando ±20 rpm a cada 5 s | −17,9 dB |
| Música mais alta que o ronco | −20,7 dB |
| Modelo com fase invertida (pior caso) | pior momento +5,5 dB; desliga com aviso |

Rodar localmente:

```
cmake -S dsp-test -B build-dsp -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp -j
./build-dsp/anc_sim            # todos
./build-dsp/anc_sim xrun       # um cenário
```

O simulador é otimista por natureza: campo sonoro de um ponto só, um caminho só, ronco
perfeitamente senoidal. Ele serve para pegar bugs de lógica e de estabilidade, não para
prever a redução no carro.
