# 2. O que é um APK e como o áudio anda dentro de uma multimídia Android

## O APK por dentro

Um APK é um arquivo ZIP com estrutura fixa:

```
app-release.apk
├── AndroidManifest.xml      permissões, Activity, Service (binário)
├── classes.dex              código Kotlin compilado para a máquina virtual do Android (ART)
├── lib/
│   ├── arm64-v8a/libantiruido.so   código C++ nativo (o DSP), um por arquitetura
│   ├── armeabi-v7a/...             multimídias mais antigas, 32 bits
│   └── x86_64/...                  emulador
├── res/ + resources.arsc    telas, textos, ícones
└── META-INF/ (+ bloco de assinatura v2/v3)
```

Como é gerado (o Gradle faz tudo):

1. **Kotlin → bytecode JVM → D8 → DEX** (`classes.dex`).
2. **C++ → NDK/CMake → `.so`** para cada ABI. O DSP é em C++ porque o laço roda 48 000
   vezes por segundo, sem pausas de coleta de lixo.
3. **aapt2** compila os recursos.
4. **Assinatura.** Todo APK precisa ser assinado. O Android só aceita atualizar um app se a
   nova versão tiver a mesma chave. Por isso o projeto usa uma chave fixa de *sideload*:
   assim dá para atualizar sem perder a calibração.

Para instalar numa multimídia:

- copie o `.apk` para um pendrive e abra com o gerenciador de arquivos, permitindo
  "fontes desconhecidas" quando o Android pedir; ou
- use `adb install -r app-release.apk` com a depuração USB ativada.

## Particularidades das multimídias Android chinesas

Essas centrais costumam ter dois "cérebros":

- **SoC Android** (Unisoc UIS7862/7870, MediaTek, Rockchip PX5/PX6): roda os apps.
- **MCU**: liga e desliga com a chave (ACC), lê os comandos do volante via canbus e
  controla o rádio FM e, às vezes, o **processador de áudio** (o chip que mistura fontes,
  aplica equalizador e fader e alimenta o amplificador).

Consequências práticas para este projeto:

| Situação | Efeito |
|---|---|
| Microfone ligado só ao módulo Bluetooth | o Android não "ouve" o microfone e o app avisa que não abriu a captura |
| Rádio FM tocando pelo caminho analógico do MCU | a anti-onda (que sai pelo Android) pode não ser mixada com o rádio; use música por Spotify, pendrive etc. |
| Equalizador, loudness e fader no processador de áudio | entram no "caminho secundário": mude-os e recalibre |
| Volume pelo botão físico | normalmente muda o volume do Android (STREAM_MUSIC); o app percebe e re-mede |
| Modo "sono" no ACC desligado | o app pode ser encerrado; abra de novo (ou marque "Ligar o ANC ao abrir o app") |

## O caminho do áudio no Android

```
 App (anti-onda) ─► Oboe/AAudio ─► AudioFlinger (mixer, junta com a música) ─► HAL ─►
   codec/I2S ─► processador de áudio da central (EQ, fader) ─► amplificador ─► alto-falante
                                                                                    │ ar
 App (DSP) ◄─ Oboe/AAudio ◄─ AudioFlinger (captura) ◄─ HAL ◄─ ADC ◄─ microfone ◄────┘
```

Cada seta tem buffers. A **latência de ida e volta** típica:

| Aparelho | Latência |
|---|---|
| Celular topo de linha (AAudio MMAP) | 10–20 ms |
| Celular comum | 30–80 ms |
| Multimídia chinesa | 80–300 ms |

Detalhes que importam para o ANC:

- **Pré-processamento do microfone.** Os modos "chamada de voz" aplicam cancelamento de
  eco, supressão de ruído e controle automático de ganho. Isso destrói a relação entre o
  que o app toca e o que ele ouve. O app usa o modo *VoiceRecognition* (que pela norma do
  Android não tem AGC) e oferece *UNPROCESSED*.
- **xrun.** Se o app atrasa ao entregar áudio (*underrun*) ou deixa o microfone
  transbordar (*overrun*), a relação entre amostra escrita e amostra lida muda alguns
  milissegundos. O app detecta isso (`getXRunCount`) e re-mede a latência.
- **Modo compartilhado.** A saída abre em modo *shared* para a música dos outros apps
  continuar tocando; o mixer do Android soma tudo. O app não pede *audio focus*, para não
  pausar o player.
- **Oboe** é a biblioteca do Google sobre o AAudio (Android 8.1+) com volta automática para
  o OpenSL ES em aparelhos antigos.

## Permissões e serviço em primeiro plano

- `RECORD_AUDIO`: microfone (pedida na primeira abertura).
- Serviço em primeiro plano do tipo `microphone`, com notificação fixa. Sem ele, o Android
  encerra o áudio quando você abre o mapa.
- `POST_NOTIFICATIONS` (Android 13+): mostrar a notificação.
- `BLUETOOTH_CONNECT` (Android 12+): só se usar o adaptador OBD-II.
