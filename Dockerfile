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
COPY deploy/patch_fast_feedback_v1_3_1.py /tmp/patch_fast_feedback_v1_3_1.py
RUN python3 /tmp/patch_fast_feedback_v1_3_1.py && rm -f /tmp/patch_fast_feedback_v1_3_1.py
COPY deploy/patch_controller_poll_grace_v1_3_3.py /tmp/patch_controller_poll_grace_v1_3_3.py
RUN python3 /tmp/patch_controller_poll_grace_v1_3_3.py && rm -f /tmp/patch_controller_poll_grace_v1_3_3.py
COPY deploy/inject_ui_v1_2.py /tmp/inject_ui_v1_2.py
RUN python3 /tmp/inject_ui_v1_2.py && rm -f /tmp/inject_ui_v1_2.py
COPY deploy/inject_ui_v1_3.py /tmp/inject_ui_v1_3.py
RUN python3 /tmp/inject_ui_v1_3.py && rm -f /tmp/inject_ui_v1_3.py
COPY deploy/inject_ui_v1_3_1.py /tmp/inject_ui_v1_3_1.py
RUN python3 /tmp/inject_ui_v1_3_1.py && rm -f /tmp/inject_ui_v1_3_1.py
COPY deploy/inject_ui_v1_3_2.py /tmp/inject_ui_v1_3_2.py
RUN python3 /tmp/inject_ui_v1_3_2.py && rm -f /tmp/inject_ui_v1_3_2.py
RUN mkdir -p /data
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--proxy-headers"]
