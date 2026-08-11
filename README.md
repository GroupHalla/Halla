
<p align="center">
  <img src="src/assets/halla-logo.png" width="120" alt="Halla" />
</p>

<h1 align="center">Halla</h1>

<p align="center">
  Cliente de comunicação por voz para desktop (Windows, Linux), escrito em
  <b>C++17</b> e <b>Qt 6 Widgets</b>.
</p>

<p align="center">
  <img src="https://i.imgur.com/XAjDMvm.png" width="720" alt="Janela principal do Halla" />
</p>

---

## Índice

- [Visão geral](#visão-geral)
- [Capturas de tela](#capturas-de-tela)
- [Principais recursos](#principais-recursos)
- [Arquitetura do projeto](#arquitetura-do-projeto)
- [Protocolo de rede](#protocolo-de-rede)
- [Áudio e voz](#áudio-e-voz)
- [Estrutura de código](#estrutura-de-código)
- [Compilando](#compilando)
- [Projetos relacionados](#projetos-relacionados)
- [Licença](#licença)

---

## Visão geral

O **Halla** é um cliente de VoIP (voz sobre IP) para comunidades e servidores
privados, no mesmo espírito do TeamSpeak 3 clássico: você entra num servidor,
navega por uma árvore de canais, conversa por voz e por texto, e tudo é
administrável por um sistema de grupos e permissões granulares.

Ele se conecta a um servidor da família **Halla Server** (protocolo próprio,
não o protocolo proprietário do TeamSpeak) via **TCP** (controle/JSON) e
**UDP** (voz, codec Opus).

O projeto **não depende de nenhum recurso visual externo** — todos os ícones
da interface (avatares, símbolos de canal, cadeados, indicadores de fala, etc.)
são desenhados em tempo de execução com `QPainter` (veja `src/gui/Icons.cpp`),
o que deixa o executável leve e os ícones nítidos em qualquer resolução/DPI.

## Capturas de tela

| Janela principal | Opções — Capturar | Opções — Teclas de atalho |
|---|---|---|
| ![main](https://i.imgur.com/XAjDMvm.png) | ![capture](https://i.imgur.com/JOIdczm.png) | ![hotkeys](https://i.imgur.com/POsrBRD.png) |

| Conectar | Opções — Sussurro |
|---|---|---|
| ![connect](https://i.imgur.com/pn5oL8q.png) | ![whisper](https://i.imgur.com/QBUwQCL.png) |

## Principais recursos

**Voz**
- Push-to-talk (tecla **ou botão do mouse**, inclusive botões laterais/extras),
  detecção de atividade de voz (VAD) com sensibilidade ajustável, ou
  transmissão contínua.
- Codecs: Opus Voice/Music, além dos legados Speex e CELT (compatibilidade de
  protocolo), com controle de bitrate e qualidade por canal.
- Processamento de sinal: cancelamento de eco, remoção de ruído de fundo,
  atenuação de digitação, redução de eco ("ducking") ao ouvir outros falarem,
  e medidor de volume em tempo real com limiar visual.
- **Sussurro**: fale só para um canal específico, canal + subcanais, ou uma
  lista fixa de usuários — com indicador visual próprio (círculo laranja no
  avatar) distinto do indicador normal de fala (verde).
- Gravação local (WAV) de chamadas, própria voz + participantes.

**Canais e usuários**
- Árvore de canais com subcanais, canais temporários/semi-permanentes/
  permanentes, canais protegidos por senha, canais moderados e vinculados
  (áudio compartilhado entre canais "linkados").
- Grupos de servidor e de canal com permissões granulares (grade de
  permissões com filtro e modo avançado de "conceder"), talk power,
  operador de canal, comandante.
- Avatares, descrições com BBCode/emoji, "cutucar" (poke), reclamações,
  mensagens offline, transferência de arquivos, lista de banidos.

**Interface**
- Tema claro/escuro trocável em tempo real, sem precisar reiniciar.
- Chat com abas por servidor/canal, BBCode (`[b] [i] [u] [color=] [size=] [url=]`)
  e emojis.
- Marcadores (bookmarks) de servidores, conexões recentes, múltiplas
  identidades locais, perfis de captura/reprodução.
- Bandeja do sistema, notificações sonoras (pacote de sons próprio gerado na
  primeira execução) e narração por texto-para-voz (`QTextToSpeech`) dos
  eventos ("Fulano entrou", "Você foi cutucado").
- Teclas de atalho totalmente configuráveis — inclusive com botões de mouse,
  capturados em múltiplas camadas para não depender só do evento clássico do
  Windows (útil com softwares de mouse gamer que interceptam os botões
  laterais).

## Arquitetura do projeto

```
                     ┌───────────────────┐
                     │     MainWindow    │  janela principal, menus, abas
                     └─────────┬─────────┘
                               │
                 ┌─────────────┴─────────────┐
                 │         ServerTab          │  uma aba = uma conexão
                 │  (árvore + chat + info)    │
                 └───┬─────────────┬──────────┘
                     │             │
             ┌───────┴───┐   ┌─────┴──────┐
             │NetSession │   │ VoiceEngine │
             │ TCP (JSON)│   │ UDP (Opus)  │
             └───────────┘   └─────────────┘
                     │             │
                     └──────┬──────┘
                             ▼
                     Halla Server (self-hosted)
```

- **`NetSession`** mantém a conexão TCP de controle e um `ServerData`
  (`src/core/Models.h`) sempre sincronizado com o estado que o servidor manda
  — usuários, canais, permissões, etc. Toda mudança dispara sinais Qt que a
  UI escuta para se redesenhar.
- **`VoiceEngine`** cuida só do áudio: captura o microfone a cada 20 ms,
  codifica em Opus e manda por UDP; do outro lado, decodifica, desenjitteriza
  (fila por remetente) e mixa para os alto-falantes. Ele degrada graciosamente
  se não houver dispositivo de áudio disponível.
- **`ServerTab`** é a aba de uma conexão: junta a árvore de canais
  (`ServerTreeWidget`), o chat (`ChatPanel`) e o painel de informações
  (`InfoPanel`), e é quem liga os sinais do `NetSession`/`VoiceEngine` à
  interface (inclusive a lógica de PTT/sussurro "segurar tecla").
- **`OptionsDialog`** replica a janela de Opções clássica, com navegação
  lateral por categoria (Aplicativo, Reprodução, Capturar, Aparência,
  Notificações, Teclas de atalho, Sussurro, Segurança, Complementos).

## Protocolo de rede

Especificado em `src/net/HallaProtocol.h` (compartilhado, byte a byte igual,
com o `HallaServer`):

- **Controle (TCP)**: mensagens JSON compactadas, uma por linha (`\n` como
  delimitador). Cada mensagem tem um campo `"t"` com o tipo (`"talking"`,
  `"whisper"`, `"user_state"`, etc.).
- **Voz (UDP)**: pacotes binários com um "magic" de 4 bytes (`"HALL"`),
  seguido de um identificador (token do cliente ou ID de quem fala), um
  número de sequência de 16 bits e o payload Opus.
- Porta padrão: **9987/tcp+udp**.
- Protocolo versionado (`kProtoVersion` / `kProtoMin`): o servidor aceita
  clientes de versões antigas, mas recursos novos (permissões granulares,
  banlist e grupos por UID exigem v3; o protocolo v4 adiciona credencial UDP
  CSPRNG de 128 bits e distribuição de ICE/TURN pelo servidor.

## Áudio e voz

- Captura/reprodução via `QAudioSource`/`QAudioSink` (Qt Multimedia), a 48 kHz
  mono, quadros de 20 ms.
- Codificação/decodificação Opus (biblioteca `libopus` — estática no Windows,
  do sistema no Linux).
- Um "jitter buffer" por remetente (fila de quadros) suaviza variações de
  chegada dos pacotes antes de mixar no alto-falante.
- PTT e sussurro são "hold keys": o app monitora periodicamente (tecla ou
  botão do mouse) se a tecla configurada está fisicamente pressionada,
  inclusive via captura global no Windows, para funcionar mesmo com o Halla
  em segundo plano.

## Estrutura de código

```
src/
├── app/            MainWindow (janela/menus), Theme (claro/escuro),
│                   SoundPack (sons), Speech (TTS)
├── core/           Models.h (dados de sessão), Settings.h (config
│                   persistente), AppLog (registro de eventos)
├── net/            NetSession (TCP/controle), VoiceEngine (UDP/áudio),
│                   HallaProtocol.h (protocolo compartilhado com o servidor)
├── gui/            ServerTab, ServerTreeWidget, ChatPanel, InfoPanel,
│                   HotkeyEdit, Icons (ícones desenhados em código),
│                   WelcomePage, TsBanner, RichTextBrowser
├── dialogs/        OptionsDialog, ConnectDialog, ChannelDialog,
│                   GroupsDialog, IdentityDialog, BookmarksDialog,
│                   AdminDialogs (banlist/reclamações/grupos/permissões),
│                   ToolsDialogs (sussurro/contatos/transferência de
│                   arquivos), MiniDialogs (poke/kick-ban/volume/etc.),
│                   LogDialog, AboutDialog
├── assets/         logo e ícones vetoriais da árvore
└── main.cpp        ponto de entrada (Qt Application, tema, argumentos)
```

## Compilando

### Dependências

- CMake ≥ 3.21
- Compilador com C++17
- Qt 6.2+ com os módulos **Widgets**, **Network**, **Multimedia** e
  **TextToSpeech**
- **libopus** (no Windows, uma build estática em `third_party/opus/`; no
  Linux/macOS, a versão do sistema via `pkg-config`)

### Linux

```bash
./build-linux.sh        # instala cmake/ninja/qt6-base-dev se faltar
./build/Halla
```

### Windows / manual (qualquer plataforma)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

No Windows, o CMake também embute o ícone e as informações de versão do
executável (`src/halla.rc`) e monta o instalador NSIS (`packaging/halla-setup.nsi`).

## Projetos relacionados

- **[Halla Server](https://github.com/GroupHalla/HallaServer)** — servidor
  auto-hospedável (C++/Qt) que fala o mesmo protocolo.
- **[Halla Mobile](https://github.com/GroupHalla/Halla-Mobile)** — cliente móvel (Qt Quick/QML) com conexão dinâmica,
  lista de canais e chat integrado.

## Licença

Distribuído sob os termos do contrato de licença de usuário final em
[`packaging/LICENSE.txt`](packaging/LICENSE.txt).
