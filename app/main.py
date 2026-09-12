import asyncio, json, os, secrets, time
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .db import connect, init_db, hash_password, verify_password, get_setting, set_setting, bump_revision
from .controller import controller_get, controller_post, sync_config, sync_cards, ingest_events, close_controller_client
from .shelly import configure_device, poll_all_and_reconcile

BASE = Path(__file__).resolve().parent
STATIC = BASE / "static"
SESSION_HOURS = int(os.getenv("SESSION_HOURS", "12"))
AUTO_CONFIGURE_SHELLY = os.getenv("AUTO_CONFIGURE_SHELLY", "1").lower() not in ("0", "false", "no", "off")

app = FastAPI(title="PROKOPOV Alarm Server", docs_url=None, redoc_url=None)
app.mount("/static", StaticFiles(directory=STATIC), name="static")

runtime = {"controller": None, "controller_error": None, "last_poll": None, "shelly_snapshot": {}, "ws": set(), "cards_bootstrapped": False, "shelly_autoconfig_done": False, "initial_sync_done": False}


def now(): return int(time.time())


def public_user(row):
    return {
        "id": row["id"], "username": row["username"], "display_name": row["display_name"],
        "role": row["role"], "can_arm": bool(row["can_arm"]), "can_disarm": bool(row["can_disarm"]),
        "can_door": bool(row["can_door"]), "enabled": bool(row["enabled"]),
        "must_change_password": bool(row["must_change_password"]),
    }


def get_session(request: Request, required=True):
    token = request.cookies.get("alarm_session")
    if not token:
        if required: raise HTTPException(401, "Not authenticated")
        return None
    with connect() as con:
        row = con.execute("""
          SELECT u.* FROM sessions s JOIN users u ON u.id=s.user_id
          WHERE s.token=? AND s.expires_at>? AND u.enabled=1
        """, (token, now())).fetchone()
    if not row:
        if required: raise HTTPException(401, "Session expired")
        return None
    return row


def require_admin(request: Request):
    u = get_session(request)
    if u["role"] != "Administrator": raise HTTPException(403, "Administrator required")
    return u


def check_permission(user, perm):
    if user["role"] == "Administrator": return
    col = {"arm":"can_arm", "disarm":"can_disarm", "door":"can_door"}.get(perm)
    if col and not user[col]: raise HTTPException(403, "Permission denied")


class LoginIn(BaseModel):
    username: str
    password: str

class PasswordIn(BaseModel):
    current_password: str
    new_password: str

class CommandIn(BaseModel):
    command: str

class UserIn(BaseModel):
    username: str
    display_name: str
    password: Optional[str] = None
    role: str = "User"
    can_arm: bool = True
    can_disarm: bool = True
    can_door: bool = True
    enabled: bool = True

class CardIn(BaseModel):
    uid: str
    user_id: Optional[int] = None
    label: str = ""
    enabled: bool = True
    can_unlock: bool = True
    can_arm: bool = True
    can_disarm: bool = True

class ZoneIn(BaseModel):
    zone_key: str
    name: str
    type: str = "alarm"
    shelly_device_id: Optional[int] = None
    channel: Optional[int] = None
    inverted: bool = False
    enabled: bool = True
    away_rule: str = "instant"
    home_rule: str = "ignore"
    night_rule: str = "ignore"
    sort_order: int = 0

class SettingsIn(BaseModel):
    exit_delay_s: int = 30
    entry_delay_s: int = 20
    siren_timeout_s: int = 300
    remote_device_timeout_s: int = 90

class ShellyIn(BaseModel):
    label: str
    ip: str
    username: str = ""
    password: str = ""
    enabled: bool = True


@app.on_event("startup")
async def startup():
    init_db()
    asyncio.create_task(background_loop())


@app.on_event("shutdown")
async def shutdown():
    await close_controller_client()


async def broadcast(payload):
    dead = []
    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    for ws in list(runtime["ws"]):
        try: await ws.send_text(text)
        except Exception: dead.append(ws)
    for ws in dead: runtime["ws"].discard(ws)


async def bootstrap_cards_from_controller_if_empty():
    """One-time safety import so an empty server DB can never overwrite existing controller/SD cards."""
    if runtime.get("cards_bootstrapped"):
        return
    with connect() as con:
        count = int(con.execute("SELECT COUNT(*) AS n FROM cards").fetchone()["n"])
    if count > 0:
        runtime["cards_bootstrapped"] = True
        return
    data = await controller_get("/cards", timeout=4.0)
    rows = data.get("cards") or []
    if not rows:
        runtime["cards_bootstrapped"] = True
        return
    with connect() as con:
        for c in rows:
            uid = str(c.get("uid") or "").strip()
            if not uid.isdigit():
                continue
            label = str(c.get("user_name") or "Imported from controller")
            con.execute("""
              INSERT OR IGNORE INTO cards(uid,user_id,label,enabled,can_unlock,can_arm,can_disarm,created_at)
              VALUES(?,NULL,?,?,?,?,?,?)
            """, (uid, label, int(bool(c.get("enabled", True))), int(bool(c.get("can_unlock", True))),
                  int(bool(c.get("can_arm", False))), int(bool(c.get("can_disarm", True))), now()))
    rev = int(data.get("revision") or 1)
    set_setting("cards_revision", max(1, rev))
    runtime["cards_bootstrapped"] = True


async def auto_configure_shelly_once():
    if runtime.get("shelly_autoconfig_done") or not AUTO_CONFIGURE_SHELLY:
        return
    with connect() as con:
        ids = [int(r["id"]) for r in con.execute("SELECT id FROM shelly_devices WHERE enabled=1 ORDER BY id").fetchall()]
    if not ids:
        runtime["shelly_autoconfig_done"] = True
        return
    for sid in ids:
        await configure_device(sid)
    runtime["shelly_autoconfig_done"] = True
    bump_revision("config_revision")


async def initial_controller_sync_once():
    if runtime.get("initial_sync_done") or not runtime.get("cards_bootstrapped"):
        return
    await sync_config()
    # If controller cards were imported into an empty DB, sync is now idempotent.
    await sync_cards()
    runtime["initial_sync_done"] = True


async def background_loop():
    tick = 0
    while True:
        try:
            runtime["controller"] = await controller_get("/status", timeout=2.5)
            runtime["controller_error"] = None
            runtime["last_poll"] = now()
            try: await bootstrap_cards_from_controller_if_empty()
            except Exception: pass
            if tick % 60 == 0 and not runtime.get("shelly_autoconfig_done"):
                try: await auto_configure_shelly_once()
                except Exception as exc: runtime["shelly_autoconfig_error"] = str(exc)
            if (runtime.get("shelly_autoconfig_done") or not AUTO_CONFIGURE_SHELLY) and not runtime.get("initial_sync_done"):
                try: await initial_controller_sync_once()
                except Exception as exc: runtime["initial_sync_error"] = str(exc)
            try: await ingest_events()
            except Exception: pass
        except Exception as exc:
            runtime["controller_error"] = str(exc)
            runtime["controller"] = None
        if tick % 5 == 0:
            try: runtime["shelly_snapshot"] = await poll_all_and_reconcile()
            except Exception: pass
        await broadcast({"type":"status", "data": runtime["controller"], "error": runtime["controller_error"]})
        tick += 1
        await asyncio.sleep(1)


@app.get("/")
async def index(): return FileResponse(STATIC / "index.html")

@app.get("/api/health")
async def health(): return {"ok": True, "controller_online": runtime["controller"] is not None}

@app.post("/api/login")
async def login(data: LoginIn, response: Response):
    with connect() as con:
        row = con.execute("SELECT * FROM users WHERE username=? AND enabled=1", (data.username,)).fetchone()
        if not row or not verify_password(data.password, row["password_hash"]): raise HTTPException(401, "Invalid credentials")
        token = secrets.token_urlsafe(32)
        expires = now() + SESSION_HOURS * 3600
        con.execute("DELETE FROM sessions WHERE expires_at<=?", (now(),))
        con.execute("INSERT INTO sessions(token,user_id,expires_at,created_at) VALUES(?,?,?,?)", (token, row["id"], expires, now()))
    response.set_cookie("alarm_session", token, max_age=SESSION_HOURS*3600, httponly=True, samesite="strict", secure=False)
    return {"ok": True, "user": public_user(row)}

@app.post("/api/logout")
async def logout(request: Request, response: Response):
    token = request.cookies.get("alarm_session")
    if token:
        with connect() as con: con.execute("DELETE FROM sessions WHERE token=?", (token,))
    response.delete_cookie("alarm_session")
    return {"ok": True}

@app.get("/api/me")
async def me(request: Request): return {"user": public_user(get_session(request))}

@app.post("/api/password")
async def password(data: PasswordIn, request: Request):
    u = get_session(request)
    if len(data.new_password) < 10: raise HTTPException(400, "Password must be at least 10 characters")
    if not verify_password(data.current_password, u["password_hash"]): raise HTTPException(400, "Current password is incorrect")
    with connect() as con:
        con.execute("UPDATE users SET password_hash=?,must_change_password=0 WHERE id=?", (hash_password(data.new_password), u["id"]))
    return {"ok": True}

@app.get("/api/status")
async def status(request: Request):
    get_session(request)
    return {"controller": runtime["controller"], "error": runtime["controller_error"], "last_poll": runtime["last_poll"]}

@app.post("/api/alarm/command")
async def alarm_command(data: CommandIn, request: Request):
    u = get_session(request)
    cmd = data.command
    if cmd.startswith("arm_"): check_permission(u, "arm")
    elif cmd in ("disarm","clear_alarm","siren_off","silence"): check_permission(u, "disarm")
    elif cmd in ("lock","unlock","lock_door","unlock_door"): check_permission(u, "door")
    if cmd in ("panic","silent_panic") and not u["can_disarm"] and u["role"] != "Administrator":
        raise HTTPException(403, "Panic permission denied")
    try:
        return await controller_post("/command", {"command": cmd, "actor": u["username"]})
    except Exception as exc:
        raise HTTPException(503, f"Controller unavailable: {exc}")

@app.get("/api/users")
async def users(request: Request):
    require_admin(request)
    with connect() as con: rows = con.execute("SELECT * FROM users ORDER BY id").fetchall()
    return {"users": [public_user(r) for r in rows]}

@app.post("/api/users")
async def add_user(data: UserIn, request: Request):
    require_admin(request)
    if not data.password or len(data.password) < 10: raise HTTPException(400, "Initial password must be at least 10 characters")
    with connect() as con:
        try:
            cur = con.execute("""INSERT INTO users(username,display_name,password_hash,role,can_arm,can_disarm,can_door,enabled,must_change_password,created_at)
              VALUES(?,?,?,?,?,?,?,?,1,?)""", (data.username.strip(), data.display_name.strip(), hash_password(data.password), data.role,
              int(data.can_arm), int(data.can_disarm), int(data.can_door), int(data.enabled), now()))
        except Exception as exc: raise HTTPException(400, str(exc))
    return {"ok": True, "id": cur.lastrowid}

@app.put("/api/users/{user_id}")
async def update_user(user_id: int, data: UserIn, request: Request):
    require_admin(request)
    with connect() as con:
        row = con.execute("SELECT * FROM users WHERE id=?", (user_id,)).fetchone()
        if not row: raise HTTPException(404, "User not found")
        ph = row["password_hash"]
        if data.password:
            if len(data.password) < 10: raise HTTPException(400, "Password too short")
            ph = hash_password(data.password)
        con.execute("""UPDATE users SET username=?,display_name=?,password_hash=?,role=?,can_arm=?,can_disarm=?,can_door=?,enabled=? WHERE id=?""",
                    (data.username, data.display_name, ph, data.role, int(data.can_arm), int(data.can_disarm), int(data.can_door), int(data.enabled), user_id))
    return {"ok": True}

@app.delete("/api/users/{user_id}")
async def delete_user(user_id: int, request: Request):
    me = require_admin(request)
    if me["id"] == user_id: raise HTTPException(400, "Cannot delete current administrator")
    with connect() as con: con.execute("DELETE FROM users WHERE id=?", (user_id,))
    bump_revision("cards_revision")
    try: await sync_cards()
    except Exception: pass
    return {"ok": True}

@app.get("/api/cards")
async def cards(request: Request):
    require_admin(request)
    with connect() as con:
        rows = con.execute("""SELECT c.*,u.username,u.display_name FROM cards c LEFT JOIN users u ON u.id=c.user_id ORDER BY c.id""").fetchall()
    return {"cards": [dict(r) for r in rows]}

@app.post("/api/cards")
async def add_card(data: CardIn, request: Request):
    require_admin(request)
    uid = data.uid.strip()
    if not uid.isdigit(): raise HTTPException(400, "UID must be numeric")
    with connect() as con:
        try:
            con.execute("""INSERT INTO cards(uid,user_id,label,enabled,can_unlock,can_arm,can_disarm,created_at) VALUES(?,?,?,?,?,?,?,?)""",
                        (uid, data.user_id, data.label, int(data.enabled), int(data.can_unlock), int(data.can_arm), int(data.can_disarm), now()))
        except Exception as exc: raise HTTPException(400, str(exc))
    bump_revision("cards_revision")
    try: await sync_cards()
    except Exception: pass
    return {"ok": True}

@app.put("/api/cards/{card_id}")
async def update_card(card_id: int, data: CardIn, request: Request):
    require_admin(request)
    with connect() as con:
        con.execute("""UPDATE cards SET uid=?,user_id=?,label=?,enabled=?,can_unlock=?,can_arm=?,can_disarm=? WHERE id=?""",
                    (data.uid.strip(), data.user_id, data.label, int(data.enabled), int(data.can_unlock), int(data.can_arm), int(data.can_disarm), card_id))
    bump_revision("cards_revision")
    try: await sync_cards()
    except Exception: pass
    return {"ok": True}

@app.delete("/api/cards/{card_id}")
async def delete_card(card_id: int, request: Request):
    require_admin(request)
    with connect() as con: con.execute("DELETE FROM cards WHERE id=?", (card_id,))
    bump_revision("cards_revision")
    try: await sync_cards()
    except Exception: pass
    return {"ok": True}

@app.post("/api/cards/enroll/start")
async def enroll_start(request: Request):
    require_admin(request)
    return await controller_post("/card-scan/begin", {})

@app.get("/api/cards/enroll/status")
async def enroll_status(request: Request):
    require_admin(request)
    return await controller_get("/card-scan/status")

@app.post("/api/cards/enroll/stop")
async def enroll_stop(request: Request):
    require_admin(request)
    return await controller_post("/card-scan/stop", {})

@app.get("/api/zones")
async def zones(request: Request):
    require_admin(request)
    with connect() as con:
        rows = con.execute("""SELECT z.*,s.label AS shelly_label,s.device_id FROM zones z LEFT JOIN shelly_devices s ON s.id=z.shelly_device_id ORDER BY z.sort_order,z.id""").fetchall()
    return {"zones": [dict(r) for r in rows]}

@app.post("/api/zones")
async def add_zone(data: ZoneIn, request: Request):
    require_admin(request)
    with connect() as con:
        try:
            con.execute("""INSERT INTO zones(zone_key,name,type,shelly_device_id,channel,inverted,enabled,away_rule,home_rule,night_rule,sort_order)
              VALUES(?,?,?,?,?,?,?,?,?,?,?)""", (data.zone_key,data.name,data.type,data.shelly_device_id,data.channel,int(data.inverted),int(data.enabled),data.away_rule,data.home_rule,data.night_rule,data.sort_order))
        except Exception as exc: raise HTTPException(400, str(exc))
    bump_revision("config_revision")
    try: await sync_config()
    except Exception: pass
    return {"ok": True}

@app.put("/api/zones/{zone_id}")
async def update_zone(zone_id: int, data: ZoneIn, request: Request):
    require_admin(request)
    with connect() as con:
        con.execute("""UPDATE zones SET zone_key=?,name=?,type=?,shelly_device_id=?,channel=?,inverted=?,enabled=?,away_rule=?,home_rule=?,night_rule=?,sort_order=? WHERE id=?""",
                    (data.zone_key,data.name,data.type,data.shelly_device_id,data.channel,int(data.inverted),int(data.enabled),data.away_rule,data.home_rule,data.night_rule,data.sort_order,zone_id))
    bump_revision("config_revision")
    try: await sync_config()
    except Exception: pass
    return {"ok": True}

@app.delete("/api/zones/{zone_id}")
async def delete_zone(zone_id: int, request: Request):
    require_admin(request)
    with connect() as con: con.execute("DELETE FROM zones WHERE id=?", (zone_id,))
    bump_revision("config_revision")
    try: await sync_config()
    except Exception: pass
    return {"ok": True}

@app.get("/api/settings")
async def settings(request: Request):
    require_admin(request)
    return {"exit_delay_s": int(get_setting("exit_delay_s",30)), "entry_delay_s": int(get_setting("entry_delay_s",20)), "siren_timeout_s": int(get_setting("siren_timeout_s",300)), "remote_device_timeout_s": int(get_setting("remote_device_timeout_s",90))}

@app.put("/api/settings")
async def update_settings(data: SettingsIn, request: Request):
    require_admin(request)
    if not (0 <= data.exit_delay_s <= 300 and 0 <= data.entry_delay_s <= 300 and 0 <= data.siren_timeout_s <= 1800 and 30 <= data.remote_device_timeout_s <= 600): raise HTTPException(400, "Timing out of range")
    set_setting("exit_delay_s", data.exit_delay_s); set_setting("entry_delay_s", data.entry_delay_s); set_setting("siren_timeout_s", data.siren_timeout_s); set_setting("remote_device_timeout_s", data.remote_device_timeout_s)
    bump_revision("config_revision")
    try: await sync_config()
    except Exception: pass
    return {"ok": True}

@app.post("/api/sync/all")
async def sync_all(request: Request):
    require_admin(request)
    out = {}
    try: out["config"] = await sync_config()
    except Exception as e: out["config_error"] = str(e)
    try: out["cards"] = await sync_cards()
    except Exception as e: out["cards_error"] = str(e)
    return out

@app.get("/api/shelly")
async def shelly_list(request: Request):
    require_admin(request)
    with connect() as con: rows = con.execute("SELECT id,label,ip,device_id,username,enabled,last_seen,last_status FROM shelly_devices ORDER BY id").fetchall()
    return {"devices": [dict(r) for r in rows]}

@app.post("/api/shelly")
async def shelly_add(data: ShellyIn, request: Request):
    require_admin(request)
    with connect() as con:
        try:
            cur = con.execute("INSERT INTO shelly_devices(label,ip,username,password,enabled,created_at) VALUES(?,?,?,?,?,?)",
                              (data.label,data.ip,data.username,data.password,int(data.enabled),now()))
        except Exception as exc: raise HTTPException(400, str(exc))
    return {"ok": True, "id": cur.lastrowid}

@app.put("/api/shelly/{device_id}")
async def shelly_update(device_id: int, data: ShellyIn, request: Request):
    require_admin(request)
    with connect() as con:
        if data.password:
            con.execute("UPDATE shelly_devices SET label=?,ip=?,username=?,password=?,enabled=? WHERE id=?",
                        (data.label,data.ip,data.username,data.password,int(data.enabled),device_id))
        else:
            con.execute("UPDATE shelly_devices SET label=?,ip=?,username=?,enabled=? WHERE id=?",
                        (data.label,data.ip,data.username,int(data.enabled),device_id))
    return {"ok": True}

@app.delete("/api/shelly/{device_id}")
async def shelly_delete(device_id: int, request: Request):
    require_admin(request)
    with connect() as con: con.execute("DELETE FROM shelly_devices WHERE id=?", (device_id,))
    bump_revision("config_revision")
    try: await sync_config()
    except Exception: pass
    return {"ok": True}

@app.post("/api/shelly/configure-all")
async def shelly_configure_all(request: Request):
    require_admin(request)
    with connect() as con:
        ids = [int(r["id"]) for r in con.execute("SELECT id FROM shelly_devices WHERE enabled=1 ORDER BY id").fetchall()]
    results = []
    errors = []
    for sid in ids:
        try:
            results.append(await configure_device(sid))
        except Exception as exc:
            errors.append({"id": sid, "error": str(exc)})
    bump_revision("config_revision")
    try:
        await sync_config()
    except Exception as exc:
        errors.append({"controller": str(exc)})
    return {"ok": not errors, "configured": results, "errors": errors}


@app.post("/api/shelly/{device_id}/configure")
async def shelly_configure(device_id: int, request: Request):
    require_admin(request)
    try:
        result = await configure_device(device_id)
        bump_revision("config_revision")
        try: await sync_config()
        except Exception: pass
        return result
    except Exception as exc: raise HTTPException(502, str(exc))

@app.get("/api/history")
async def history(request: Request, limit: int = 200, event_type: str = "", search: str = ""):
    get_session(request)
    limit = max(1,min(1000,limit))
    q = "SELECT * FROM events WHERE 1=1"; args=[]
    if event_type: q += " AND type=?"; args.append(event_type)
    if search:
        q += " AND (source LIKE ? OR event LIKE ? OR actor LIKE ?)"; pat=f"%{search}%"; args += [pat,pat,pat]
    q += " ORDER BY event_time DESC,id DESC LIMIT ?"; args.append(limit)
    with connect() as con: rows=con.execute(q,args).fetchall()
    return {"events":[dict(r) for r in rows]}

@app.delete("/api/history")
async def history_clear(request: Request):
    require_admin(request)
    with connect() as con: con.execute("DELETE FROM events")
    return {"ok": True}

@app.get("/api/hardware")
async def hardware(request: Request):
    get_session(request)
    with connect() as con:
        sh=[dict(r) for r in con.execute("SELECT id,label,ip,device_id,enabled,last_seen,last_status FROM shelly_devices ORDER BY id")]
    return {"controller":runtime["controller"],"controller_error":runtime["controller_error"],"shelly":sh}

@app.websocket("/ws")
async def ws(websocket: WebSocket):
    token = websocket.cookies.get("alarm_session")
    with connect() as con:
        row = con.execute("SELECT u.id FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token=? AND s.expires_at>? AND u.enabled=1", (token,now())).fetchone() if token else None
    if not row:
        await websocket.close(code=4401); return
    await websocket.accept(); runtime["ws"].add(websocket)
    try:
        await websocket.send_json({"type":"status","data":runtime["controller"],"error":runtime["controller_error"]})
        while True: await websocket.receive_text()
    except WebSocketDisconnect: pass
    finally: runtime["ws"].discard(websocket)
