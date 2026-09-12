from pathlib import Path

controller = Path('app/controller.py')
main = Path('app/main.py')

s = controller.read_text()

old = '''def headers():
    return {"X-Alarm-Key": CONTROLLER_KEY, "Content-Type": "application/json"}


async def controller_get(path: str, params=None, timeout=3.0):
    async with httpx.AsyncClient(timeout=timeout) as client:
        r = await client.get(CONTROLLER_URL + path, headers=headers(), params=params)
        r.raise_for_status()
        return r.json()


async def controller_post(path: str, payload=None, timeout=4.0):
    async with httpx.AsyncClient(timeout=timeout) as client:
        r = await client.post(CONTROLLER_URL + path, headers=headers(), json=payload or {})
        if r.is_error:
            detail = r.text.strip()[:500]
            raise RuntimeError(f"controller {path} -> HTTP {r.status_code}: {detail}")
        return r.json()


async def controller_put(path: str, payload=None, timeout=5.0):
    async with httpx.AsyncClient(timeout=timeout) as client:
        r = await client.put(CONTROLLER_URL + path, headers=headers(), json=payload or {})
        if r.is_error:
            detail = r.text.strip()[:500]
            raise RuntimeError(f"controller {path} -> HTTP {r.status_code}: {detail}")
        return r.json()
'''

new = '''def headers():
    return {"X-Alarm-Key": CONTROLLER_KEY, "Content-Type": "application/json"}


_controller_client = None


def _get_controller_client():
    global _controller_client
    if _controller_client is None:
        _controller_client = httpx.AsyncClient(
            timeout=httpx.Timeout(5.0),
            limits=httpx.Limits(
                max_connections=4,
                max_keepalive_connections=2,
                keepalive_expiry=30.0,
            ),
            headers=headers(),
        )
    return _controller_client


async def close_controller_client():
    global _controller_client
    if _controller_client is not None:
        await _controller_client.aclose()
        _controller_client = None


async def controller_get(path: str, params=None, timeout=3.0):
    client = _get_controller_client()
    r = await client.get(CONTROLLER_URL + path, params=params, timeout=timeout)
    r.raise_for_status()
    return r.json()


async def controller_post(path: str, payload=None, timeout=4.0):
    client = _get_controller_client()
    r = await client.post(CONTROLLER_URL + path, json=payload or {}, timeout=timeout)
    if r.is_error:
        detail = r.text.strip()[:500]
        raise RuntimeError(f"controller {path} -> HTTP {r.status_code}: {detail}")
    return r.json()


async def controller_put(path: str, payload=None, timeout=5.0):
    client = _get_controller_client()
    r = await client.put(CONTROLLER_URL + path, json=payload or {}, timeout=timeout)
    if r.is_error:
        detail = r.text.strip()[:500]
        raise RuntimeError(f"controller {path} -> HTTP {r.status_code}: {detail}")
    return r.json()
'''

if old not in s:
    raise SystemExit('controller HTTP client marker not found')
s = s.replace(old, new, 1)
controller.write_text(s)

m = main.read_text()
old_import = 'from .controller import controller_get, controller_post, sync_config, sync_cards, ingest_events\n'
new_import = 'from .controller import controller_get, controller_post, sync_config, sync_cards, ingest_events, close_controller_client\n'
if old_import not in m:
    raise SystemExit('main controller import marker not found')
m = m.replace(old_import, new_import, 1)

old_startup = '''@app.on_event("startup")
async def startup():
    init_db()
    asyncio.create_task(background_loop())
'''
new_startup = '''@app.on_event("startup")
async def startup():
    init_db()
    asyncio.create_task(background_loop())


@app.on_event("shutdown")
async def shutdown():
    await close_controller_client()
'''
if old_startup not in m:
    raise SystemExit('startup marker not found')
m = m.replace(old_startup, new_startup, 1)
main.write_text(m)

print('PROKOPOV alarm-server v1.3.5 persistent controller HTTP pool patch applied successfully')
print('Modified:')
print(' -', controller)
print(' -', main)
