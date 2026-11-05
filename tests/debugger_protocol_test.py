#!/usr/bin/env python3
"""Real worker IPC and cancellation; simulation packages only, no hardware."""

from pathlib import Path
import json
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
import zipfile

cli, worker = map(Path, sys.argv[1:3])
checks = 0


def check(value, detail=""):
    global checks
    checks += 1
    assert value, detail


def command(*args):
    p = subprocess.run([str(cli), *map(str, args)], capture_output=True, timeout=15)
    check(p.returncode == 0, p.stderr.decode())
    return p.stdout


def defaults(kind):
    return {field["key"]: field.get("default") for field in json.loads(command("catalog", kind))}


with tempfile.TemporaryDirectory(prefix="studio-debug-ipc-") as directory:
    root = Path(directory)
    file = root / "debug.bpcb"
    command("new", file)
    project = json.loads(file.read_text())
    project["board"]["connections"] = "Fixture de simulação; nenhum hardware."
    test = defaults("test")
    test.update(
        id="debug",
        mode="simulation",
        timeout=1,
        instructions="Somente simulação.",
        parameters=[],
        formulas=[],
        steps=[],
    )
    for i in range(2):
        step = defaults("step")
        step.update(
            id=f"calc{i}",
            title=f"Calculate {i}",
            type="calculate",
            variable="count",
            expression="1" if i == 0 else "count+1",
        )
        test["steps"].append(step)
    result = defaults("step")
    result.update(id="result", title="Result", type="result", expression="count", reference="2")
    test["steps"].append(result)
    project["tests"] = [test]
    file.write_text(json.dumps(project, ensure_ascii=False))
    package = root / "debug.bancada"
    command("export", file, worker, package)
    folder = root / "package"
    with zipfile.ZipFile(package) as z:
        z.extractall(folder)

    def start(debug=True, mode="simulation"):
        p = subprocess.Popen(
            [str(worker)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        p.buffer = b""
        p.stdin.write(
            (
                json.dumps(
                    dict(
                        api=2,
                        package_dir=str(folder),
                        test="debug",
                        mode=mode,
                        studio_debug=debug,
                        parameters={},
                        components={},
                    )
                )
                + "\n"
            ).encode()
        )
        p.stdin.flush()
        return p

    def event(p, timeout=4):
        deadline = time.monotonic() + timeout
        while b"\n" not in p.buffer:
            remaining = deadline - time.monotonic()
            check(remaining > 0, "event deadline exceeded")
            ready, _, _ = select.select([p.stdout], [], [], remaining)
            check(bool(ready), "worker did not emit event")
            data = os.read(p.stdout.fileno(), 65536)
            check(bool(data), "worker closed its output")
            p.buffer += data
        line, p.buffer = p.buffer.split(b"\n", 1)
        return json.loads(line)

    p = start()
    visited = []
    while True:
        ev = event(p)
        if ev["type"] == "confirm":
            check(ev.get("debug") is True and isinstance(ev["variables"], dict), ev)
            visited.append(ev["step_id"])
            if len(visited) == 1:
                time.sleep(1.2)  # Pause exceeds the one-second execution budget.
                check(p.poll() is None, "pause must wait for explicit response")
            p.stdin.write((json.dumps(dict(reply_to=ev["id"], accepted=True)) + "\n").encode())
            p.stdin.flush()
        elif ev["type"] in ["done", "error"]:
            check(ev["type"] == "done", ev)
            break
    check(p.wait(timeout=4) == 0)
    check(visited == ["calc0", "calc1", "result"], visited)

    for debug, mode in [("yes", "simulation"), (True, "hardware")]:
        p = start(debug, mode)
        output, error = p.communicate(timeout=4)
        check(p.returncode == 1 and "Depuração".encode() in output, (output, error))
    for cancel in ["reply", "signal", "bad-id"]:
        p = start()
        ev = event(p)
        check(ev.get("debug") is True)
        if cancel == "signal":
            p.send_signal(signal.SIGTERM)
        else:
            p.stdin.write(
                (
                    json.dumps(
                        dict(
                            reply_to=ev["id"] if cancel == "reply" else "wrong",
                            accepted=cancel == "bad-id",
                        )
                    )
                    + "\n"
                ).encode()
            )
            p.stdin.flush()
        output, error = p.communicate(timeout=4)
        check(p.returncode == 1, (cancel, output, error))
    p = start(False)
    output, error = p.communicate(timeout=4)
    check(p.returncode == 0 and b'"debug":true' not in output, (output, error))
print(
    f"{checks} verificações de IPC, pausa fora do prazo, retomada, cancelamento e bloqueio físico."
)
