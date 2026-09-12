#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="/opt/docker/alarm/prokopov-alarm-server"
BRANCH="alarm-production"
HEALTH_URL="http://127.0.0.1:8099/api/health"

cd "$ROOT"

echo "===== PROKOPOV ALARM UPDATE ====="

# Не обновяваме върху ръчно променени файлове.
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "ERROR: Local tracked files have uncommitted changes."
    git status --short
    exit 1
fi

OLD_REV="$(git rev-parse HEAD)"
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP_DIR="$ROOT/backups/$STAMP"

mkdir -p "$BACKUP_DIR"

# Backup на secrets/config.
if [ -f .env ]; then
    cp -a .env "$BACKUP_DIR/.env"
fi

# Консистентен SQLite backup.
if [ -f data/alarm.db ]; then
    python3 - "$ROOT/data/alarm.db" "$BACKUP_DIR/alarm.db" <<'PY'
import sqlite3
import sys

src, dst = sys.argv[1], sys.argv[2]

source = sqlite3.connect(src)
target = sqlite3.connect(dst)

with target:
    source.backup(target)

target.close()
source.close()
PY
fi

echo
echo "Current revision: $OLD_REV"
echo "Backup: $BACKUP_DIR"

echo
echo "===== FETCH ====="
git fetch origin "$BRANCH"

NEW_REV="$(git rev-parse "origin/$BRANCH")"

if [ "$OLD_REV" = "$NEW_REV" ]; then
    echo "Already up to date."
    sudo docker compose ps
    exit 0
fi

echo "New revision: $NEW_REV"

echo
echo "===== APPLY UPDATE ====="
git merge --ff-only "origin/$BRANCH"

echo
echo "===== BUILD / RESTART ====="
if ! sudo docker compose up -d --build; then
    echo "ERROR: Docker build/start failed. Rolling back."
    git reset --hard "$OLD_REV"
    sudo docker compose up -d --build
    exit 1
fi

echo
echo "===== HEALTH CHECK ====="
for i in $(seq 1 12); do
    if curl -fsS "$HEALTH_URL" >/tmp/prokopov-alarm-health.json 2>/dev/null; then
        cat /tmp/prokopov-alarm-health.json
        echo
        echo "UPDATE SUCCESSFUL"
        echo "$OLD_REV -> $NEW_REV"
        exit 0
    fi
    sleep 5
done

echo "ERROR: Health check failed. Rolling back to $OLD_REV"

git reset --hard "$OLD_REV"
sudo docker compose up -d --build

echo "Rollback completed."
exit 1
