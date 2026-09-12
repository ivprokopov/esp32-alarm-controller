from pathlib import Path

root = Path('/app/app/static')
index = root / 'index.html'
css_path = root / 'ui_v1_4.css'
js_path = root / 'ui_v1_4.js'

for p in (index, css_path, js_path):
    if not p.exists():
        raise SystemExit(f'UI v1.4 injection missing required file: {p}')

html = index.read_text(encoding='utf-8')
css = css_path.read_text(encoding='utf-8')
js = js_path.read_text(encoding='utf-8')

style_open = '<style id="prokopov-ui-v1-4">'
script_open = '<script id="prokopov-ui-v1-4-script">'

start = html.find(style_open)
if start != -1:
    end = html.find('</style>', start)
    if end == -1:
        raise SystemExit('Broken previous UI v1.4 style block')
    html = html[:start] + html[end + len('</style>'):]

start = html.find(script_open)
if start != -1:
    end = html.find('</script>', start)
    if end == -1:
        raise SystemExit('Broken previous UI v1.4 script block')
    html = html[:start] + html[end + len('</script>'):]

if '</head>' not in html or '</body>' not in html:
    raise SystemExit('index.html does not contain expected closing tags')

style_tag = f'\n<style id="prokopov-ui-v1-4">\n{css}\n</style>\n'
script_tag = f'\n<script id="prokopov-ui-v1-4-script">\n{js}\n</script>\n'

html = html.replace('</head>', style_tag + '</head>', 1)
html = html.replace('</body>', script_tag + '</body>', 1)
index.write_text(html, encoding='utf-8')

check = index.read_text(encoding='utf-8')
required = [
    'prokopov-ui-v1-4',
    '1× SILENT · 2× SIREN',
    'door-active-unlock',
    '__prokopovUi14CmdHooked',
]
missing = [x for x in required if x not in check]
if missing:
    raise SystemExit(f'UI v1.4 injection verification failed: {missing}')

print('PROKOPOV UI v1.4 injected successfully')
