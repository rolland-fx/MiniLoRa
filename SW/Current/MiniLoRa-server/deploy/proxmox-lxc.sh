#!/usr/bin/env bash
# MiniLoRa-server — Proxmox VE LXC installer
#
# ONE COMMAND (from anywhere — downloads this script and the code from
# GitHub; run as root on the Proxmox host):
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/rolland-fx/MiniLoRa/main/SW/Current/MiniLoRa-server/deploy/proxmox-lxc.sh)"
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/rolland-fx/MiniLoRa/main/SW/Current/MiniLoRa-server/deploy/proxmox-lxc.sh)" \
#     --id 120 --ip 192.168.1.50/24 --gateway 192.168.1.1
#
# From a local clone (uses the checkout next to this script — no download):
#   bash deploy/proxmox-lxc.sh
#
# Update an existing container (code re-pulled from the same source):
#   bash -c "$(curl -fsSL ...)" -- --update 120
#   bash deploy/proxmox-lxc.sh --update 120
#
# What it builds: an unprivileged Debian 12 LXC running the server natively
# (python venv + systemd, NO docker), data in /opt/minilora-server/data,
# web UI on http://<ct-ip>:8080 — with the bounded-log and 100 MB DB
# retention already configured.
set -euo pipefail
APP_DIR=/opt/minilora-server
CT_HOSTNAME=minilora-server
CT_PORT=8080
MEM_MB=512
CORES=1
DISK_GB=4
BRIDGE=vmbr0
STORAGE=local
CTID=""
CTIP=""
CTGW=""
REPO=""
BRANCH=""
UPDATE_CT=""
SRC=""            # defaults to the directory this script lives in
PASS=""

msg()  { printf '\033[1;34m[MiniLoRa]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --id)       CTID="$2"; shift 2 ;;
    --ip)       CTIP="$2"; shift 2 ;;
    --gateway)  CTGW="$2"; shift 2 ;;
    --bridge)   BRIDGE="$2"; shift 2 ;;
    --storage)  STORAGE="$2"; shift 2 ;;
    --password) PASS="$2"; shift 2 ;;
    --repo)     REPO="$2"; shift 2 ;;
    --branch)   BRANCH="$2"; shift 2 ;;
    --source)   SRC="$2"; shift 2 ;;
    --update)   UPDATE_CT="$2"; shift 2 ;;
    *)          die "unknown option $1 (see header)" ;;
  esac
done

[[ $EUID -eq 0 ]] || die "run as root on the Proxmox host (pct/pvesh need it)"

# Source mode: executed from a file on disk → use the checkout next to it
# (no download); piped from curl (`bash -c "$(curl ...)"`) or stdin →
# default to the GitHub repo unless --repo says otherwise.
if [[ -f "${BASH_SOURCE:-}" ]]; then
  [[ -n "$SRC" ]] || SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
elif [[ -z "$REPO" ]]; then
  REPO="https://github.com/rolland-fx/MiniLoRa"
  [[ -n "$BRANCH" ]] || BRANCH="main"
fi

# ---------- target container: create (fresh) or reuse (update) ----------
if [[ -n "$UPDATE_CT" ]]; then
  CTID="$UPDATE_CT"
  pct exec "$CTID" -- true 2>/dev/null || die "CT $CTID not found or not running"
  msg "updating existing CT $CTID"
else
  msg "checking the debian-12 LXC template"
  if ! pveam list "$STORAGE" 2>/dev/null | grep -q debian-12; then
    pveam update >/dev/null
    TMPL="$(pveam available --section system 2>/dev/null | awk '/debian-12-standard/{print $2; exit}')"
    [[ -n "$TMPL" ]] || die "no debian-12-standard template found (pveam available)"
    pveam download "$STORAGE" "$TMPL" >/dev/null
  fi
  TEMPLATE="$(pveam list "$STORAGE" | awk '/debian-12-standard/{print $1; exit}')"
  ok "template: $TEMPLATE"

  [[ -n "$CTID" ]] || CTID="$(pvesh get /cluster/nextid)"
  [[ -n "$PASS"  ]] || PASS="$(head -c 12 /dev/urandom | base64 | tr -d '=+/')"

  NET="name=eth0,bridge=${BRIDGE},ip=dhcp"
  if [[ -n "$CTIP" ]]; then
    NET="name=eth0,bridge=${BRIDGE},ip=${CTIP}"
    [[ -n "$CTGW" ]] && NET+=",gw=${CTGW}"
  fi

  msg "creating CT $CTID (${CT_HOSTNAME}, ${MEM_MB} MB, ${DISK_GB} GB, unprivileged)"
  pct create "$CTID" "$TEMPLATE" \
    --hostname "$CT_HOSTNAME" --memory "$MEM_MB" --cores "$CORES" \
    --rootfs "${STORAGE}:${DISK_GB}" --net0 "$NET" \
    --unprivileged 1 --onboot 1 --start 1 --password "$PASS" \
    --description "MiniLoRa-server (LAN tracker backend)" >/dev/null
  ok "CT $CTID created and started"

  for i in $(seq 1 30); do
    pct exec "$CTID" -- true >/dev/null 2>&1 && break
    sleep 1
  done
  pct exec "$CTID" -- bash -c 'ip -4 addr show eth0 | grep -q inet' \
    || die "container network did not come up"
fi

# ---------- the in-container setup script (install AND update) ----------
# Source of truth for the code: /tmp/minilora-src.tar.gz if present, else
# the git repo baked into /opt/minilora-deploy.env by the --repo/--branch
# options. config.toml is written ONCE and survives every update.
write_deploy_env() {
  cat > "$1" <<ENV
REPO_URL='${REPO//\'/}'
BRANCH='${BRANCH//\'/}'
ENV
}

write_inner_setup() {
  cat > "$1" <<'INNER'
#!/usr/bin/env bash
# MiniLoRa-server in-container setup — idempotent (install AND update).
set -euo pipefail
APP_DIR=/opt/minilora-server
export DEBIAN_FRONTEND=noninteractive

[ -f /opt/minilora-deploy.env ] && . /opt/minilora-deploy.env

apt-get update -qq
apt-get install -y -qq python3-venv python3-pip git curl ca-certificates tzdata >/dev/null

mkdir -p "$APP_DIR"
if [ -f /tmp/minilora-src.tar.gz ]; then
  tar -xzf /tmp/minilora-src.tar.gz -C "$APP_DIR"
  rm -f /tmp/minilora-src.tar.gz
elif [ -n "${REPO_URL:-}" ]; then
  git clone --depth 1 ${BRANCH:+-b "$BRANCH"} "$REPO_URL" /tmp/src-dl
  # The published repo nests the server under SW/Current/MiniLoRa-server;
  # a repo that IS the server (no nesting) also works.
  SRC_ROOT=/tmp/src-dl/SW/Current/MiniLoRa-server
  [ -d "$SRC_ROOT" ] || SRC_ROOT=/tmp/src-dl
  rm -rf "$APP_DIR/minilora_server" "$APP_DIR/templates" "$APP_DIR/static" \
         "$APP_DIR/requirements.txt" "$APP_DIR/deploy"
  cp -a "$SRC_ROOT/." "$APP_DIR/"
  rm -rf /tmp/src-dl
elif [ -f "$APP_DIR/minilora_server/main.py" ]; then
  echo "source already staged — reusing $APP_DIR"
else
  echo "no source: no tarball and no REPO_URL" >&2; exit 1
fi

cd "$APP_DIR"
[ -d .venv ] || python3 -m venv .venv
.venv/bin/pip install -q -r requirements.txt

mkdir -p "$APP_DIR/data"
if [ ! -f config.toml ]; then
cat > config.toml <<CFG
[server]
host = "0.0.0.0"
port = 8080
db_path = "$APP_DIR/data/minilora.sqlite3"
log_file = "$APP_DIR/data/minilora.log"
log_max_mb = 5
log_backups = 3

[retention]
enabled = true
readings_per_device = 200000
diagnostics_per_device = 50000
db_max_mb = 100
check_interval_s = 3600
CFG
fi

cat > /etc/systemd/system/minilora-server.service <<UNIT
[Unit]
Description=MiniLoRa-server (LAN tracker backend)
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=$APP_DIR
ExecStart=$APP_DIR/.venv/bin/python -m minilora_server.main
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT

if [ -d /run/systemd/system ]; then
  systemctl daemon-reload
  systemctl enable minilora-server.service >/dev/null 2>&1 || true
  systemctl restart minilora-server.service
else
  echo "(systemd not running here — unit written, boot-time start skipped)"
fi
INNER
}

INNER=/tmp/minilora-inner.sh
ENVF=/tmp/minilora-deploy.env
write_inner_setup "$INNER"
write_deploy_env  "$ENVF"
pct push "$CTID" "$INNER" /opt/minilora-inner.sh
pct push "$CTID" "$ENVF" /opt/minilora-deploy.env
if [[ -z "$REPO" ]]; then
  [[ -f "$SRC/minilora_server/main.py" ]] || die "no minilora_server/main.py under $SRC (use --source)"
  rm -f /tmp/minilora-src.tar.gz
  # data/, config.toml (secrets!), .venv and local junk never travel
  tar -czf /tmp/minilora-src.tar.gz --exclude='.venv' --exclude='./data' \
      --exclude='__pycache__' --exclude='config.toml' --exclude='.pytest_cache' \
      --exclude='.git' --exclude='./tests' -C "$SRC" .
  pct push "$CTID" /tmp/minilora-src.tar.gz /tmp/minilora-src.tar.gz
  ok "source staged (tarball)"
else
  ok "source: git $REPO${BRANCH:+ (branch $BRANCH)}"
fi

msg "installing (apt + venv + pip + systemd) — a few minutes"
pct exec "$CTID" -- bash /opt/minilora-inner.sh
ok "service installed and started"

IP="$(pct exec "$CTID" -- hostname -I | awk '{print $1}')"
echo
ok "done. UI:     http://$IP:$CT_PORT"
ok "        health:   http://$IP:$CT_PORT/health  ·  retention: /api/retention"
ok "        root pw:  $PASS  (change: pct exec $CTID -- passwd root)"
ok "        config:   $APP_DIR/config.toml (edit → pct exec $CTID -- systemctl restart minilora-server)"
ok "        update:   bash $0 --update $CTID"
ok "        backups:  PVE vzdump + cron http://$IP:$CT_PORT/api/export/db"
