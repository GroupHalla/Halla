from pathlib import Path
import json
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
translations = root / "translations"
source_path = translations / "halla_source.ts"
source_tree = ET.parse(source_path)


def messages(tree):
    result = {}
    for context in tree.getroot().findall("context"):
        context_name = context.findtext("name") or ""
        for message in context.findall("message"):
            source = message.findtext("source") or ""
            translation = message.findtext("translation") or ""
            result[(context_name, source)] = translation
    return result


source_messages = messages(source_tree)
assert len(source_messages) >= 800, len(source_messages)
placeholder_pattern = re.compile(r"%(?:L?\d+|n)")
portuguese_residue = re.compile(
    r"\b(?:não|você|usuário|usuários|servidor|servidores|canal|canais|cargo|cargos|"
    r"permissão|permissões|aplicar|remover|atualizar|fechar|senha|endereço|"
    r"configuração|configurações|selecione|excluir|adicionar|mensagem|arquivo|"
    r"arquivos|padrão|depois|antes|ordem|exibição|posição|sussurro)\b",
    re.IGNORECASE,
)

for language in ("en", "es"):
    path = translations / f"halla_{language}.ts"
    tree = ET.parse(path)
    assert tree.getroot().attrib.get("language") == language
    translated = messages(tree)
    assert translated.keys() == source_messages.keys(), (
        language,
        len(source_messages.keys() - translated.keys()),
        len(translated.keys() - source_messages.keys()),
    )
    assert "type=\"unfinished\"" not in ET.tostring(tree.getroot(), encoding="unicode")
    for key, value in translated.items():
        context, source = key
        assert value.strip(), (language, context, source)
        assert sorted(placeholder_pattern.findall(value)) == sorted(
            placeholder_pattern.findall(source)
        ), (language, context, source, value)
        if language == "en":
            # github.com is the only intentional English string containing a
            # Portuguese short-word false positive, so strip URLs first.
            without_urls = re.sub(r"https?://\S+|github\.com/\S*", "", value)
            assert not portuguese_residue.search(without_urls), (
                language, context, source, value
            )

reviewed = json.loads((translations / "reviewed_overrides.json").read_text(encoding="utf-8"))
for language, overrides in reviewed.items():
    translated = messages(ET.parse(translations / f"halla_{language}.ts"))
    by_source = {}
    for (_, source), value in translated.items():
        by_source.setdefault(source, set()).add(value)
    for source, expected in overrides.items():
        if source in by_source:
            assert by_source[source] == {expected}, (language, source, by_source[source], expected)

qrc = (root / "src/halla.qrc").read_text(encoding="utf-8")
main = (root / "src/main.cpp").read_text(encoding="utf-8")
for language in ("en", "es"):
    qm = root / f"src/assets/i18n/halla_{language}.qm"
    assert qm.is_file() and qm.stat().st_size > 50_000, qm
    assert f"assets/i18n/halla_{language}.qm" in qrc
    assert f"halla_{language}.qm" in main

print(f"Translation audit OK: {len(source_messages)} contextual strings in English and Spanish")
