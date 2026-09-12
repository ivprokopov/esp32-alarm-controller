from pathlib import Path

path = Path('/app/app/main.py')
if not path.exists():
    raise SystemExit(f'Missing backend file: {path}')

text = path.read_text(encoding='utf-8')

success_old = '''            runtime["controller"] = await controller_get("/status", timeout=2.5)\n            runtime["controller_error"] = None\n            runtime["last_poll"] = now()\n'''
success_new = '''            runtime["controller"] = await controller_get("/status", timeout=2.5)\n            runtime["controller_error"] = None\n            runtime["last_poll"] = now()\n            runtime["controller_failures"] = 0\n'''

failure_old = '''        except Exception as exc:\n            runtime["controller_error"] = str(exc)\n            runtime["controller"] = None\n'''
failure_new = '''        except Exception as exc:\n            # A single transient /status timeout must not make the entire web UI\n            # jump to OFFLINE. Keep the last known-good controller state and only\n            # declare it offline after repeated failures and several seconds without\n            # a successful poll. The autonomous alarm core itself is unaffected.\n            runtime["controller_error"] = str(exc)\n            runtime["controller_failures"] = int(runtime.get("controller_failures", 0)) + 1\n            last_ok = runtime.get("last_poll")\n            stale_s = (now() - int(last_ok)) if last_ok else 999\n            if runtime["controller_failures"] >= 2 and stale_s >= 6:\n                runtime["controller"] = None\n'''

if success_old in text:
    text = text.replace(success_old, success_new, 1)
elif 'runtime["controller_failures"] = 0' not in text:
    raise SystemExit('Could not locate successful controller poll block')

if failure_old in text:
    text = text.replace(failure_old, failure_new, 1)
elif 'single transient /status timeout' not in text:
    raise SystemExit('Could not locate failed controller poll block')

path.write_text(text, encoding='utf-8')

check = path.read_text(encoding='utf-8')
required = [
    'runtime["controller_failures"] = 0',
    'single transient /status timeout',
    'runtime["controller_failures"] >= 2 and stale_s >= 6',
]
missing = [item for item in required if item not in check]
if missing:
    raise SystemExit(f'Controller poll grace verification failed: {missing}')

print('PROKOPOV controller poll grace v1.3.3 applied successfully')
