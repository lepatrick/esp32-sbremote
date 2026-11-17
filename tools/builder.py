#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive the ESPHome Device Builder add-on through Home Assistant.

Reads the HA host and token from .secrets in the repository root (see
.secrets.example); neither is ever printed.

The device config is switchbot-remote.yaml; if local/switchbot-remote.yaml
exists (git-ignored, for personal settings) that one is used instead.

  builder.py status                 device state as the Device Builder sees it
  builder.py push                   upload the YAML and the component
  builder.py install                compile in the Device Builder and flash over the air
  builder.py compile                compile only
  builder.py logs SECONDS [REGEX]   stream the device log
  builder.py events SECONDS         listen in HA for esphome.switchbot_remote events

Setup:  python3 -m venv .venv && .venv/bin/pip install websockets esphome==<add-on version>
"""

import asyncio
import base64
import io
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request

import websockets

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONF = "switchbot-remote.yaml"
LOCAL_CONF = os.path.join(ROOT, "local", CONF)
CONF_PATH = LOCAL_CONF if os.path.exists(LOCAL_CONF) else os.path.join(ROOT, CONF)
ADDON = "5c53de3b_esphome"
# files in the Device Builder's config dir that this project owns
OURS = (CONF, "components/bt_classic_hid/")
EVENT = "esphome.switchbot_remote"

_secrets = open(os.path.join(ROOT, ".secrets")).read()
HOST = re.search(r"^Host:\s*(\S+)", _secrets, re.M).group(1)
TOKEN = re.search(r"^Token:\s*(\S+)", _secrets, re.M).group(1)
ANSI = re.compile(r"(\x1b|\\033)\[[0-9;]*m")


async def ha_socket():
    ws = await websockets.connect(f"ws://{HOST}/api/websocket", max_size=None)
    await ws.recv()
    await ws.send(json.dumps({"type": "auth", "access_token": TOKEN}))
    if json.loads(await ws.recv()).get("type") != "auth_ok":
        raise SystemExit("Home Assistant rejected the token")
    return ws


async def supervisor(ws, msg_id, endpoint, method):
    await ws.send(json.dumps({"id": msg_id, "type": "supervisor/api", "endpoint": endpoint, "method": method}))
    while True:
        r = json.loads(await ws.recv())
        if r.get("id") == msg_id:
            if not r.get("success"):
                raise SystemExit(f"supervisor {endpoint}: {r.get('error')}")
            return r["result"]


class Builder:
    """WebSocket + HTTP client for the Device Builder, reached via HA ingress."""

    async def __aenter__(self):
        ha = await ha_socket()
        info = await supervisor(ha, 1, f"/addons/{ADDON}/info", "get")
        self.session = (await supervisor(ha, 2, "/ingress/session", "post"))["session"]
        await ha.close()
        self.entry = info["ingress_entry"]
        self.ws = await websockets.connect(
            f"ws://{HOST}{self.entry}/ws", max_size=None,
            additional_headers={"Cookie": f"ingress_session={self.session}"})
        self.info = json.loads(await self.ws.recv())
        self.ids = itertools.count(1)
        return self

    async def __aexit__(self, *exc):
        await self.ws.close()

    async def call(self, command, timeout=60, **args):
        mid = str(next(self.ids))
        await self.ws.send(json.dumps({"command": command, "message_id": mid, "args": args}))
        while True:
            m = json.loads(await asyncio.wait_for(self.ws.recv(), timeout))
            if m.get("message_id") != mid:
                continue
            if "error_code" in m:
                raise SystemExit(f"{command}: {m['error_code']}: {m.get('details')}")
            if "event" in m:
                if m["event"] == "result":
                    return m.get("data")
                continue
            return m.get("result")

    def http(self, path, data=None, method=None):
        req = urllib.request.Request(f"http://{HOST}{self.entry}{path}", data=data, method=method)
        req.add_header("Cookie", f"ingress_session={self.session}")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()


async def cmd_status():
    async with Builder() as db:
        print("Device Builder", db.info.get("server_version"), "| ESPHome", db.info.get("esphome_version"))
        for c in (await db.call("devices/list")).get("configured", []):
            if c.get("configuration") == CONF:
                rs = c.get("runtime_state") or {}
                print(f"{c.get('name')}: {rs.get('state')} | ip {rs.get('ip_addresses')} | "
                      f"deployed {rs.get('deployed_version')} | pending changes: {c.get('has_pending_changes')}")


def make_bundle() -> bytes:
    """Bundle YAML + component without any secrets, so the Builder's secrets.yaml stays untouched."""
    esphome = os.path.join(os.path.dirname(sys.executable), "esphome")
    with tempfile.TemporaryDirectory() as work:
        shutil.copy(CONF_PATH, os.path.join(work, CONF))
        shutil.copytree(os.path.join(ROOT, "components"), os.path.join(work, "components"),
                        ignore=shutil.ignore_patterns("__pycache__"))
        # placeholder values: only needed so that the config loads for bundling
        with open(os.path.join(ROOT, "secrets.yaml.example")) as f:
            keys = re.findall(r"^(\w+):", f.read(), re.M)
        with open(os.path.join(work, "secrets.yaml"), "w") as f:
            for k in keys:
                # throwaway values; the bundle is stripped of secrets before upload
                value = base64.b64encode(os.urandom(32)).decode() if k.endswith("_api_key") else "placeholder"
                f.write(f'{k}: "{value}"\n')
        out = os.path.join(work, "bundle.tar.gz")
        r = subprocess.run([esphome, "bundle", "-o", out, CONF], cwd=work, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(r.stdout + r.stderr)
        buf = io.BytesIO()
        with tarfile.open(out) as src, tarfile.open(fileobj=buf, mode="w:gz") as dst:
            for m in src.getmembers():
                if m.name == "secrets.yaml":
                    continue
                data = src.extractfile(m).read() if m.isfile() else None
                if m.name == "manifest.json":
                    man = json.loads(data)
                    man["files"] = [f for f in man["files"] if f != "secrets.yaml"]
                    man["has_secrets"] = False
                    man["config_dir"] = ""
                    data = json.dumps(man, indent=2).encode()
                    m.size = len(data)
                dst.addfile(m, io.BytesIO(data) if data is not None else None)
        return buf.getvalue()


async def cmd_push():
    blob = make_bundle()
    async with Builder() as db:
        async def post(query):
            token = (await db.call("devices/import_bundle_token"))["token"]
            status, body = await asyncio.to_thread(
                db.http, f"/api/devices/import_bundle?token={token}{query}", blob, "POST")
            if status != 200:
                raise SystemExit(f"import failed: HTTP {status} {body[:300]!r}")
            return json.loads(body)

        r = await post("")
        if r["status"] == "conflicts":
            foreign = [c for c in r["conflicts"] if not c.startswith(OURS)]
            if foreign:
                raise SystemExit(f"refusing to overwrite files that are not ours: {foreign}")
            r = await post("&mode=resolve" + "".join(
                "&overwrite=" + urllib.parse.quote(c, safe="") for c in r["conflicts"]))
        print("import:", r["status"], "| written:", r.get("written"), "| kept:", r.get("kept"))


async def cmd_job(kind):
    async with Builder() as db:
        async def wait(job_id):
            while True:
                j = await db.call("firmware/get_job", job_id=job_id)
                if j["status"] in ("completed", "failed", "cancelled"):
                    return j
                await asyncio.sleep(10)

        args = {"configuration": CONF}
        if kind == "install":
            args["port"] = "OTA"
        first = await db.call(f"firmware/{kind}", **args)
        jobs = [await wait(first["job_id"])]
        if kind == "install":
            for other in await db.call("firmware/get_jobs", configuration=CONF):
                if other["job_id"] != first["job_id"] and other.get("created_at", "") >= first.get("created_at", ""):
                    jobs.append(await wait(other["job_id"]))
        failed = False
        for j in jobs:
            print(f"{j['job_type']}: {j['status']} (exit {j.get('exit_code')})")
            if j["status"] != "completed":
                failed = True
                out = ANSI.sub("", "".join(j.get("output") or [])).replace("\r", "\n")
                print("\n".join(line[:240] for line in out.split("\n") if line.strip())[-4000:])
        if failed:
            raise SystemExit(1)


async def cmd_logs(seconds, pattern=None):
    pat = re.compile(pattern) if pattern else None
    async with Builder() as db:
        await db.ws.send(json.dumps({"command": "devices/logs", "message_id": "L",
                                     "args": {"configuration": CONF, "port": "OTA"}}))
        loop = asyncio.get_event_loop()
        end = loop.time() + seconds
        while loop.time() < end:
            try:
                m = json.loads(await asyncio.wait_for(db.ws.recv(), max(0.1, end - loop.time())))
            except asyncio.TimeoutError:
                break
            if m.get("message_id") != "L":
                continue
            if "error_code" in m or m.get("event") == "result":
                print("log stream ended:", m.get("error_code") or m.get("data"))
                break
            for line in str(m.get("data") or "").split("\n"):
                line = ANSI.sub("", line).rstrip()
                if line and (not pat or pat.search(line)):
                    print(line, flush=True)


async def cmd_events(seconds):
    ws = await ha_socket()
    await ws.send(json.dumps({"id": 1, "type": "subscribe_events", "event_type": EVENT}))
    loop = asyncio.get_event_loop()
    end = loop.time() + seconds
    count = 0
    while loop.time() < end:
        try:
            m = json.loads(await asyncio.wait_for(ws.recv(), max(0.1, end - loop.time())))
        except asyncio.TimeoutError:
            break
        if m.get("type") == "event":
            e = m["event"]
            count += 1
            print(e["time_fired"][11:23], e["data"].get("key"), e["data"].get("code"), e["data"].get("action"),
                  flush=True)
    await ws.close()
    print(count, "event(s)")


def main():
    args = sys.argv[1:]
    cmd = args[0] if args else ""
    if cmd == "status":
        asyncio.run(cmd_status())
    elif cmd == "push":
        asyncio.run(cmd_push())
    elif cmd in ("install", "compile"):
        asyncio.run(cmd_job(cmd))
    elif cmd == "logs" and len(args) >= 2:
        asyncio.run(cmd_logs(float(args[1]), args[2] if len(args) > 2 else None))
    elif cmd == "events" and len(args) >= 2:
        asyncio.run(cmd_events(float(args[1])))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
