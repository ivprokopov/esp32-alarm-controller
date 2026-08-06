import ipaddress
import json
import os
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt_client
import paho.mqtt.publish as mqtt_publish
from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

DB_PATH = os.getenv("ALARM_DB_PATH", "/data/alarm.db")
MQTT_HOST = os.getenv("MQTT_HOST", "mqtt")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
API_TOKEN = os.getenv("API_TOKEN", "")
STATIC_DIR = Path(__file__).parent / "static"

app = FastAPI(title="Olimex Alarm Server", version="0.3.2-lan-testing")


@contextmanager
def db():
    connection = sqlite3.connect(DB_PATH, timeout=3)
    connection.row_factory = sqlite3.Row
    try:
        yield connection
        connection.commit()
    finally:
        connection.close()


def init_db() -> None:
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    with db() as connection:
        connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS configuration (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                version INTEGER NOT NULL,
                payload TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS cards (
                uid TEXT PRIMARY KEY,
                user_name TEXT NOT NULL,
                can_unlock INTEGER NOT NULL DEFAULT 0,
                can_arm INTEGER NOT NULL DEFAULT 0,
                can_disarm INTEGER NOT NULL DEFAULT 0,
                enabled INTEGER NOT NULL DEFAULT 1,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                category TEXT NOT NULL,
                code TEXT NOT NULL,
                level TEXT NOT NULL,
                payload TEXT NOT NULL,
                created_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS device_status (
                device_id TEXT PRIMARY KEY,
                payload TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
            """
        )


def store_status(device_id: str, payload: dict[str, Any]) -> None:
    now = int(time.time())
    with db() as connection:
        connection.execute(
            """
            INSERT INTO device_status(device_id, payload, updated_at)
            VALUES(?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                payload=excluded.payload,
                updated_at=excluded.updated_at
            """,
            (device_id, json.dumps(payload, ensure_ascii=False), now),
        )


def on_mqtt_connect(client, userdata, flags, reason_code, properties=None):
    client.subscribe("alarm/+/status", qos=1)


def on_mqtt_message(client, userdata, message):
    try:
        parts = message.topic.split("/")
        if len(parts) == 3 and parts[0] == "alarm" and parts[2] == "status":
            store_status(parts[1], json.loads(message.payload.decode("utf-8")))
    except Exception as exc:
        print("MQTT status ingest error:", exc)


@app.on_event("startup")
def startup() -> None:
    init_db()
    client = mqtt_client.Client(
        mqtt_client.CallbackAPIVersion.VERSION2,
        client_id="alarm-api-status-listener",
    )
    client.on_connect = on_mqtt_connect
    client.on_message = on_mqtt_message
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()
    app.state.mqtt_client = client


@app.on_event("shutdown")
def shutdown() -> None:
    client = getattr(app.state, "mqtt_client", None)
    if client is not None:
        client.loop_stop()
        client.disconnect()


def is_private_client(request: Request) -> bool:
    host = request.client.host if request.client else ""
    try:
        address = ipaddress.ip_address(host)
        return address.is_private or address.is_loopback
    except ValueError:
        return False


def require_token(
    request: Request,
    x_api_token: str | None = Header(default=None),
) -> None:
    # During local testing, devices on the private LAN may use the panel
    # without entering the API token. External clients still require it.
    if is_private_client(request):
        return
    if API_TOKEN and x_api_token == API_TOKEN:
        return
    raise HTTPException(status_code=401, detail="Authentication required")


class CardRecord(BaseModel):
    uid: str = Field(min_length=4, max_length=64)
    user_name: str = Field(min_length=1, max_length=100)
    can_unlock: bool = False
    can_arm: bool = False
    can_disarm: bool = False
    enabled: bool = True


class EventRecord(BaseModel):
    device_id: str
    category: str
    code: str
    level: str = "info"
    payload: dict[str, Any] = {}
    created_at: int | None = None


class CommandRecord(BaseModel):
    command: str
    payload: dict[str, Any] = {}


@app.get("/", include_in_schema=False)
def dashboard():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/health")
def health() -> dict[str, Any]:
    return {"status": "ok", "service": "alarm-api", "time": int(time.time())}


@app.get("/api/v1/config", dependencies=[Depends(require_token)])
def get_config() -> dict[str, Any]:
    with db() as connection:
        row = connection.execute(
            "SELECT version, payload, updated_at FROM configuration WHERE id = 1"
        ).fetchone()
    if row is None:
        return {"version": 0, "config": {}, "updated_at": 0}
    return {"version": row["version"], "config": json.loads(row["payload"]), "updated_at": row["updated_at"]}


@app.put("/api/v1/config", dependencies=[Depends(require_token)])
def put_config(config: dict[str, Any]) -> dict[str, Any]:
    now = int(time.time())
    with db() as connection:
        row = connection.execute("SELECT version FROM configuration WHERE id = 1").fetchone()
        version = (row["version"] if row else 0) + 1
        connection.execute(
            """
            INSERT INTO configuration(id, version, payload, updated_at)
            VALUES(1, ?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET
                version=excluded.version,
                payload=excluded.payload,
                updated_at=excluded.updated_at
            """,
            (version, json.dumps(config, ensure_ascii=False), now),
        )
    return {"saved": True, "version": version, "updated_at": now}


@app.get("/api/v1/cards", dependencies=[Depends(require_token)])
def list_cards() -> dict[str, Any]:
    with db() as connection:
        rows = connection.execute(
            """
            SELECT uid, user_name, can_unlock, can_arm, can_disarm, enabled, updated_at
            FROM cards ORDER BY user_name, uid
            """
        ).fetchall()
    cards = [{
        "uid": row["uid"],
        "user_name": row["user_name"],
        "can_unlock": bool(row["can_unlock"]),
        "can_arm": bool(row["can_arm"]),
        "can_disarm": bool(row["can_disarm"]),
        "enabled": bool(row["enabled"]),
        "updated_at": row["updated_at"],
    } for row in rows]
    return {"version": max((c["updated_at"] for c in cards), default=0), "cards": cards}


@app.put("/api/v1/cards/{uid}", dependencies=[Depends(require_token)])
def upsert_card(uid: str, card: CardRecord) -> dict[str, Any]:
    if uid != card.uid:
        raise HTTPException(status_code=400, detail="UID mismatch")
    now = int(time.time())
    with db() as connection:
        connection.execute(
            """
            INSERT INTO cards(uid, user_name, can_unlock, can_arm, can_disarm, enabled, updated_at)
            VALUES(?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(uid) DO UPDATE SET
                user_name=excluded.user_name,
                can_unlock=excluded.can_unlock,
                can_arm=excluded.can_arm,
                can_disarm=excluded.can_disarm,
                enabled=excluded.enabled,
                updated_at=excluded.updated_at
            """,
            (card.uid, card.user_name, int(card.can_unlock), int(card.can_arm), int(card.can_disarm), int(card.enabled), now),
        )
    return {"saved": True, "uid": uid, "updated_at": now}


@app.post("/api/v1/events", dependencies=[Depends(require_token)])
def add_event(event: EventRecord) -> dict[str, Any]:
    created_at = event.created_at or int(time.time())
    with db() as connection:
        cursor = connection.execute(
            """
            INSERT INTO events(device_id, category, code, level, payload, created_at)
            VALUES(?, ?, ?, ?, ?, ?)
            """,
            (event.device_id, event.category, event.code, event.level, json.dumps(event.payload, ensure_ascii=False), created_at),
        )
    return {"stored": True, "event_id": cursor.lastrowid}


@app.get("/api/v1/events", dependencies=[Depends(require_token)])
def list_events(limit: int = Query(default=20, ge=1, le=200)) -> dict[str, Any]:
    with db() as connection:
        rows = connection.execute(
            """
            SELECT id, device_id, category, code, level, payload, created_at
            FROM events ORDER BY id DESC LIMIT ?
            """,
            (limit,),
        ).fetchall()
    return {"events": [{
        "id": row["id"],
        "device_id": row["device_id"],
        "category": row["category"],
        "code": row["code"],
        "level": row["level"],
        "payload": json.loads(row["payload"]),
        "created_at": row["created_at"],
    } for row in rows]}


@app.post("/api/v1/devices/{device_id}/status", dependencies=[Depends(require_token)])
def set_status(device_id: str, status: dict[str, Any]) -> dict[str, Any]:
    store_status(device_id, status)
    return {"stored": True, "updated_at": int(time.time())}


@app.get("/api/v1/devices/{device_id}/status", dependencies=[Depends(require_token)])
def get_status(device_id: str) -> dict[str, Any]:
    with db() as connection:
        row = connection.execute(
            "SELECT payload, updated_at FROM device_status WHERE device_id = ?",
            (device_id,),
        ).fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail="Device status not found")
    payload = json.loads(row["payload"])
    payload["server_received_at"] = row["updated_at"]
    return payload


@app.post("/api/v1/devices/{device_id}/command", dependencies=[Depends(require_token)])
def send_command(device_id: str, command: CommandRecord) -> dict[str, Any]:
    topic = f"alarm/{device_id}/command"
    message = {"command": command.command, "payload": command.payload, "created_at": int(time.time())}
    try:
        mqtt_publish.single(
            topic,
            payload=json.dumps(message),
            hostname=MQTT_HOST,
            port=MQTT_PORT,
            qos=1,
            retain=False,
        )
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"MQTT unavailable: {exc}") from exc
    return {"published": True, "topic": topic, "command": command.command}
