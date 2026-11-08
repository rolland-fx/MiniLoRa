FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY minilora_server/ ./minilora_server/
COPY templates/ ./templates/
COPY static/ ./static/
COPY config.example.toml ./

# Data (SQLite) and config are expected on /data and /config when mounted.
# The bind/port come from the environment, NOT the mounted config — the
# container always serves 0.0.0.0:8080 even if the config was written for a
# native run on :5000. The log file lands on the mounted volume too, and is
# size-capped by rotation (MINILORA_LOG_MAX_MB × backups).
ENV MINILORA_DB=/data/minilora.sqlite3 \
    MINILORA_HOST=0.0.0.0 \
    MINILORA_PORT=8080 \
    MINILORA_LOG_FILE=/data/minilora.log \
    MINILORA_LOG_MAX_MB=5 \
    MINILORA_LOG_BACKUPS=3
EXPOSE 8080

# docker ps shows healthy/unhealthy; restart: unless-stopped recovers
# crashes. Self-check on the container's own port (env-overridable).
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD python -c "import urllib.request,os; urllib.request.urlopen(\
'http://127.0.0.1:%s/health' % os.environ.get('MINILORA_PORT', '8080'), \
timeout=4)" || exit 1

CMD ["python", "-m", "minilora_server.main"]
