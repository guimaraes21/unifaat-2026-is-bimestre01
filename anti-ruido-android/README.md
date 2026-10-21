# Anti-Ruído — ANC para multimídia Android (Honda Accord G8)

App Android que tenta repor o **cancelamento ativo de ruído** de fábrica do Accord de 8ª
geração, que se perde ao trocar o rádio original por uma multimídia Android. Ele usa o
microfone da multimídia para achar o **ronco tonal do motor** e toca pelos alto-falantes
uma onda em oposição de fase **só nessa frequência e nas harmônicas**.

> **Status:** protótipo. O DSP foi validado num simulador (11 cenários, todos passando) e
> o APK é compilado automaticamente, mas **ainda não foi testado numa multimídia real**.
> Leia a [reflexão](docs/03-reflexao.md) antes de criar expectativa.

## Documentação

1. [Como funciona o ANC](docs/01-como-funciona-o-anc.md): física, ordens do motor, FxLMS e
   o que a Honda faz.
2. [APK e áudio no Android](docs/02-apk-e-audio-no-android.md): o que tem dentro do APK,
   latência e particularidades das centrais chinesas.
3. [Reflexão](docs/03-reflexao.md): por que "inverter o microfone" não funciona, o que
   funciona e o que esperar.
4. [Arquitetura](docs/04-arquitetura.md): algoritmos, proteções e resultados do simulador.

## Baixar o APK

A cada push, o GitHub Actions (workflow **Anti-Ruído Android**) roda o simulador e gera os
APKs:

1. Abra a aba **Actions** do repositório e clique na execução mais recente do workflow
   *Anti-Ruído Android* com ✅.
2. Em **Artifacts**, baixe `anti-ruido-apk` (um .zip) e extraia.
3. Use `app-release.apk`. O `app-debug.apk` é igual, mas com depuração ativada.

Os dois são assinados com a mesma chave de *sideload* (`app/sideload.keystore`), então dá
para atualizar por cima sem perder a calibração. A chave é pública: serve só para uso
pessoal, não publique o app com ela.

## Instalar na multimídia

1. Copie o APK para um pendrive, abra no gerenciador de arquivos da central e permita
   "instalar de fontes desconhecidas" quando o Android pedir.
   Alternativa: `adb install -r app-release.apk`.
2. Abra **Anti-Ruído** e aceite a permissão de microfone (e de notificações, se aparecer).
3. Fale perto do microfone: o nível e o espectro na tela devem reagir. Se não reagirem, sua
   central provavelmente liga o microfone só ao módulo Bluetooth, e o app não tem como
   ouvir. Tente trocar o "tipo de captura do microfone" nas configurações.

## Usar

1. **Calibrar** (uma vez): carro parado, motor desligado (ACC), portas fechadas, sem música,
   volume no nível de sempre. São ~40 s de chiado e tons graves.
2. **Ligar ANC** com o motor ligado, de preferência em cruzeiro. A cada vez que liga, o app
   solta um chiado curto (0,8 s) para conferir latência e volume.
3. Acompanhe na tela: frequência do ronco, redução em dB por harmônica, espectro com e sem
   ANC (laranja = estimado sem ANC, azul = microfone).
4. Mudou equalizador, fader ou o lugar do microfone? Calibre de novo.

Configurações úteis:

- **Harmônicas:** 2 é um bom começo; 3–4 se o espectro mostrar picos múltiplos do ronco.
- **Volume máximo da anti-onda:** se a redução ficar travada e a saída "no limitador",
  aumente aos poucos (cuidado com alto-falantes pequenos).
- **Motor + OBD-II:** opcional. Com um adaptador ELM327 Bluetooth pareado, o app usa a
  rotação para procurar o ronco só nas ordens certas (4 cilindros: 2ª; V6: 3ª; V6 com VCM:
  1,5ª, 2ª e 3ª). Muitas centrais não deixam apps usarem o Bluetooth; nesse caso o app
  funciona só pelo microfone.
- **Microfone:** a melhor posição é no forro do teto, entre as cabeças, onde a Honda instala
  o microfone do ANC original.

## Compilar localmente

Requer Android Studio (ou SDK + NDK 27 + CMake 3.22.1) e JDK 17:

```
cd anti-ruido-android
./gradlew assembleRelease      # APK em app/build/outputs/apk/release/
```

Testar só o DSP, sem Android (precisa de CMake e um compilador C++17):

```
cmake -S dsp-test -B build-dsp -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp -j && ./build-dsp/anc_sim
```

## Estrutura

```
anti-ruido-android/
├── app/src/main/cpp/
│   ├── dsp/              núcleo de DSP em C++ puro (testado no simulador)
│   ├── AncEngine.*       áudio full-duplex com Oboe
│   └── jni_bridge.cpp    ponte para o Kotlin
├── app/src/main/java/br/com/antiruido/
│   ├── MainActivity.kt   tela
│   ├── AncService.kt     serviço em 1º plano (mantém o ANC com outros apps abertos)
│   ├── audio/            JNI, status, armazenamento da calibração
│   ├── obd/              leitor de RPM via ELM327
│   ├── settings/         configurações
│   └── ui/               espectro e textos de status
├── dsp-test/anc_sim.cpp  simulador da cabine + pipeline de áudio do Android
└── docs/                 estudo, reflexão e arquitetura
```

## Limitações conhecidas

- Cancela só ruído **tonal e estável** (ronco do motor, VCM). Pneu, vento e vozes ficam
  como estão, e buzina e sirene também, o que é bom.
- A redução é local, perto do microfone, e cai durante acelerações fortes.
- Rádio FM que não passa pelo áudio do Android pode não ser mixado com a anti-onda.
- Depende de a central liberar o microfone para apps e de os alto-falantes reproduzirem a
  frequência do ronco (portas costumam ser fracas abaixo de 40–50 Hz).
