#!/usr/bin/env python3
"""Verifica o estilo dos fontes ativos; só altera arquivos com --aplicar."""

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
APPLICATION = (ROOT / "ambiente/aplicativo.txt").read_text(encoding="utf-8").strip()
SOURCE = ROOT
TOOLS = ROOT / "trabalho/ferramentas-estilo"
C_EXTENSIONS = {".c", ".h", ".cpp", ".hpp", ".inc"}
VERSIONS = {
    "clang-format": "23.1.3",
    "ruff": "0.14.3",
    "shfmt": "3.12.0",
    "cmake-format": "0.6.13",
}
CLASS_FRAGMENTS = {
    "bpcb": {
        "ui/package_picker.inc",
        "ui/confirmation.inc",
        "ui/document.inc",
        "ui/calibration.inc",
        "ui/history.inc",
        "ui/rigol.inc",
        "ui/communication.inc",
        "ui/communication_tools.inc",
        "ui/lab.inc",
        "ui/workspace.inc",
        "ui/support.inc",
        "ui/workbench.inc",
        "ui/check_ui.inc",
    },
    "studio": {"ui/authoring.inc"},
}


def language(path):
    if path.suffix == ".json" and path.parent.name == "catalog":
        return "json"
    if path.suffix in C_EXTENSIONS:
        return "c"
    if path.suffix == ".py":
        return "python"
    if path.suffix == ".sh":
        return "shell"
    if path.suffix == ".cmake" or path.name == "CMakeLists.txt":
        return "cmake"
    return None


def source_files(selected_language):
    paths = set()
    excluded = {
        ".git",
        ".venv",
        "venv",
        "__pycache__",
        "build",
        "trabalho",
        "dependencias",
        "distribuicao",
        "dados",
        "evidencias",
        "historico",
    }
    for folder in [SOURCE, ROOT / "scripts"]:
        for directory, children, names in os.walk(folder, followlinks=False):
            current = Path(directory)
            children[:] = sorted(
                child
                for child in children
                if child not in excluded
                and not child.startswith(("build-", "cmake-build-"))
                and not (current == SOURCE / "vendor" and child == "drivers")
            )
            for name in names:
                path = current / name
                kind = language(path)
                if kind and (selected_language is None or kind == selected_language):
                    paths.add(path)
    return sorted(paths)


def run(command, content=None, environment=None):
    process = subprocess.run(
        command,
        input=content,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        timeout=30,
        check=False,
    )
    if process.returncode:
        message = process.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(message or f"Comando falhou: {command[0]}")
    return process.stdout


def find_tool(name, local_path, environment_variable):
    override = os.environ.get(environment_variable)
    packaged = TOOLS / "python/bin" / name
    if not local_path.is_file() and packaged.is_file():
        local_path = packaged
    path = override or (str(local_path) if local_path.is_file() else shutil.which(name))
    if not path:
        raise RuntimeError(
            f"{name} ausente. Use {environment_variable} para informar seu caminho."
        )
    return [path]


def formatter_commands(languages):
    commands = {}
    environment = os.environ.copy()
    python_path = str(TOOLS / "python")
    previous = environment.get("PYTHONPATH")
    environment["PYTHONPATH"] = python_path + (os.pathsep + previous if previous else "")
    specifications = {
        "c": ("clang-format", TOOLS / "bin/clang-format", "BPCB_CLANG_FORMAT"),
        "python": ("ruff", TOOLS / "python/bin/ruff", "BPCB_RUFF"),
        "shell": ("shfmt", TOOLS / "bin/shfmt", "BPCB_SHFMT"),
    }
    for kind in sorted(languages - {"json"}):
        if kind == "cmake":
            override = os.environ.get("BPCB_CMAKE_FORMAT")
            command = [override] if override else [sys.executable, "-m", "cmakelang.format"]
            name = "cmake-format"
        else:
            name, local_path, variable = specifications[kind]
            command = find_tool(name, local_path, variable)
        version = run([*command, "--version"], environment=environment).decode("utf-8").strip()
        expected = VERSIONS[name]
        if not re.search(rf"(?<![0-9.]){re.escape(expected)}(?![0-9.])", version):
            raise RuntimeError(f"{name}: esperado {expected}; encontrado {version}.")
        commands[kind] = command
    return commands, environment


def formatted_content(path, content, commands, environment):
    kind = language(path)
    if kind == "json":
        value = json.loads(content)
        return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    command = commands[kind]
    if kind == "c":
        relative = path.relative_to(SOURCE).as_posix() if path.is_relative_to(SOURCE) else ""
        class_fragment = relative in CLASS_FRAGMENTS[APPLICATION]
        prefix = b"struct FormattingContext {\n"
        suffix = b"};\n"
        original = prefix + content + b"\n" + suffix if class_fragment else content
        result = run(
            [*command, "--style=file", f"--assume-filename={path.with_suffix('.cpp')}"],
            original,
            environment,
        )
        if class_fragment:
            if not result.startswith(prefix) or not result.endswith(suffix):
                raise RuntimeError(f"Fragmento de classe não reconhecido: {path}")
            result = result[len(prefix) : -len(suffix)]
        return result
    if kind == "python":
        return run(
            [
                *command,
                "format",
                "--no-cache",
                "--config",
                str(ROOT / "ruff.toml"),
                "--stdin-filename",
                str(path),
                "-",
            ],
            content,
            environment,
        )
    if kind == "shell":
        return run([*command, "-ln", "bash", "-i", "4", "-ci"], content, environment)
    return run(
        [*command, "-c", str(ROOT / ".cmake-format.json"), "--", str(path)],
        environment=environment,
    )


def snapshot_matches(apply):
    if APPLICATION != "studio":
        return True
    vendor = SOURCE / "vendor/bpcb"
    path = vendor / "COMMUNICATION-SNAPSHOT.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    changed = False
    for name, expected in manifest["files"].items():
        actual = hashlib.sha256((vendor / name).read_bytes()).hexdigest()
        if actual != expected:
            manifest["files"][name] = actual
            changed = True
    if changed and apply:
        path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("Hashes do snapshot independente atualizados.")
    elif changed:
        print("Snapshot independente: hashes precisam de atualização explícita.")
    return apply or not changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--aplicar", action="store_true", help="Aplica o estilo; padrão só verifica."
    )
    parser.add_argument("--diff", action="store_true", help="Mostra as diferenças de formatação.")
    parser.add_argument("--linguagem", choices=["c", "python", "shell", "cmake", "json"])
    args = parser.parse_args()
    if not SOURCE.is_dir():
        raise RuntimeError(f"Árvore ativa não encontrada: {SOURCE}")
    files = source_files(args.linguagem)
    commands, environment = formatter_commands({language(path) for path in files})
    changed = []
    for path in files:
        if not stat.S_ISREG(path.lstat().st_mode):
            raise RuntimeError(f"O formatador exige um arquivo regular: {path}")
        original = path.read_bytes()
        formatted = formatted_content(path, original, commands, environment)
        if original == formatted:
            continue
        relative = path.relative_to(ROOT).as_posix()
        changed.append(relative)
        if args.diff:
            difference = difflib.unified_diff(
                original.decode("utf-8").splitlines(keepends=True),
                formatted.decode("utf-8").splitlines(keepends=True),
                fromfile=relative,
                tofile=relative + " (formatado)",
            )
            sys.stdout.writelines(difference)
        if args.aplicar:
            path.write_bytes(formatted)
        print(f"{'Formatado' if args.aplicar else 'Fora do padrão'}: {relative}")
    print(
        f"{len(files)} arquivos verificados; {len(changed)} "
        f"{'formatados' if args.aplicar else 'fora do padrão'}."
    )
    consistent = snapshot_matches(args.aplicar)
    return 0 if consistent and (args.aplicar or not changed) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"Erro de formatação: {error}", file=sys.stderr)
        raise SystemExit(2)
