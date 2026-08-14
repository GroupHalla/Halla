#!/usr/bin/env python3
"""Generate complete English/Spanish Qt catalogs from Portuguese source strings.

The committed catalogs are reviewed artifacts. This helper preserves Qt
placeholders and prefers Halla's curated terminology before requesting a
machine-translation baseline for newly added strings.
"""
from __future__ import annotations

import concurrent.futures
import json
import re
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_TS = ROOT / "translations" / "halla_source.ts"
CACHE_PATH = ROOT / ".translation-cache.json"
OVERRIDES_PATH = ROOT / "translations" / "reviewed_overrides.json"
MAIN_CPP = ROOT / "src" / "main.cpp"


def cpp_unescape(value: str) -> str:
    # Translation tables only use the common C++ escapes below.
    return (value.replace(r"\n", "\n").replace(r"\t", "\t")
            .replace(r'\"', '"').replace(r"\\", "\\"))


def exact_map(prefix: str, source: str) -> dict[str, str]:
    pattern = re.compile(
        rf'm_{prefix}\["((?:\\.|[^"\\])*)"\]\s*=\s*"((?:\\.|[^"\\])*)";')
    return {cpp_unescape(a): cpp_unescape(b) for a, b in pattern.findall(source)}


def word_block(name: str, source: str) -> dict[str, str]:
    match = re.search(
        rf"const QList<QPair<QString, QString>> {name} = \{{(.*?)\n\s*\}};",
        source, re.S)
    if not match:
        return {}
    pairs = re.findall(
        r'\{"((?:\\.|[^"\\])*)",\s*"((?:\\.|[^"\\])*)"\}',
        match.group(1))
    return {cpp_unescape(a): cpp_unescape(b) for a, b in pairs}


def curated_maps() -> dict[str, dict[str, str]]:
    source = MAIN_CPP.read_text(encoding="utf-8")
    english = exact_map("en", source)
    spanish = exact_map("es", source)
    for name in ("en", "enExtra", "enMissing"):
        english.update(word_block(name, source))
    for name in ("es", "esExtra", "esMissing"):
        spanish.update(word_block(name, source))
    return {"en": english, "es": spanish}


def protect(text: str) -> tuple[str, dict[str, str]]:
    replacements: dict[str, str] = {}

    def replace(match: re.Match[str]) -> str:
        token = f"HALLAQTARG{len(replacements)}TOKEN"
        replacements[token] = match.group(0)
        return token

    protected = re.sub(r"%(?:L?\d+|n)", replace, text)
    return protected, replacements


def translate_remote(text: str, target: str) -> str:
    if not text:
        return text
    leading = text[:len(text) - len(text.lstrip())]
    trailing = text[len(text.rstrip()):]
    core = text.strip()
    if not core:
        return text
    protected, replacements = protect(core)
    query = urllib.parse.urlencode({
        "client": "gtx", "sl": "pt", "tl": target, "dt": "t", "q": protected
    })
    url = "https://translate.googleapis.com/translate_a/single?" + query
    last_error: Exception | None = None
    for attempt in range(5):
        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                payload = json.loads(response.read().decode("utf-8"))
            translated = "".join(part[0] for part in payload[0] if part[0])
            for token, original in replacements.items():
                translated = translated.replace(token, original)
                translated = translated.replace(token.lower(), original)
            if any(token in translated for token in replacements):
                raise ValueError("placeholder token was not restored")
            return leading + translated + trailing
        except Exception as error:  # pragma: no cover - network helper
            last_error = error
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"translation failed for {text!r}: {last_error}")


def main() -> None:
    maps = curated_maps()
    reviewed = json.loads(OVERRIDES_PATH.read_text(encoding="utf-8"))
    cache = json.loads(CACHE_PATH.read_text(encoding="utf-8")) if CACHE_PATH.exists() else {}
    source_root = ET.parse(SOURCE_TS).getroot()
    sources = sorted({message.findtext("source") or ""
                      for message in source_root.findall(".//message")})

    requests: list[tuple[str, str]] = []
    results: dict[str, dict[str, str]] = {"en": {}, "es": {}}
    for language in ("en", "es"):
        language_cache = cache.setdefault(language, {})
        for source in sources:
            if source in reviewed.get(language, {}):
                results[language][source] = reviewed[language][source]
            elif source in maps[language]:
                results[language][source] = maps[language][source]
            elif source in language_cache:
                results[language][source] = language_cache[source]
            else:
                requests.append((language, source))

    def fetch(item: tuple[str, str]) -> tuple[str, str, str]:
        language, source = item
        return language, source, translate_remote(source, language)

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
        for language, source, translated in executor.map(fetch, requests):
            results[language][source] = translated
            cache[language][source] = translated
            print(f"{language}: {source!r} -> {translated!r}")

    CACHE_PATH.write_text(json.dumps(cache, ensure_ascii=False, indent=2) + "\n",
                          encoding="utf-8")

    translations_dir = ROOT / "translations"
    translations_dir.mkdir(exist_ok=True)
    for language in ("en", "es"):
        tree = ET.parse(SOURCE_TS)
        root = tree.getroot()
        root.set("language", language)
        root.set("sourcelanguage", "pt_BR")
        for message in root.findall(".//message"):
            source = message.findtext("source") or ""
            translation = message.find("translation")
            if translation is None:
                translation = ET.SubElement(message, "translation")
            translation.attrib.pop("type", None)
            translation.text = results[language][source]
        ET.indent(tree, space="  ")
        destination = translations_dir / f"halla_{language}.ts"
        tree.write(destination, encoding="utf-8", xml_declaration=True)
        print(f"wrote {destination} ({len(sources)} strings)")


if __name__ == "__main__":
    main()
