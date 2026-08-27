# Catálogo comunitário de complementos

> **O catálogo oficial mudou de casa.** Desde o Halla Desktop 1.1.11 o aplicativo
> lê a central oficial de complementos em
> **https://grouphalla.github.io/Halla-Addons/** (repositório
> [GroupHalla/Halla-Addons](https://github.com/GroupHalla/Halla-Addons)),
> com etiquetas de plataforma (Desktop/Mobile), versões e atualizações.
> Este arquivo permanece apenas para clientes antigos e como exemplo do
> formato v1 — `catalog.json` segue válido, porém vazio.

A aba **Complementos** de versões antigas do Halla Desktop lê `catalog.json`
diretamente deste diretório. Para sugerir um pacote comunitário:

1. publique o `.halla-addon` em uma URL HTTPS estável;
2. publique também seu SHA-256;
3. abra uma contribuição adicionando a entrada ao array `addons`;
4. informe código-fonte, autor, licença e instruções para reproduzir a DLL.

A inclusão no catálogo não transforma uma DLL comunitária em código oficial.
O Halla sempre exibe o aviso de execução nativa e valida o SHA-256 antes da
instalação.

O formato completo está documentado em [`docs/PLUGINS.md`](../docs/PLUGINS.md).
