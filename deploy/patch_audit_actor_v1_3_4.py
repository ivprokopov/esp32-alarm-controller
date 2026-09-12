from pathlib import Path

path = Path('/app/app/main.py')
if not path.exists():
    raise SystemExit(f'Missing backend file: {path}')

text = path.read_text(encoding='utf-8')

old = '{"command": cmd, "actor": u["username"]}'
new = '{"command": cmd, "actor": (u["display_name"] or u["username"])}'

count = text.count(old)
if count == 1:
    text = text.replace(old, new, 1)
elif new not in text:
    raise SystemExit(f'Expected exactly one alarm command actor expression, found {count}')

path.write_text(text, encoding='utf-8')

check = path.read_text(encoding='utf-8')
if new not in check:
    raise SystemExit('Audit actor v1.3.4 verification failed')

print('PROKOPOV audit actor v1.3.4 applied successfully')
