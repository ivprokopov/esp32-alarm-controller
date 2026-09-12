from pathlib import Path

path = Path('/app/app/main.py')
if not path.exists():
    raise SystemExit(f'Missing backend file: {path}')

text = path.read_text(encoding='utf-8')

old = '''    try:\n        return await controller_post("/command", {"command": cmd, "actor": u["username"]})\n    except Exception as exc:\n        raise HTTPException(503, f"Controller unavailable: {exc}")\n'''

new = '''    try:\n        result = await controller_post("/command", {"command": cmd, "actor": u["username"]})\n        # Refresh the controller cache immediately after a successful command.\n        # The stable v1.3 frontend is left untouched: its existing WebSocket and\n        # loadStatus() paths receive this fresh state without any JS monkey-patching.\n        try:\n            runtime["controller"] = await controller_get("/status", timeout=0.8)\n            runtime["controller_error"] = None\n            runtime["last_poll"] = now()\n            await broadcast({"type":"status", "data": runtime["controller"], "error": None})\n        except Exception:\n            # The command has already succeeded. Normal background polling remains\n            # the fallback if the immediate status read is transiently unavailable.\n            pass\n        return result\n    except Exception as exc:\n        raise HTTPException(503, f"Controller unavailable: {exc}")\n'''

if old in text:
    text = text.replace(old, new, 1)
elif 'stable v1.3 frontend is left untouched' not in text:
    raise SystemExit('Could not locate alarm command block for safe fast-feedback patch')

path.write_text(text, encoding='utf-8')

check = path.read_text(encoding='utf-8')
required = [
    'stable v1.3 frontend is left untouched',
    'runtime["controller"] = await controller_get("/status", timeout=0.8)',
    'await broadcast({"type":"status"',
]
missing = [item for item in required if item not in check]
if missing:
    raise SystemExit(f'Fast-feedback v1.3.1 verification failed: {missing}')

print('PROKOPOV safe fast feedback v1.3.1 applied successfully')
