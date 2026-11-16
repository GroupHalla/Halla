# Halla 1.1.38 — notas da versão

> **Correções severas.** Esta versão conserta falhas graves encontradas em
> auditoria no áudio, no compartilhamento de tela, no E2EE, nos complementos e
> na interface. Recomendamos atualizar o quanto antes.

## Áudio (voz) — o mais crítico

- **A supressão de ruído embutida (RNNoise) voltou a funcionar de verdade.**
  O caminho sem o SDK do WebRTC alimentava o modelo com amostras fora da
  escala esperada e misturava a voz com um atraso de um quadro inventado: o
  resultado era **quase nenhum abafamento de ruído** (cerca de 1,3 dB — o
  equivalente a nada). Agora o modelo recebe o sinal na escala correta, a
  mistura é alinhada amostra a amostra e a supressão real medida é de
  **~16 dB** sobre ruído de ventoinha, mantendo a voz praticamente intacta.
- **Compensação automática de nível de microfone.** Headsets muito baixos e
  microfones USB muito altos deixavam o modelo "no escuro". Uma correção lenta
  de nível (~200 ms) leva o sinal para a faixa em que a supressão funciona e
  devolve o nível original na saída — sem modular o ganho dentro da fala e sem
  amplificar resíduo de eco quando o cancelamento está ligado.
- **Cancelamento de eco** continua validado por medição real (ERLE ~22 dB no
  portão de CI), com reinício do filtro após pausa longa do playout.
- O portão de CI do DSP passou a medir **ganho de SNR** (queda do piso de
  ruído comparada à queda do nível de fala) em vez de atenuação global de um
  ruído branco chapado — o critério antigo premiava "mutar o quadro inteiro"
  em vez de limpar o quadro. O log agora é legível em inglês.

## Compartilhamento de tela e WebRTC

- Liberação correta do XShm e permissão `0600` no arquivo de captura.
- `QPointer` + mutex na lista de pares e no encerramento da thread de captura:
  não há mais uso após liberação ao parar a transmissão.
- Correção do plano UV em NV12 (`alignedHeight`) — faixas verdes/deslocadas em
  algumas resoluções.
- Validação da fonte de captura antes de iniciar; nada de mídia em texto claro.
- Áudio do loopback PulseAudio: callback destacado e rollback correto no stop.

## E2EE e identidade

- Envelopes de chave assinados e verificação de vínculo DH/Ed25519.
- Removido o caminho de XOR de "emergência": não existe mais downgrade
  silencioso de criptografia.
- Backup de identidade e migração seguem com as mesmas restrições de ABI já
  validadas no CI (BoringSSL × OpenSSL).

## Complementos (plugins)

- Validação de pacote e staging de arquivos com rejeição de caminhos inseguros.
- Limites de tamanho explícitos (`100 MiB` / `250 MiB`) e política de
  redirecionamento `NoLessSafeRedirectPolicy`.
- `AFTER_VAD` só é honrado com `audio.capture`; nenhuma mídia é entregue sem
  permissão.

## Interface

- **Limpar cache de ícones e de log agora funciona de verdade** (antes os
  botões apenas fechavam o diálogo).
- Remoção de código morto: sinal `commanderToggled` sem uso, bloco
  Reed-Solomon morto no handshake, variável de tema não utilizada.
- Ajustes de rolagem/descrição de canal e de registro do cliente herdados da
  1.1.37 continuam valendo.

## Manutenção

- **Todas as anotações do código agora estão em inglês**, de forma
  profissional; textos de interface em português/espanhol/inglês seguem
  intactos (`tr(...)`).
- Smokes de teste reportam em inglês (`FAILED` / `RESULT: OK`) para leitura
  direta no log do CI.

## Verificação

- `tests/repo_sanity.py`, `tests/plugin_audit.py`, `tests/translation_audit.py`
  (1221 strings contextualizadas) e `tests/icon_audit.py`: OK.
- `tests/dsp_fallback_smoke.cpp`: AEC 21,87 dB · SNR de ruído 16,01 dB ·
  passthrough exato — OK.
- `tests/echo_guard_smoke.cpp`, `tests/hotkey_vk_smoke.cpp` (83 checagens) e
  `tests/radio_voice_smoke.cpp`: OK.
