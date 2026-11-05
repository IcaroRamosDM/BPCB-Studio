#!/usr/bin/env python3
"""Corrupted backup inputs must fail before creating the destination."""

from pathlib import Path
import hashlib
import json
import subprocess
import sys
import tempfile
import zipfile

cli = Path(sys.argv[1]).resolve()


def call(*args, ok=True):
    result = subprocess.run([str(cli), *map(str, args)], capture_output=True, text=True, timeout=30)
    assert (result.returncode == 0) == ok, (args, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="studio-backup-cases-") as directory:
    root = Path(directory)
    project = root / "p.bpcb"
    backup = root / "b.bpcbstudio"
    call("example", project)
    call("backup", project, backup)
    preview = json.loads(call("backup-preview", backup))
    assert len(preview["files"]) == 1
    with zipfile.ZipFile(backup) as archive:
        files = {n: archive.read(n) for n in archive.namelist()}
    for label, mutate in [
        ("hash", lambda f: f.update({"project.bpcb": f["project.bpcb"].replace(b"1 k", b"9 k")})),
        ("escape", lambda f: f.update({"../outside": b"bad"})),
        ("extra", lambda f: f.update({"assets/extra.svg": b"extra"})),
        ("missing", lambda f: f.pop("project.bpcb")),
        (
            "type",
            lambda f: f.update(
                {"backup.json": b'{"format":"bpcb.studio.backup","schema":1,"files":[]}'}
            ),
        ),
    ]:
        changed = files.copy()
        mutate(changed)
        candidate = root / (label + ".bpcbstudio")
        with zipfile.ZipFile(candidate, "w") as archive:
            for name, data in changed.items():
                archive.writestr(name, data)
        call("backup-preview", candidate, ok=False)
        destination = root / (label + "-restore")
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        call("restore", candidate, destination, digest, ok=False)
        assert not destination.exists()
    candidate = root / "symlink.bpcbstudio"
    with zipfile.ZipFile(candidate, "w") as archive:
        for name, data in files.items():
            archive.writestr(name, data)
        info = zipfile.ZipInfo("assets/link.svg")
        info.create_system = 3
        info.external_attr = 0o120777 << 16
        archive.writestr(info, "../../outside")
    call("backup-preview", candidate, ok=False)
    restored = root / "restored"
    call("restore", backup, restored, preview["digest"])
    assert json.loads((restored / "project.bpcb").read_text()) == json.loads(project.read_text())
    call("restore", backup, restored, preview["digest"], ok=False)
    call("restore", backup, root / "wrong-token", "0" * 64, ok=False)
print("PASS: hash, caminhos, arquivos extras/ausentes, tipos, symlink, token e destino existente.")
