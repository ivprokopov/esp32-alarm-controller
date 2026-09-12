from pathlib import Path

root = Path('/app/app/static')
index = root / 'index.html'
css = (root / 'ui_v1_3_2.css').read_text(encoding='utf-8')
js = (root / 'ui_v1_3_2.js').read_text(encoding='utf-8')
html = index.read_text(encoding='utf-8')

if '</head>' not in html or '</body>' not in html:
    raise SystemExit('UI v1.3.2: invalid index.html')

html = html.replace('</head>', '\n<style id="prokopov-ui-v1-3-2-style">\n' + css + '\n</style>\n</head>', 1)
html = html.replace('</body>', '\n<script id="prokopov-ui-v1-3-2-script">\n' + js + '\n</script>\n</body>', 1)
index.write_text(html, encoding='utf-8')

check = index.read_text(encoding='utf-8')
for marker in ('prokopov-ui-v1-3-2-style', 'prokopov-ui-v1-3-2-script', '__prokopovUi132Loaded'):
    if marker not in check:
        raise SystemExit('UI v1.3.2 injection verification failed: ' + marker)

print('PROKOPOV UI v1.3.2 injected successfully')
