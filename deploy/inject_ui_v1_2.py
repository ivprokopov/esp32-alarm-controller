from pathlib import Path

# Docker WORKDIR is /app and the project package is copied with:
#   COPY app ./app
# Therefore static files live under /app/app/static inside the image.
static_dir = Path('/app/app/static')
index = static_dir / 'index.html'
css_path = static_dir / 'ui_v1_2.css'
js_path = static_dir / 'ui_v1_2.js'

for required_path in (index, css_path, js_path):
    if not required_path.is_file():
        raise SystemExit(f'Missing UI build input: {required_path}')

html = index.read_text(encoding='utf-8')
css = css_path.read_text(encoding='utf-8')
js = js_path.read_text(encoding='utf-8')

# Remove any previous external UI v1.1/v1.2 references so the build is deterministic.
for marker in (
    '<link rel="stylesheet" href="/static/ui_v1_1.css?v=1.1.0">',
    '<script src="/static/ui_v1_1.js?v=1.1.0"></script>',
    '<link rel="stylesheet" href="/static/ui_v1_2.css?v=1.2.0">',
    '<script src="/static/ui_v1_2.js?v=1.2.0"></script>',
):
    html = html.replace(marker, '')

style_tag = f'\n<style id="prokopov-ui-v1-2">\n{css}\n</style>\n'
script_tag = f'\n<script id="prokopov-ui-v1-2-script">\n{js}\n</script>\n'

# If an older inline v1.2 exists, strip it first.
start = html.find('<style id="prokopov-ui-v1-2">')
if start != -1:
    end = html.find('</style>', start)
    if end != -1:
        html = html[:start] + html[end + len('</style>'):]
start = html.find('<script id="prokopov-ui-v1-2-script">')
if start != -1:
    end = html.find('</script>', start)
    if end != -1:
        html = html[:start] + html[end + len('</script>'):]

if '</head>' not in html or '</body>' not in html:
    raise SystemExit('index.html does not contain expected closing tags')

html = html.replace('</head>', style_tag + '</head>', 1)
html = html.replace('</body>', script_tag + '</body>', 1)
index.write_text(html, encoding='utf-8')

# Build-time verification: fail the image build if injection did not happen.
check = index.read_text(encoding='utf-8')
required = ['prokopov-ui-v1-2', 'uiAlertBtn', 'modeDisarm', 'prokopovPanic']
missing = [x for x in required if x not in check]
if missing:
    raise SystemExit(f'UI v1.2 injection verification failed: {missing}')
print('PROKOPOV UI v1.2 injected successfully')
