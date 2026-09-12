import os, sqlite3, hashlib, hmac, secrets, time
from contextlib import contextmanager

DB_PATH = os.getenv("ALARM_DB", "/data/alarm.db")

SCHEMA = """
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  username TEXT UNIQUE NOT NULL,
  display_name TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  role TEXT NOT NULL DEFAULT 'User',
  can_arm INTEGER NOT NULL DEFAULT 1,
  can_disarm INTEGER NOT NULL DEFAULT 1,
  can_door INTEGER NOT NULL DEFAULT 1,
  enabled INTEGER NOT NULL DEFAULT 1,
  must_change_password INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
  token TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  expires_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS shelly_devices (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  label TEXT NOT NULL,
  ip TEXT UNIQUE NOT NULL,
  device_id TEXT,
  username TEXT DEFAULT '',
  password TEXT DEFAULT '',
  enabled INTEGER NOT NULL DEFAULT 1,
  last_seen INTEGER,
  last_status TEXT,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS zones (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  zone_key TEXT UNIQUE NOT NULL,
  name TEXT NOT NULL,
  type TEXT NOT NULL DEFAULT 'alarm',
  shelly_device_id INTEGER REFERENCES shelly_devices(id) ON DELETE SET NULL,
  channel INTEGER,
  inverted INTEGER NOT NULL DEFAULT 0,
  enabled INTEGER NOT NULL DEFAULT 1,
  away_rule TEXT NOT NULL DEFAULT 'instant',
  home_rule TEXT NOT NULL DEFAULT 'ignore',
  night_rule TEXT NOT NULL DEFAULT 'ignore',
  sort_order INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS cards (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  uid TEXT UNIQUE NOT NULL,
  user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
  label TEXT DEFAULT '',
  enabled INTEGER NOT NULL DEFAULT 1,
  can_unlock INTEGER NOT NULL DEFAULT 1,
  can_arm INTEGER NOT NULL DEFAULT 1,
  can_disarm INTEGER NOT NULL DEFAULT 1,
  last_used INTEGER,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  controller_seq INTEGER UNIQUE,
  event_time INTEGER NOT NULL,
  type TEXT NOT NULL,
  source TEXT NOT NULL,
  event TEXT NOT NULL,
  actor TEXT NOT NULL,
  raw TEXT,
  received_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_events_time ON events(event_time DESC);
CREATE INDEX IF NOT EXISTS idx_cards_user ON cards(user_id);
CREATE INDEX IF NOT EXISTS idx_zones_shelly ON zones(shelly_device_id, channel);
"""

DEFAULT_ZONES = [
    ("pir_entrance", "PIR входна врата / антре", "pir_entry", "instant", "ignore", "ignore", 10),
    ("pir_dining", "PIR трапезария", "pir", "instant", "ignore", "ignore", 20),
    ("pir_living", "PIR хол", "pir", "instant", "ignore", "ignore", 30),
    ("pir_office", "PIR кабинет", "pir", "instant", "ignore", "ignore", 40),
    ("perimeter", "Периметрова охрана", "perimeter", "instant", "ignore", "instant", 50),
    ("pir_summer_kitchen", "PIR лятна кухня", "pir_external", "instant", "ignore", "instant", 60),
    ("pir_shed", "PIR барака", "pir_external", "instant", "ignore", "instant", 70),
    ("spare_input", "Свободен вход", "unused", "ignore", "ignore", "ignore", 80),
]

# Physical mapping recovered from the earlier working controller files.
DEFAULT_SHELLIES = [
    ("Shelly i4 вътрешни зони", "192.168.0.216", "f8b3b7fb52cc"),
    ("Shelly i4 външни зони", "192.168.0.26", "f8b3b7fb30b4"),
]
DEFAULT_ZONE_MAPPING = {
    "pir_entrance": ("192.168.0.216", 0),
    "pir_dining": ("192.168.0.216", 1),
    "pir_living": ("192.168.0.216", 2),
    "pir_office": ("192.168.0.216", 3),
    "perimeter": ("192.168.0.26", 0),
    "pir_summer_kitchen": ("192.168.0.26", 1),
    "pir_shed": ("192.168.0.26", 2),
    "spare_input": ("192.168.0.26", 3),
}


DEFAULT_SETTINGS = {
    "exit_delay_s": "30",
    "entry_delay_s": "20",
    "siren_timeout_s": "300",
    "remote_device_timeout_s": "90",
    "config_revision": "1",
    "cards_revision": "1",
    "last_event_seq": "0",
}


def _pbkdf(password: str, salt: bytes, rounds: int = 260000) -> bytes:
    return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds)


def hash_password(password: str) -> str:
    salt = secrets.token_bytes(16)
    digest = _pbkdf(password, salt)
    return f"pbkdf2_sha256$260000${salt.hex()}${digest.hex()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        alg, rounds_s, salt_h, dig_h = stored.split("$", 3)
        if alg != "pbkdf2_sha256": return False
        got = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes.fromhex(salt_h), int(rounds_s))
        return hmac.compare_digest(got.hex(), dig_h)
    except Exception:
        return False


@contextmanager
def connect():
    con = sqlite3.connect(DB_PATH, timeout=10, check_same_thread=False)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA foreign_keys=ON")
    try:
        yield con
        con.commit()
    finally:
        con.close()


def init_db():
    os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
    with connect() as con:
        con.executescript(SCHEMA)
        for k, v in DEFAULT_SETTINGS.items():
            con.execute("INSERT OR IGNORE INTO settings(key,value) VALUES(?,?)", (k, v))
        for label, ip, device_id in DEFAULT_SHELLIES:
            con.execute("""
              INSERT OR IGNORE INTO shelly_devices(label,ip,device_id,enabled,created_at)
              VALUES(?,?,?,1,?)
            """, (label, ip, device_id, int(time.time())))

        for zone_key, name, typ, away, home, night, sort_order in DEFAULT_ZONES:
            con.execute("""
              INSERT OR IGNORE INTO zones(zone_key,name,type,away_rule,home_rule,night_rule,sort_order)
              VALUES(?,?,?,?,?,?,?)
            """, (zone_key, name, typ, away, home, night, sort_order))
            # Upgrade machine-name defaults without overwriting names edited by the user.
            con.execute("UPDATE zones SET name=?,type=? WHERE zone_key=? AND name=?", (name, typ, zone_key, zone_key))
            if zone_key == "spare_input":
                con.execute("UPDATE zones SET enabled=0 WHERE zone_key=? AND type='unused'", (zone_key,))

        for zone_key, (ip, channel) in DEFAULT_ZONE_MAPPING.items():
            row = con.execute("SELECT id FROM shelly_devices WHERE ip=?", (ip,)).fetchone()
            if row:
                con.execute("""
                  UPDATE zones SET shelly_device_id=?,channel=?
                  WHERE zone_key=? AND shelly_device_id IS NULL
                """, (row["id"], channel, zone_key))

        admin_user = os.getenv("ALARM_ADMIN_USER", "admin")
        admin_pass = os.getenv("ALARM_ADMIN_PASSWORD", "")
        exists = con.execute("SELECT id FROM users WHERE username=?", (admin_user,)).fetchone()
        if not exists:
            if not admin_pass:
                admin_pass = secrets.token_urlsafe(18)
                print("[SECURITY] Generated temporary admin password:", admin_pass, flush=True)
            con.execute("""
              INSERT INTO users(username,display_name,password_hash,role,can_arm,can_disarm,can_door,enabled,must_change_password,created_at)
              VALUES(?,?,?,?,1,1,1,1,1,?)
            """, (admin_user, "Administrator", hash_password(admin_pass), "Administrator", int(time.time())))


def get_setting(key: str, default=None):
    with connect() as con:
        row = con.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        return row["value"] if row else default


def set_setting(key: str, value):
    with connect() as con:
        con.execute("INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, str(value)))


def bump_revision(key: str) -> int:
    with connect() as con:
        row = con.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        val = int(row["value"] if row else 0) + 1
        con.execute("INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, str(val)))
        return val
