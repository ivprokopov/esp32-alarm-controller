FROM python:3.12-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app ./app
COPY deploy/PROKOPOV_ALARM_SERVER_PATCH_v1.0.2.tar.gz /tmp/server_patch.tar.gz
RUN tar -xzf /tmp/server_patch.tar.gz -C /app \
    && rm -f /tmp/server_patch.tar.gz /app/PATCH_NOTES_BG.txt
RUN mkdir -p /data
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--proxy-headers"]
