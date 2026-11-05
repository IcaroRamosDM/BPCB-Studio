#!/usr/bin/env python3
"""Obtém os fontes oficiais do FLTK e verifica sua integridade antes de extrair."""

import hashlib
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
VERSION = "1.4.5"
ARCHIVE_NAME = f"fltk-{VERSION}-source.tar.gz"
URL = f"https://github.com/fltk/fltk/releases/download/release-{VERSION}/{ARCHIVE_NAME}"
SHA256 = "eede1fb2b8e9c2e581e77082e15252145855c79aad30070ee3b24aabe2f926f1"
MAX_DOWNLOAD = 32 * 1024 * 1024
MAX_EXTRACTED = 160 * 1024 * 1024


def verify(archive):
    with archive.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != SHA256:
        raise RuntimeError(f"SHA-256 inválido: {archive}. Remova-o e tente novamente.")


def prepare():
    downloads = ROOT / "dependencias/downloads"
    sources = ROOT / "dependencias/locais"
    target = sources / f"fltk-{VERSION}"
    if target.is_dir():
        if not (target / "CMakeLists.txt").is_file():
            raise RuntimeError(f"Pasta de fontes incompleta: {target}")
        print(f"Usando fontes locais existentes: {target}")
        return
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / ARCHIVE_NAME
    if not archive.exists():
        with tempfile.TemporaryDirectory(prefix="fltk-", dir=downloads) as temporary:
            partial = Path(temporary) / ARCHIVE_NAME
            with urllib.request.urlopen(URL, timeout=60) as response, partial.open("wb") as output:
                total = 0
                while chunk := response.read(1024 * 1024):
                    total += len(chunk)
                    if total > MAX_DOWNLOAD:
                        raise RuntimeError("Download excedeu o limite esperado.")
                    output.write(chunk)
            verify(partial)
            partial.replace(archive)
    verify(archive)
    sources.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fltk-extract-", dir=sources) as temporary:
        with tarfile.open(archive, "r:gz") as package:
            members = package.getmembers()
            if len(members) > 20000 or sum(member.size for member in members) > MAX_EXTRACTED:
                raise RuntimeError("Arquivo FLTK excedeu os limites de extração.")
            for member in members:
                parts = Path(member.name).parts
                if (
                    not parts
                    or parts[0] != f"fltk-{VERSION}"
                    or ".." in parts
                    or member.name.startswith("/")
                    or not (member.isfile() or member.isdir())
                ):
                    raise RuntimeError(f"Entrada inesperada no arquivo FLTK: {member.name}")
            package.extractall(temporary, members=members, filter="data")
        extracted = Path(temporary) / f"fltk-{VERSION}"
        if not (extracted / "CMakeLists.txt").is_file():
            raise RuntimeError("Arquivo não contém o projeto FLTK esperado.")
        shutil.move(str(extracted), str(target))
    print(f"FLTK {VERSION} verificado e extraído em {target}")


if __name__ == "__main__":
    prepare()
