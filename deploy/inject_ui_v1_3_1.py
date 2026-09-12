from pathlib import Path

root = Path('/app/app/static')
index = root / 'index.html'
css_path = root / 'ui_v1_3_1.css'

for p in (index, css_path):
    if not p.exists():
        raise SystemExit(f'UI v1.3.1 injection missing required file: {p}')

html = index.read_text(encoding='utf-8')
css = css_path.read_text(encoding='utf-8')

style_open = '<style id="prokopov-ui-v1-3-1">'
start = html.find(style_open)
if start != -1:
    end = html.find('</style>', start)
    if end == -1:
        raise SystemExit('Broken previous UI v1.3.1 style block')
    html = html[:start] + html[end + len('</style>'):]

if '</head>' not in html:
    raise SystemExit('index.html does not contain expected </head> tag')

style_tag = f'\n<style id="prokopov-ui-v1-3-1">\n{css}\n</style>\n'
html = html.replace('</head>', style_tag + '</head>', 1)
index.write_text(html, encoding='utf-8')

check = index.read_text(encoding='utf-8')
required = [
    'prokopov-ui-v1-3-1',
    'LOCKED = blue',
    'UNLOCKED = green',
]
missing = [item for item in required if item not in check]
if missing:
    raise SystemExit(f'UI v1.3.1 verification failed: {missing}')

print('PROKOPOV UI v1.3.1 door colours injected successfully')
