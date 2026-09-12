import os, httpx, time
from .db import connect, get_setting, set_setting

CONTROLLER_URL = os.getenv("CONTROLLER_URL", "http://192.168.0.218:8088").rstrip("/")
CONTROLLER_KEY = os.getenv("CONTROLLER_KEY", "")


def headers():
    return {"X-Alarm-Key": CONTROLLER_KEY, "Content-Type": "application/json"}


_controller_client = None


def _get_controller_client():
    global _controller_client
    if _controller_client is None:
        _controller_client = httpx.AsyncClient(
            timeout=httpx.Timeout(5.0),
            limits=httpx.Limits(
                max_connections=4,
                max_keepalive_connections=2,
                keepalive_expiry=30.0,
            ),
            headers=headers(),
        )
    return _controller_client


async def close_controller_client():
    global _controller_client
    if _controller_client is not None:
        await _controller_client.aclose()
        _controller_client = None


async def controller_get(path: str, params=None, timeout=3.0):
    client = _get_controller_client()
    r = await client.get(CONTROLLER_URL + path, params=params, timeout=timeout)
    r.raise_for_status()
    return r.json()


async def controller_post(path: str, payload=None, timeout=4.0):
    client = _get_controller_client()
    r = await client.post(CONTROLLER_URL + path, json=payload or {}, timeout=timeout)
    if r.is_error:
        detail = r.text.strip()[:500]
        raise RuntimeError(f"controller {path} -> HTTP {r.status_code}: {detail}")
    return r.json()


async def controller_put(path: str, payload=None, timeout=5.0):
    client = _get_controller_client()
    r = await client.put(CONTROLLER_URL + path, json=payload or {}, timeout=timeout)
    if r.is_error:
        detail = r.text.strip()[:500]
        raise RuntimeError(f"controller {path} -> HTTP {r.status_code}: {detail}")
    return r.json()


def build_controller_config():
    with connect() as con:
        settings = {r["key"]: r["value"] for r in con.execute("SELECT key,value FROM settings")}
        rows = con.execute("""
          SELECT z.*, s.device_id
          FROM zones z LEFT JOIN shelly_devices s ON s.id=z.shelly_device_id
          ORDER BY z.sort_order,z.id
        """).fetchall()
        zones = []
        for r in rows:
            zones.append({
                "id": r["zone_key"],
                "name": r["name"],
                "device": r["device_id"] or "",
                "channel": r["channel"] if r["channel"] is not None else -1,
                "enabled": bool(r["enabled"]),
                "inverted": bool(r["inverted"]),
                "away": r["away_rule"],
                "home": r["home_rule"],
                "night": r["night_rule"],
            })
        return {
            "revision": int(settings.get("config_revision", 1)),
            "timings": {
                "exit_delay_s": int(settings.get("exit_delay_s", 30)),
                "entry_delay_s": int(settings.get("entry_delay_s", 20)),
                "siren_timeout_s": int(settings.get("siren_timeout_s", 300)),
                "remote_device_timeout_s": int(settings.get("remote_device_timeout_s", 90)),
            },
            "zones": zones,
        }


def build_controller_cards():
    with connect() as con:
        rev = int(get_setting("cards_revision", 1))
        rows = con.execute("""
          SELECT c.*, u.username, u.display_name
          FROM cards c LEFT JOIN users u ON u.id=c.user_id ORDER BY c.id
        """).fetchall()
        return {
            "revision": rev,
            "cards": [{
                "uid": r["uid"],
                "user_name": r["display_name"] or r["username"] or r["label"] or "",
                "enabled": bool(r["enabled"]),
                "can_unlock": bool(r["can_unlock"]),
                "can_arm": bool(r["can_arm"]),
                "can_disarm": bool(r["can_disarm"]),
            } for r in rows]
        }


async def sync_config():
    return await controller_put("/config", build_controller_config())


async def sync_cards():
    return await controller_put("/cards", build_controller_cards())


async def ingest_events():
    last_seq = int(get_setting("last_event_seq", 0) or 0)
    data = await controller_get("/events", {"after": last_seq}, timeout=6.0)
    events = data.get("events") or []
    max_seq = last_seq
    with connect() as con:
        for e in events:
            seq = int(e.get("seq") or 0)
            epoch = int(e.get("epoch") or 0)
            when = epoch if epoch > 0 else int(time.time())
            con.execute("""
              INSERT OR IGNORE INTO events(controller_seq,event_time,type,source,event,actor,raw,received_at)
              VALUES(?,?,?,?,?,?,?,?)
            """, (seq or None, when, str(e.get("type") or "SYSTEM"), str(e.get("source") or "Controller"),
                  str(e.get("event") or ""), str(e.get("actor") or "system"), str(e), int(time.time())))
            max_seq = max(max_seq, seq)
    if max_seq > last_seq: set_setting("last_event_seq", max_seq)
    return len(events)
