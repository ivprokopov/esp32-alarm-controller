from pathlib import Path

path = Path('/app/app/main.py')
if not path.exists():
    raise SystemExit(f'Missing backend file: {path}')

text = path.read_text(encoding='utf-8')
old = '''    try:\n        return await controller_post("/command", {"command": cmd, "actor": u["username"]})\n    except Exception as exc:\n        raise HTTPException(503, f"Controller unavailable: {exc}")\n'''
new = '''    try:\n        result = await controller_post("/command", {"command": cmd, "actor": u["username"]})\n        # Refresh controller state immediately after a command so the web UI does\n        # not wait for the background polling cycle. This is especially visible\n        # for door lock/unlock feedback.\n        try:\n            runtime["controller"] = await controller_get("/status", timeout=1.2)\n            runtime["controller_error"] = None\n            runtime["last_poll"] = now()\n            await broadcast({"type":"status", "data": runtime["controller"], "error": None})\n        except Exception:\n            # Command already succeeded; the normal background loop will reconcile.\n            pass\n        return result\n    except Exception as exc:\n        raise HTTPException(503, f"Controller unavailable: {exc}")\n'''

if old not in text:
    if 'Refresh controller state immediately after a command' in text:
        print('PROKOPOV realtime backend v1.4 already applied')
    else:
        raise SystemExit('Could not locate alarm command block for realtime v1.4 patch')
else:
    text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8')

check = path.read_text(encoding='utf-8')
required = ['Refresh controller state immediately after a command', 'await broadcast({"type":"status"']
missing = [x for x in required if x not in check]
if missing:
    raise SystemExit(f'Realtime backend v1.4 verification failed: {missing}')

print('PROKOPOV realtime backend v1.4 applied successfully')
