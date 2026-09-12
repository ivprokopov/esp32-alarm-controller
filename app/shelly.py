import os, time, urllib.parse, httpx, json
from .db import connect
from .controller import controller_post

WEBHOOK_BASE = os.getenv("CONTROLLER_WEBHOOK_URL", "http://192.168.0.218:8081/shelly")
WEBHOOK_KEY = os.getenv("SHELLY_WEBHOOK_KEY", "")
HEARTBEAT_NAME = "PROKOPOV_ALARM_HEARTBEAT"
HEARTBEAT_PERIOD_MS = 30000


def heartbeat_url(device_id: str) -> str:
    base = WEBHOOK_BASE.rsplit("/", 1)[0] + "/heartbeat"
    q = urllib.parse.urlencode({"key": WEBHOOK_KEY, "device": device_id})
    return f"{base}?{q}"


async def rpc(ip: str, method: str, params=None, username: str = "", password: str = "", timeout=4.0):
    auth = httpx.DigestAuth(username, password) if username else None
    async with httpx.AsyncClient(timeout=timeout, auth=auth) as client:
        r = await client.post(f"http://{ip}/rpc", json={"id": 1, "method": method, "params": params or {}})
        r.raise_for_status()
        data = r.json()
        if "error" in data:
            raise RuntimeError(data["error"])
        return data.get("result", data)


async def refresh_device(row):
    info = await rpc(row["ip"], "Shelly.GetDeviceInfo", username=row["username"] or "", password=row["password"] or "")
    status = await rpc(row["ip"], "Shelly.GetStatus", username=row["username"] or "", password=row["password"] or "")
    device_id = str(info.get("id") or "")
    with connect() as con:
        con.execute(
            "UPDATE shelly_devices SET device_id=?,last_seen=?,last_status=? WHERE id=?",
            (device_id, int(time.time()), json.dumps(status, separators=(",", ":")), row["id"]),
        )
    return device_id, status


async def install_heartbeat_script(row, device_id: str):
    """Install/update only our own Shelly script; do not touch user scripts."""
    username, password = row["username"] or "", row["password"] or ""
    listed = await rpc(row["ip"], "Script.List", username=username, password=password)
    scripts = listed.get("scripts", []) if isinstance(listed, dict) else []
    script_id = None
    for script in scripts:
        if str(script.get("name") or "") == HEARTBEAT_NAME:
            script_id = int(script["id"])
            break
    if script_id is None:
        created = await rpc(row["ip"], "Script.Create", {"name": HEARTBEAT_NAME}, username, password)
        script_id = int(created["id"])
    else:
        try:
            status = await rpc(row["ip"], "Script.GetStatus", {"id": script_id}, username, password)
            if status.get("running"):
                await rpc(row["ip"], "Script.Stop", {"id": script_id}, username, password)
        except Exception:
            pass

    url = heartbeat_url(device_id)
    # Keep the code intentionally tiny; it only establishes liveness of the remote input module.
    code = f'''let URL={json.dumps(url)};\nfunction ping(){{Shelly.call("HTTP.GET",{{url:URL,timeout:5}},function(r,ec,em){{}});}}\nping();\nTimer.set({HEARTBEAT_PERIOD_MS},true,ping);\n'''
    await rpc(row["ip"], "Script.PutCode", {"id": script_id, "code": code}, username, password, timeout=8.0)
    await rpc(row["ip"], "Script.SetConfig", {"id": script_id, "config": {"enable": True}}, username, password)
    await rpc(row["ip"], "Script.Start", {"id": script_id}, username, password)
    return script_id


async def configure_device(device_db_id: int):
    with connect() as con:
        row = con.execute("SELECT * FROM shelly_devices WHERE id=?", (device_db_id,)).fetchone()
    if not row:
        raise ValueError("Shelly device not found")
    device_id, _ = await refresh_device(row)
    if not device_id:
        raise RuntimeError("Shelly device id unavailable")

    # Configure all four i4 inputs as switch inputs. Zone names and alarm rules stay in alarm-server only.
    for ch in range(4):
        await rpc(
            row["ip"], "Input.SetConfig", {"id": ch, "config": {"type": "switch"}},
            row["username"] or "", row["password"] or ""
        )

    # Remove only webhooks created by this system. Never touch unrelated Shelly automations.
    hooks = await rpc(row["ip"], "Webhook.List", username=row["username"] or "", password=row["password"] or "")
    for hook in hooks.get("hooks", []):
        if str(hook.get("name") or "").startswith("PROKOPOV ALARM"):
            await rpc(row["ip"], "Webhook.Delete", {"id": int(hook["id"])}, row["username"] or "", row["password"] or "")

    for ch in range(4):
        for state, event in [(1, "input.toggle_on"), (0, "input.toggle_off")]:
            q = urllib.parse.urlencode({"key": WEBHOOK_KEY, "device": device_id, "channel": ch, "state": state})
            url = f"{WEBHOOK_BASE}?{q}"
            await rpc(row["ip"], "Webhook.Create", {
                "event": event,
                "cid": ch,
                "enable": True,
                "name": f"PROKOPOV ALARM CH{ch} {'ON' if state else 'OFF'}",
                "urls": [url],
            }, row["username"] or "", row["password"] or "")

    heartbeat_script_id = await install_heartbeat_script(row, device_id)
    return {"ok": True, "device_id": device_id, "heartbeat_script_id": heartbeat_script_id}


async def poll_all_and_reconcile():
    snapshot = {}
    with connect() as con:
        devices = con.execute("SELECT * FROM shelly_devices WHERE enabled=1 ORDER BY id").fetchall()
        zone_rows = con.execute("""
          SELECT z.zone_key,z.channel,z.inverted,s.id AS shelly_db_id,s.device_id
          FROM zones z JOIN shelly_devices s ON s.id=z.shelly_device_id
          WHERE z.enabled=1 AND s.enabled=1 AND z.channel IS NOT NULL
        """).fetchall()
    by_device = {}
    for z in zone_rows:
        by_device.setdefault(z["shelly_db_id"], []).append(z)
    for d in devices:
        try:
            device_id, status = await refresh_device(d)
            for z in by_device.get(d["id"], []):
                item = status.get(f"input:{int(z['channel'])}") or {}
                if item.get("state") is None:
                    continue
                physical = bool(item.get("state"))
                active = (not physical) if z["inverted"] else physical
                snapshot[z["zone_key"]] = active
        except Exception as exc:
            with connect() as con:
                con.execute("UPDATE shelly_devices SET last_status=? WHERE id=?", (json.dumps({"error": str(exc)}), d["id"]))
    if snapshot:
        try:
            await controller_post("/zones/reconcile", {"zones": snapshot})
        except Exception:
            pass
    return snapshot
