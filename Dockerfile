FROM python:3.12-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app ./app
COPY deploy/patch_v1.0.2.b64.* /tmp/server_patch_parts/
RUN cat /tmp/server_patch_parts/patch_v1.0.2.b64.* | base64 -d > /tmp/server_patch.tar.gz \
    && tar -tzf /tmp/server_patch.tar.gz >/dev/null \
    && tar -xzf /tmp/server_patch.tar.gz -C /app \
    && rm -rf /tmp/server_patch_parts /tmp/server_patch.tar.gz /app/PATCH_NOTES_BG.txt
RUN grep -q 'ui_v1_1.css' /app/static/index.html \
    || sed -i 's#</head>#<link rel="stylesheet" href="/static/ui_v1_1.css?v=1.1.0"></head>#' /app/static/index.html
RUN grep -q 'ui_v1_1.js' /app/static/index.html \
    || sed -i 's#</body>#<script src="/static/ui_v1_1.js?v=1.1.0"></script></body>#' /app/static/index.html
RUN mkdir -p /data
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--proxy-headers"]
