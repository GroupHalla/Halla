# SDK de complementos do Halla Desktop

O Halla Desktop carrega plugins nativos da comunidade por uma ABI C estável,
Qt-free, declarada em [`sdk/halla_plugin_api.h`](../sdk/halla_plugin_api.h). No
Windows, cada plugin é uma DLL carregada com `QLibrary`. O cabeçalho e os
exemplos podem ser usados sob a licença permissiva de
[`sdk/LICENSE.txt`](../sdk/LICENSE.txt).

> **Segurança:** uma DLL executa no mesmo processo e com os mesmos privilégios
> do Halla. Capacidades declaradas informam ao usuário o que o complemento
> pretende fazer, mas não formam uma sandbox contra código nativo malicioso.

## Compatibilidade da ABI

A ABI-base continua sendo `HALLA_PLUGIN_ABI_VERSION 1`. Plugins produzidos com
o SDK 1.0.63 permanecem binariamente compatíveis. Recursos extensos não foram
acrescentados diretamente a uma estrutura gigante: o campo aditivo
`HallaHostApi::query_interface` fornece módulos independentes e versionados:

```text
halla.core.v1
halla.connection.v1
halla.audio.v1
halla.data.v1
halla.ui.v1
```

Antes de usar `query_interface`, confira `host->struct_size`. Um plugin deve
aceitar que uma interface ou função opcional não esteja disponível.

## Pacote `.halla-addon`

O pacote é um ZIP comum renomeado para `.halla-addon`:

```text
manifest.json
bin/windows-x64/meu_plugin.dll
assets/...
```

Exemplo avançado de manifesto:

```json
{
  "id": "com.exemplo.audio-posicional",
  "name": "Áudio posicional",
  "version": "1.0.0",
  "author": "Comunidade Halla",
  "description": "Integra posição e rádio de um jogo.",
  "type": "native",
  "apiVersion": 1,
  "defaultEnabled": false,
  "capabilities": [
    "connection.read",
    "audio.playback",
    "audio.spatial",
    "plugin.data",
    "ui.notifications",
    "ui.actions"
  ],
  "platforms": {
    "windows-x64": {
      "library": "bin/windows-x64/audio_posicional.dll"
    }
  },
  "settings": [
    {
      "key": "maxDistance",
      "type": "int",
      "label": "Distância máxima",
      "default": 60,
      "min": 5,
      "max": 1000
    }
  ]
}
```

IDs aceitam letras ASCII minúsculas, números, ponto, hífen e sublinhado. A
biblioteca deve ter caminho relativo e permanecer dentro do pacote.

O empacotador oficial cria ZIP reproduzível e checksum:

```bash
python tools/package_plugin.py pasta-do-plugin meu-plugin.halla-addon
```

## Capacidades

| Capacidade | Acesso fornecido |
|---|---|
| `connection.read` | Snapshots de conexões, servidores, usuários, canais e permissões |
| `connection.control` | Canal próprio, estados, apelido, chat, whisper, mute e volume locais |
| `audio.capture` | PCM S16 do microfone antes da codificação Opus |
| `audio.playback` | PCM de cada voz recebida e mixagem estéreo final |
| `audio.spatial` | Posição 3D, ganho, pan e filtro de rádio por usuário |
| `plugin.data` | Mensagens TLS entre instâncias do mesmo complemento |
| `ui.notifications` | Notificações do cliente |
| `ui.actions` | Ações no menu Complementos e atalhos de janela |

O instalador mostra as capacidades solicitadas antes da confirmação final.
Interfaces não declaradas retornam `NULL`; operações não autorizadas retornam
`HALLA_RESULT_PERMISSION_DENIED`.

## Entrada e ciclo de vida

A DLL exporta uma função C sem name mangling:

```cpp
extern "C" HALLA_PLUGIN_EXPORT
const HallaPluginApi* halla_plugin_entry(void);
```

O plugin fornece:

- metadados que devem corresponder ao manifesto;
- `initialize` e `shutdown`;
- `on_event` para eventos JSON;
- `on_settings_changed` para configuração atualizada sem reinicialização.

`shutdown()` deve cancelar trabalhos, remover callbacks e liberar recursos
antes de retornar. O Halla também remove processadores, handlers, ações e
estado espacial restantes antes de descarregar a biblioteca.

## Interface Core

`HallaCoreApiV1` oferece:

- relógio monotônico em milissegundos;
- informações JSON do aplicativo, plataforma e interfaces;
- `post_to_ui`, para agendar trabalho curto na thread principal.

`post_to_ui` pode ser chamado por uma thread própria do complemento. A função
agendada não deve bloquear a interface.

## Conexões, clientes e canais

`HallaConnectionApiV1` trabalha com `connection_id`. O valor zero representa a
conexão ativa. IDs permanecem estáveis durante a vida da aba.

`get_connections_json` lista todas as abas; `get_connection_json` fornece:

- servidor, endereço, versão, plataforma e ping;
- usuário local e canal atual;
- permissões efetivas;
- todos os usuários visíveis, UIDs, canais, estados de fala e mudo;
- grupos, descrição, volume/mute local e screen share;
- árvore completa de canais visíveis, codec, bitrate, vínculos e participantes.

Operações de controle incluem:

- mover o próprio usuário;
- definir mudo de entrada/saída e ausência;
- mudar apelido;
- enviar chat de canal, servidor ou privado;
- definir destinos de whisper;
- silenciar e ajustar volume de um usuário localmente;
- mover, cutucar, definir comandante, expulsar ou banir usuários;
- criar, editar e excluir canais por objetos JSON do protocolo.

O servidor continua sendo a autoridade. Permissões, hierarquia, senha e demais
regras podem recusar uma ação solicitada pelo plugin.

## Pipeline de áudio

O motor de voz usa Opus mono, 48 kHz, quadros de 20 ms e reprodução estéreo.
Cada remetente possui seu próprio decoder Opus e sua própria fila. As vozes são
processadas individualmente, espacializadas e depois mixadas com saturação.

### Callbacks PCM

`register_processor` registra uma callback nos estágios:

| Estágio | Formato |
|---|---|
| `HALLA_AUDIO_CAPTURE` | PCM S16 mono, usuário local, antes de VAD/Opus |
| `HALLA_AUDIO_REMOTE_BEFORE_SPATIAL` | PCM S16 mono de um participante após Opus |
| `HALLA_AUDIO_MIXED_PLAYBACK` | PCM S16 estéreo da mixagem final |

`HallaAudioFrame` contém conexão, usuário, amostras mutáveis, frames, canais e
sample rate. Os buffers pertencem ao Halla e são válidos somente durante a
callback.

### Regras de tempo real

Callbacks de áudio **não podem**:

- bloquear em rede, arquivo, mutex demorado ou UI;
- abrir diálogos;
- esperar outra thread;
- reter o ponteiro de amostras;
- lançar exceções através da fronteira C.

Pré-aloque buffers e envie trabalho não urgente para outra thread. Uma exceção
capturada pelo host desativa o processador daquele complemento.

### Áudio espacial de alto nível

Para a maioria dos plugins, prefira as funções seguras de alto nível:

```cpp
audio->set_listener_transform(ctx, connection, &listener);
audio->set_user_transform(ctx, connection, userId, &position,
                          1.0f, 80.0f, 1.4f);
audio->set_user_gain(ctx, connection, userId, 0.8f);
audio->set_user_pan(ctx, connection, userId, -0.25f);
audio->set_user_radio_effect(ctx, connection, userId, 1, 0.9f, 0.15f);
```

O host calcula atenuação entre distância mínima/máxima, orientação esquerda e
direita, pan estéreo e filtro de rádio. Vários plugins podem contribuir; ganhos
são multiplicados e pans somados com clamp.

`reset_user` e `reset_connection` removem imediatamente o estado aplicado pelo
plugin. `play_pcm` injeta efeitos PCM S16 mono/estéreo de 48 kHz, limitados a dez
segundos por chamada — útil para cliques, bipes e ruído de rádio. O Halla também
remove todo o estado ao desativar/descarregar o complemento.

## Transporte de dados do plugin — protocolo v5

`HallaDataApiV1` envia payload binário pelo canal TCP/TLS do Halla Server.
Mensagens são isoladas pelo ID do complemento e podem ter como destino:

- usuários específicos;
- participantes do canal atual;
- todos os clientes compatíveis no servidor.

Limites:

- payload de 8 KiB;
- tópico UTF-8 de 64 bytes;
- até 64 destinos explícitos;
- rate limit de 200 mensagens por 10 segundos por cliente.

O servidor não persiste nem interpreta o payload. Ele valida, limita e encaminha
somente a clientes com protocolo v5. A callback recebe conexão, remetente,
tópico e bytes. Para coordenadas, use estruturas versionadas, endian definido e
frequência moderada (normalmente 10–20 Hz).

## Interface e atalhos

`HallaUiApiV1` permite:

- mostrar notificações;
- registrar ações no menu **Complementos**;
- atribuir um atalho de janela à ação;
- remover a ação durante `shutdown`.

Callbacks de ações executam na thread principal. No Windows, sequências de
teclado compatíveis são registradas com `RegisterHotKey` e funcionam mesmo com o
jogo em foco; se o registro global estiver ocupado ou não for representável, o
Halla mantém o atalho no contexto da janela. Nas outras plataformas, esta versão
usa atalho de janela.

## Eventos JSON

Eventos atuais:

- `client_state`: snapshot compacto da conexão ativa, usado também pelo overlay;
- `connection_opened` / `connection_closed`;
- `connection_state`: snapshot detalhado de qualquer conexão atualizada;
- `chat_message` e `poke_received`;
- `server_error`: recusa assíncrona de uma ação pelo servidor;
- `plugin_data`: cópia base64 para compatibilidade com plugins orientados a eventos;
- `application_shutdown`.

Plugins que registram `HallaPluginDataFn` recebem também o payload binário direto,
sem conversão base64.

## Arquitetura para SaltyChat, TFAR e ACRE

Uma integração recomendada possui três partes:

1. Um resource/mod do jogo obtém posição, orientação, veículo e rádio por API
   oficial e entrega os dados à DLL por named pipe/socket local.
2. A DLL envia metadados versionados por `HallaDataApiV1` e recebe os estados
   dos outros participantes.
3. A DLL atualiza listener/fontes por `HallaAudioApiV1`; o Halla executa
   atenuação, pan e filtros dentro do pipeline de áudio.

Ler memória do jogo com `ReadProcessMemory` é tecnicamente possível para uma DLL
nativa, mas não é oferecido nem recomendado pelo Halla: pode quebrar em updates e
acionar anticheats. Prefira APIs de FiveM/Arma e IPC local autenticado.

## Exemplos

- [`examples/plugins/hello_world`](../examples/plugins/hello_world): ABI-base,
  log, configurações e eventos.
- [`examples/plugins/advanced_sdk`](../examples/plugins/advanced_sdk): descoberta
  modular, snapshots de conexões, callback PCM, dados binários, notificação,
  ação e atalho.

## Catálogo

A aba Complementos lê por HTTPS:

```text
https://raw.githubusercontent.com/GroupHalla/Halla/main/addons/catalog.json
```

Cada item informa URL HTTPS e SHA-256. O download é cancelado se o checksum não
corresponder.

## Limites de segurança dos pacotes

- 100 MiB compactados;
- 2.000 entradas;
- 250 MiB extraídos;
- 256 KiB por manifesto;
- bloqueio de caminhos absolutos, `..` e links simbólicos;
- catálogo de no máximo 1 MiB e 500 entradas exibidas.

## Overlay oficial

`official.talking-overlay` permanece uma extensão interna, desativada por
padrão. Usa janela transparente, click-through e sempre no topo, sem injeção ou
hooking. É destinado a jogos em janela e tela cheia sem bordas.
