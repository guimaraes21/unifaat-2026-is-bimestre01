# 1. Como funciona o cancelamento ativo de ruído (ANC)

## A ideia física

Som é variação de pressão do ar. Duas ondas no mesmo ponto **se somam**. Se uma onda
chega com a mesma amplitude e **fase oposta** (deslocada meio período, ou 180°), a soma
dá zero: é a interferência destrutiva.

```
ruído        ╱╲    ╱╲    ╱╲
            ╱  ╲  ╱  ╲  ╱  ╲
anti-ruído  ╲  ╱  ╲  ╱  ╲  ╱
             ╲╱    ╲╱    ╲╱
soma        ──────────────────   (silêncio, no ponto onde as duas se encontram)
```

O cancelamento ativo (ANC, *Active Noise Control*) faz isso de propósito. Um microfone
mede o ruído, um processador calcula a "anti-onda" e um alto-falante a toca.

## Por que só funciona bem nos graves

- **Comprimento de onda.** A 50 Hz a onda tem cerca de 6,9 m. A 1 kHz tem cerca de 34 cm. A
  região onde a anti-onda fica em fase oposta à do ruído (a "zona de silêncio") tem
  aproximadamente **1/10 do comprimento de onda** para 10 dB de redução. Isso dá ~70 cm a
  50 Hz e só ~3 cm a 1 kHz. Por isso ANC de carro e de fone trabalha abaixo de ~300 Hz.
- **Isolamento passivo.** Espuma, borracha e vidro barram bem os agudos e mal os graves.
  O ANC complementa justamente onde o isolamento falha.
- **Precisão de fase.** Um erro de fase φ deixa uma sobra de `|1 − e^{jφ}|`. Com φ = 20° a
  redução máxima é de ~9 dB, e com φ = 60° não sobra nada. Um erro de tempo τ vira erro de
  fase `φ = 360° · f · τ`. A 100 Hz, 1 ms de erro já dá 36°.

## As duas famílias de ANC

| | Realimentação (*feedback*) | Antecipação (*feedforward*) |
|---|---|---|
| Referência | o próprio microfone de erro | um sinal correlacionado com a fonte (outro microfone, rotação do motor) |
| Precisa de | latência baixíssima (µs a poucos ms) | conhecer o caminho alto-falante → microfone |
| Onde aparece | fones de ouvido | carros, dutos de ar-condicionado |

## O ruído de motor é especial: ele é periódico

O ronco do motor é a soma de senoides em **ordens** da rotação. Num motor 4 tempos com N
cilindros acontecem N/2 explosões por volta do virabrequim:

```
f_explosão = (RPM / 60) × (N / 2)
```

| Motor | Rotação | Ordem dominante | Frequência |
|---|---|---|---|
| 4 cilindros | 2000 rpm | 2ª | 66,7 Hz |
| 4 cilindros | 3000 rpm | 2ª | 100 Hz |
| V6, 6 cilindros ativos | 2000 rpm | 3ª | 100 Hz |
| V6 com VCM, 3 cilindros ativos | 1500 rpm | 1,5ª | 37,5 Hz |

Um sinal periódico é **previsível**: sabendo frequência, amplitude e fase agora, sabemos
como ele estará daqui a 100 ms. Isso permite cancelar mesmo com atraso grande no sistema.
O controlador não precisa "reagir" ao microfone. Ele sintetiza senoides e vai ajustando
amplitude e fase até o microfone parar de ouvir o tom.

## O algoritmo FxLMS de banda estreita

É o algoritmo clássico de ANC automotivo. Para cada harmônica k o controlador mantém um
número complexo `W` (amplitude e fase da anti-onda) e gera:

```
y(n) = Re{ W · e^{j·θ(n)} }          θ = fase do oscilador na frequência do ronco
```

No microfone chega `e = d + S{y}`. Aqui `d` é o ruído e `S` é o **caminho secundário**: tudo
entre o número que o app escreve e o número que ele lê de volta (mixer do Android,
amplificador, alto-falante, ar da cabine, microfone, conversores, buffers). O LMS anda na
direção que reduz `e²`:

```
W ← W − μ · e(n) · conj( Ŝ · e^{j·θ(n − D)} )
```

O "F" de *Filtered-x* é o termo `Ŝ · e^{jθ(n−D)}`: a referência passada por um **modelo** do
caminho secundário. Sem ele, o atraso D faria o LMS corrigir na direção errada. A regra
prática:

- o FxLMS converge se o erro de fase do modelo `Ŝ` for menor que ~90°;
- quanto maior a latência D, menor o passo μ que mantém a malha estável, e mais lenta a
  adaptação. Neste app usamos `μ·|Ŝ|²·D ≈ 0,2`, o que dá uma constante de tempo de
  aproximadamente 5·D (0,5 s para 100 ms de latência).

## O que a Honda faz no Accord

Nos Accord de 8ª geração que têm ANC de fábrica (principalmente os V6 com VCM, que desligam
cilindros e geram um ronco grave), o sistema:

1. tem microfone(s) no forro do teto, perto das cabeças dos ocupantes;
2. recebe a rotação do motor da central de injeção (referência *feedforward*);
3. processa dentro do rádio original e toca a anti-onda pelos alto-falantes do carro;
4. trabalha junto com os coxins ativos do motor (ACM), que cuidam da vibração.

Como o processamento está no rádio original, **trocar o rádio desativa o ANC**. É esse o
problema que este projeto tenta contornar.

## Leituras recomendadas

- S. M. Kuo e D. R. Morgan, *Active Noise Control Systems: Algorithms and DSP
  Implementations* (Wiley, 1996): FxLMS, ANC de banda estreita, caminho secundário.
- S. J. Elliott, *Signal Processing for Active Control* (Academic Press, 2001): estabilidade
  com atraso e zona de silêncio.
- Material da Honda sobre *Active Noise Cancellation* e *Active Control Engine Mount* no
  Accord V6 com VCM.
