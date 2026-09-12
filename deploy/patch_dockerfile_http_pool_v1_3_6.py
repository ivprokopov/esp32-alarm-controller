from pathlib import Path

p = Path('Dockerfile')
s = p.read_text()

marker = '''COPY deploy/patch_audit_actor_v1_3_4.py /tmp/patch_audit_actor_v1_3_4.py
RUN python3 /tmp/patch_audit_actor_v1_3_4.py && rm -f /tmp/patch_audit_actor_v1_3_4.py
'''

insert = marker + '''COPY deploy/patch_controller_http_pool_v1_3_5.py /tmp/patch_controller_http_pool_v1_3_5.py
RUN python3 /tmp/patch_controller_http_pool_v1_3_5.py && rm -f /tmp/patch_controller_http_pool_v1_3_5.py
'''

if 'patch_controller_http_pool_v1_3_5.py /tmp/patch_controller_http_pool_v1_3_5.py' in s:
    raise SystemExit('Dockerfile already applies v1.3.5 HTTP pool patch')
if marker not in s:
    raise SystemExit('Dockerfile audit patch marker not found')

s = s.replace(marker, insert, 1)
p.write_text(s)

print('PROKOPOV alarm-server v1.3.6 Docker image HTTP pool patch applied successfully')
print('Modified:')
print(' -', p)
