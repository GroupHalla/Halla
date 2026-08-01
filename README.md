# Halla

Cliente de comunicação de voz para desktop, escrito em **C++ e Qt (Qt Widgets)**,
com a interface e o comportamento do TeamSpeak 3: mesma barra de menus
(Conexões, Favoritos, Permissões, Ferramentas, Ajuda), barra de ferramentas com
plugues de conexão, abas de servidor, árvore de canais/usuários com mini-ícones
de estado, chat com BBCode, painel de informações à direita, barra de status com
ping e diálogos com a faixa azul-marinho característica.

> **Observação sobre o protocolo:** o Halla é o **aplicativo cliente** (a
> interface e toda a lógica do lado do cliente). O protocolo de rede do
> TeamSpeak 3 é proprietário e não faz parte deste projeto — as conexões criam
> o estado local exatamente como o cliente mantém em memória (canais, usuário
> local, chat, permissões), sem servidor e sem simulação de outros clientes.

![Halla](shots/demo.png)

## Recursos

- **Janela principal estilo TS3:** menus completos, barra de ferramentas com
  Conectar/Desconectar, Favoritos, Ausente, Mudo (microfone), Mudo
  (alto-falantes), Registro do cliente, Opções e Notificações.
- **Conexões múltiplas em abas** (estilo TS3 3.5+), com menu de contexto nas
  abas, fechar com o "x", desconectar tudo.
- **Árvore do servidor idêntica:** canais (padrão=ícone de casa, protegido por
  senha=cadeado, moderado, temporário em outra cor), contagem de clientes,
  usuários com anel verde ao falar e mini-ícones (mic mudo, fones mudos,
  ausente, gravando, comandante), *tooltips*, **arraste a si mesmo** entre
  canais e **duplo clique** para entrar no canal.
- **Menus de contexto completos** (servidor, canal e cliente): alternar para o
  canal, criar canal/sub-canal, editar, excluir, apelido, descrição, cutucar,
  volume, silenciar localmente, comandante do canal, expulsar, banir, mover.
- **Chat com BBCode** (`[b][i][u][color=][size=][url=]`), emojis, controle de
  tamanho do texto, abas de chat do servidor/canal e mensagens privadas.
- **Painel de informações** (banner + detalhes do servidor, canal ou cliente
  selecionado, com tempo ativo atualizando ao vivo).
- **Opções** com todas as páginas do TS3: Aplicativo, Design (tema **claro e
  escuro** funcionais), Notificações, Reprodução, Captura (PTT com tecla,
  nível de ativação de voz, redução de eco...), **Teclas de atalho** (atalhos
  funcionais dentro do app), Segurança e Complementos.
- **Favoritos** (gerenciador completo com auto-conectar), **conexões recentes**,
  **identidades** (ID único gerado por cliente, identidade padrão), **contatos**,
  **listas de sussurro**, **grupos de servidores** com grade de permissões do
  TS3 (modo avançado habilita a coluna "Conceder"), **chave de privilégio**,
  **client log** com filtro por nível e exportação.
- **Bandeja do sistema**, confirmação ao sair conectado, restauração de sessão,
  log em arquivo, configurações persistentes (`QSettings`).

## Compilação

### Instalador Windows (.exe) — já incluído

| Arquivo | Tamanho | Descrição |
|---|---|---|
| `Halla-Setup-3.6.2.exe` | ~15 MB | Instalador NSIS (licença, pasta, atalhos no Menu Iniciar/Área de trabalho, desinstalador em "Adicionar ou remover programas") |
| `Halla-3.6.2-win64-portable.zip` | ~20 MB | Versão portátil — descompacte e execute `Halla.exe` |

O instalador foi produzido por **compilação cruzada** (Linux → Windows) e
**validado executando o `Halla.exe` real** via Wine:

```bash
# 1) toolchain MinGW + NSIS + aqtinstall (baixa o Qt p/ Windows)
sudo apt install g++-mingw-w64-x86-64-posix nsis p7zip-full
pip3 install aqtinstall
python3 -m aqt install-qt windows desktop 6.8.2 win64_mingw -O ~/qt-win --archives qtbase

# 2) shim das ferramentas do Qt do host (moc/uic/rcc)
mkdir -p ~/qt-host-shim/lib && ln -sfn /usr/lib/x86_64-linux-gnu/cmake ~/qt-host-shim/lib/cmake

# 3) build cruzado
cmake -S . -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/win64-mingw.cmake \
  -DCMAKE_PREFIX_PATH=~/qt-win/6.8.2/mingw_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-win        # gera build-win/Halla.exe

# 4) empacota DLLs do Qt + runtime MinGW em dist/Halla/, depois:
makensis packaging/halla-setup.nsi   # gera Halla-Setup-3.6.2.exe
```

### Linux (nativo)

```bash
sudo apt install cmake ninja-build qt6-base-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/Halla
```

### Windows (MSVC + Qt 6)

```powershell
# Instale o Qt 6 (qt.io) com Qt Widgets e o Qt Creator/CMake
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="C:\Qt\6.8.0\msvc2022_64"
cmake --build build --config Release
build\Release\Halla.exe
```

### macOS

```bash
brew install qt@6 cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build
open build/Halla.app
```

## Estrutura do código

```
src/
  main.cpp              # ponto de entrada (suporta --shot/--demo p/ capturas)
  version.h
  core/                 # modelo de dados (ServerData/Channel/User), log, QSettings
  gui/                  # ícones desenhados com QPainter, banner, árvore TS3,
                        # chat BBCode, painel de informações, tela inicial
  dialogs/              # Conectar, Criar/Editar canal, Identidades, Favoritos,
                        # Opções (8 páginas), Grupos/permissões, Log, Contatos,
                        # Sussurro, Transferências, Cutucar/Expulsar/Banir/Volume
  app/MainWindow.*      # janela principal: menus, toolbar, abas, status, bandeja
```
