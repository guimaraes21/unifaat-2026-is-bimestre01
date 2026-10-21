# 3. Reflexão: dá para fazer isso num app? O que esperar?

## A ideia original não funciona, e o motivo ensina o caminho

A primeira ideia era "pegar o som do microfone e tocar invertido". Com a latência de uma
multimídia Android (80–300 ms) isso falha por dois motivos:

1. **Fase.** A 66 Hz (4 cilindros a 2000 rpm), 120 ms de atraso equivalem a 8 períodos
   inteiros, mais um pedaço. A onda invertida chega "torta" em relação ao ruído real:
   soma numas frequências e subtrai em outras (filtro pente). Para cancelar 10 dB seria
   preciso acertar o tempo com folga de ~0,5 ms.
2. **Realimentação.** O microfone ouve o próprio alto-falante. Microfone invertido com ganho
   e atraso é a receita da microfonia (aquele apito do show), só que grave.

O que salva o projeto é o ronco do motor ser **periódico e previsível**. Não precisamos
reagir ao som. Sintetizamos senoides na frequência do ronco e ajustamos amplitude e fase
(FxLMS) até o microfone parar de ouvi-lo. A latência deixa de ser um impeditivo e vira um
número a medir (calibração) e a compensar. É o mesmo princípio do ANC original da Honda,
que também é de banda estreita e guiado pela rotação.

## O que muda em relação ao sistema original

| | Honda (rádio original) | Este app |
|---|---|---|
| Latência | ~1 ms (DSP dedicado) | 80–300 ms (Android) |
| Referência de frequência | pulso de rotação da injeção | o próprio microfone (FFT + PLL), ou RPM do OBD-II |
| Microfone | no teto, posição projetada | o da multimídia (melhor reposicionar) |
| Adaptação | rápida, acompanha acelerações | 0,5–1,5 s; não acompanha acelerações fortes |
| Caminho secundário | fixo, de fábrica | medido pelo app (calibração) e conferido em operação |

## Expectativa honesta

- **Funciona em:** ronco tonal em regime estável (cruzeiro na estrada, ronco do VCM), perto
  do microfone.
- **Não faz nada com:** ruído de pneu, vento e chuva (banda larga, imprevisível), vozes,
  buzina e sirene. Isso é uma vantagem de segurança.
- **Atenuação:** no simulador o tom cai de 20 a 40 dB no microfone. No carro espere bem
  menos, algo como 5 a 15 dB no tom, e só na região perto do microfone. As causas: o
  campo sonoro real da cabine, várias ordens do motor, carga variando, alto-falantes de
  porta que reproduzem mal abaixo de 40–50 Hz e o limitador de segurança.
- **Durante acelerações** o app perde a frequência por alguns segundos e reaprende depois.

## Os riscos que o projeto trata

| Risco | O que o app faz |
|---|---|
| A latência muda ao reabrir o áudio | rajada curta de ruído a cada vez que o ANC liga, comparada com a "impressão digital" salva na calibração |
| xrun (falha de buffer) | o Android conta os xruns; o app re-mede, até 6 vezes por minuto |
| Volume mudou depois da calibração | observa o volume do Android e re-mede; a rajada também mede o **ganho** em bandas de 1/3 de oitava |
| Equalizador, fader ou acústica mudaram | regressão online entre o erro do microfone e a anti-onda durante a convergência; corrige o modelo em até ±80° e ±9,5 dB |
| Microfone num clock diferente (deriva) | a calibração mede o atraso antes e depois da varredura; a deriva estimada é compensada continuamente e reconferida a cada 2 min |
| Modelo muito errado, risco de amplificar o ronco | limitador de amplitude, proteção que zera a harmônica se o erro passar de 1,5× o nível sem ANC, e desligamento com aviso após 3 divergências em 1 minuto |
| Música alta | a correlação é só na frequência do ronco; o resto do áudio vira ruído de medida, que se dilui na média |
| Estalos ao ligar e desligar | rampas de 30 ms a 0,5 s em todas as transições |

## O que eu não consegui verificar daqui

O DSP foi testado num **simulador** que modela buffers, xruns, deriva de clock, alto-falante
sem graves, modo acústico da cabine, música e ruído de pista (`dsp-test/`, 11 cenários).
O APK é compilado no GitHub Actions. **Não testei numa multimídia real.** Os pontos que só
o carro responde:

1. se a sua central deixa apps usarem o microfone (a tela mostra o nível do microfone e o
   espectro: se mexer quando você fala, está ok);
2. a latência real e se ela fica estável (aparece na tela depois da calibração);
3. quanto os alto-falantes reproduzem na frequência do seu ronco (veja quantos pontos da
   calibração ficaram válidos abaixo de 60 Hz);
4. onde o microfone fica. Se a central usa um microfone com fio (P2), vale levá-lo para o
   forro do teto entre as cabeças. É lá que a Honda põe os do ANC original. Se for
   aproveitar o microfone original do teto, confirme a pinagem no manual de serviço antes
   de ligar qualquer coisa.

## Segurança e bom senso

- Comece com volume moderado. O limitador padrão é 25% do fundo de escala, e graves fortes
  por tempo longo podem castigar alto-falantes pequenos.
- Não configure nada com o carro em movimento.
- Se ouvir um tom grave crescendo, desligue: é o sintoma de modelo errado. O app desliga
  sozinho, mas não confie cegamente.
