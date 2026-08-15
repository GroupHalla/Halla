# Catálogo comunitário de complementos

A aba **Complementos** do Halla Desktop lê `catalog.json` diretamente deste
diretório. Para sugerir um pacote comunitário:

1. publique o `.halla-addon` em uma URL HTTPS estável;
2. publique também seu SHA-256;
3. abra uma contribuição adicionando a entrada ao array `addons`;
4. informe código-fonte, autor, licença e instruções para reproduzir a DLL.

A inclusão no catálogo não transforma uma DLL comunitária em código oficial.
O Halla sempre exibe o aviso de execução nativa e valida o SHA-256 antes da
instalação.

O formato completo está documentado em [`docs/PLUGINS.md`](../docs/PLUGINS.md).
