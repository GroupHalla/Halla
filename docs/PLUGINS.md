# Extensões e pacotes do Halla Desktop

O Halla Desktop carrega plugins nativos da comunidade por meio da ABI C
estável declarada em [`sdk/halla_plugin_api.h`](../sdk/halla_plugin_api.h).
No Windows, cada plugin é uma DLL carregada com `QLibrary`. O cabeçalho e os
exemplos do SDK podem ser usados em plugins comunitários sob os termos
permissivos de [`sdk/LICENSE.txt`](../sdk/LICENSE.txt).

> **Segurança:** uma DLL executa código no mesmo processo e com os mesmos
> privilégios do Halla. Instale somente pacotes de autores confiáveis. O Halla
> mostra o autor e o SHA-256 antes de cada instalação.

## Pacote `.halla-addon`

O arquivo é um ZIP comum renomeado para `.halla-addon`. Ele deve conter:

```text
manifest.json
bin/windows-x64/meu_plugin.dll
assets/...
```

Exemplo de `manifest.json`:

```json
{
  "id": "com.exemplo.meu-plugin",
  "name": "Meu plugin",
  "version": "1.0.0",
  "author": "Comunidade Halla",
  "description": "Exemplo de extensão nativa.",
  "type": "native",
  "apiVersion": 1,
  "defaultEnabled": false,
  "platforms": {
    "windows-x64": {
      "library": "bin/windows-x64/meu_plugin.dll"
    }
  },
  "settings": [
    {
      "key": "enabledFeature",
      "type": "bool",
      "label": "Ativar recurso",
      "default": true
    },
    {
      "key": "position",
      "type": "choice",
      "label": "Posição",
      "default": "right",
      "options": [
        {"value": "left", "label": "Esquerda"},
        {"value": "right", "label": "Direita"}
      ]
    }
  ]
}
```

IDs aceitam letras minúsculas, números, ponto, hífen e sublinhado. O caminho
da biblioteca deve ser relativo e não pode sair do pacote. Pacotes são limitados
a 100 MiB, 2.000 entradas e 250 MiB extraídos; caminhos absolutos, `..` e links
simbólicos são recusados.

O empacotador oficial cria um ZIP reproduzível e o checksum:

```bash
python tools/package_plugin.py pasta-do-plugin meu-plugin.halla-addon
```

## ABI do plugin

A DLL exporta uma única função C:

```cpp
extern "C" HALLA_PLUGIN_EXPORT
const HallaPluginApi* halla_plugin_entry(void);
```

O plugin recebe `HallaHostApi` em `initialize()`. A API inicial oferece:

- registro no log do cliente;
- leitura das configurações em JSON;
- solicitação do estado atual da call;
- eventos `client_state` e `application_shutdown`;
- atualização de configurações sem reiniciar o Halla.

`client_state` possui este formato básico:

```json
{
  "event": "client_state",
  "payload": {
    "connected": true,
    "serverName": "Servidor Halla",
    "serverAddress": "exemplo.com:9987",
    "selfId": 5,
    "channelId": 1,
    "channelName": "Canal padrão",
    "users": [
      {
        "id": 7,
        "name": "Ana",
        "talking": true,
        "whispering": false,
        "muted": false,
        "self": false
      }
    ]
  }
}
```

As callbacks são executadas na thread principal da interface. Não bloqueie essa
thread e envie trabalho demorado para uma thread própria. Não retenha ponteiros
recebidos do host depois da chamada atual; copie qualquer string que precisar
manter. `shutdown()` deve encerrar threads e liberar janelas e outros recursos
antes de retornar.

## Catálogo

A aba **Complementos** lê por HTTPS:

```text
https://raw.githubusercontent.com/GroupHalla/Halla/main/addons/catalog.json
```

Formato:

```json
{
  "version": 1,
  "addons": [
    {
      "id": "com.exemplo.meu-plugin",
      "name": "Meu plugin",
      "version": "1.0.0",
      "author": "Comunidade Halla",
      "description": "Descrição curta",
      "downloadUrl": "https://exemplo.com/meu-plugin.halla-addon",
      "sha256": "64 caracteres hexadecimais"
    }
  ]
}
```

O download é recusado se não usar HTTPS ou se o SHA-256 não corresponder.

## Overlay oficial

`official.talking-overlay` é distribuído dentro do Desktop e serve como extensão
oficial inicial. Ele usa uma janela transparente, click-through e sempre no
topo — sem injetar código no processo do jogo. Funciona melhor em modo janela e
tela cheia sem bordas. Suporte a tela cheia exclusiva por hooking poderá ser
avaliado separadamente por causa de anticheats e riscos de segurança.
