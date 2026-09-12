from pathlib import Path

root = Path('/app/app/static')
index = root / 'index.html'
js_path = root / 'ui_v1_4_1.js'

for p in (index, js_path):
    if not p.exists():
        raise SystemExit(f'UI v1.4.1 injection missing required file: {p}')

html = index.read_text(encoding='utf-8')
js = js_path.read_text(encoding='utf-8')

script_open = '<script id="prokopov-ui-v1-4-1-script">'
start = html.find(script_open)
if start != -1:
    end = html.find('</script>', start)
    if end == -1:
        raise SystemExit('Broken previous UI v1.4.1 script block')
    html = html[:start] + html[end + len('</script>'):]

if '</body>' not in html:
    raise SystemExit('index.html does not contain expected </body> tag')

script_tag = f'\n<script id="prokopov-ui-v1-4-1-script">\n{js}\n</script>\n'
html = html.replace('</body>', script_tag + '</body>', 1)
index.write_text(html, encoding='utf-8')

check = index.read_text(encoding='utf-8')
required = [
    'prokopov-ui-v1-4-1-script',
    '__prokopovUi141Loaded',
    'refreshStatus',
    'connectRecoveryWs',
    'window.loadStatus',
]
missing = [x for x in required if x not in check]
if missing:
    raise SystemExit(f'UI v1.4.1 injection verification failed: {missing}')

print('PROKOPOV UI v1.4.1 injected successfully')
