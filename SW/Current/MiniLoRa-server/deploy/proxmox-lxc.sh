#!/usr/bin/env bash
# MiniLoRa-server — Proxmox VE LXC installer
#
# ONE COMMAND (from anywhere — downloads this script and the code from
# GitHub; run as root on the Proxmox host):
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/rolland-fx/MiniLoRa/main/SW/Current/MiniLoRa-server/deploy/proxmox-lxc.sh)"
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/rolland-fx/MiniLoRa/main/SW/Current/MiniLoRa-server/deploy/proxmox-lxc.sh)" \
#     --id 120 --ip 192.168.1.50/24 --gateway 192.168.1.1
#   bash -c "$(curl -fsSL ...)" -- --rootfs-storage local-lvm   # if the
#     auto-detected rootfs storage is wrong on your box
#
# From a local clone (uses the checkout next to this script — no download):
#   bash deploy/proxmox-lxc.sh
#
# Update an existing container (code re-pulled from the same source):
#   bash -c "$(curl -fsSL ...)" -- --update 120
#   bash deploy/proxmox-lxc.sh --update 120
# Or, from the CONTAINER's own shell (installed with every new setup):
#   update          # re-pull code + refresh venv + restart, keep config/data
#
# Interactive on a terminal: an installer wizard asks default-vs-custom
# (CT ID, resources, storages, network) before creating. Scripted runs
# (any option, --update, cron, no TTY) skip it; --no-wizard forces the
# silent defaults.
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
STORAGE=local         # where the Debian template is downloaded (vztmpl)
ROOTFS=""             # rootfs storage: defaults to auto-detect (rootdir-
                      # capable, prefers local-lvm); --storage forces both,
                      # --rootfs-storage forces just the rootfs
CTID=""
CTIP=""
CTGW=""
REPO=""
BRANCH=""
UPDATE_CT=""
SRC=""            # defaults to the directory this script lives in
PASS=""
NO_WIZARD=0
EXPLICIT=0        # any explicit config option = scripted install

msg()  { printf '\033[1;34m[MiniLoRa]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --id)       CTID="$2"; EXPLICIT=1; shift 2 ;;
    --ip)       CTIP="$2"; EXPLICIT=1; shift 2 ;;
    --gateway)  CTGW="$2"; EXPLICIT=1; shift 2 ;;
    --bridge)   BRIDGE="$2"; EXPLICIT=1; shift 2 ;;
    --storage)  STORAGE="$2"; ROOTFS="$2"; EXPLICIT=1; shift 2 ;;
    --rootfs-storage) ROOTFS="$2"; EXPLICIT=1; shift 2 ;;
    --password) PASS="$2"; EXPLICIT=1; shift 2 ;;
    --repo)     REPO="$2"; EXPLICIT=1; shift 2 ;;
    --branch)   BRANCH="$2"; EXPLICIT=1; shift 2 ;;
    --source)   SRC="$2"; EXPLICIT=1; shift 2 ;;
    --no-wizard) NO_WIZARD=1; shift ;;
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

# ---------- interactive wizard (community-scripts style) ----------
# Enter = default settings; "No" walks through every parameter with the
# defaults prefilled. Skipped whenever the install is scripted — any
# explicit option, --no-wizard, --update, or a terminal-less run
# (cron/CI) — so the documented one-liner keeps working everywhere.
have_interactive_tty() {
  command -v whiptail >/dev/null 2>&1 || return 1
  (exec 3<>/dev/tty) 2>/dev/null || return 1
  return 0
}

_wi() { whiptail --title "MiniLoRa-server" "$@" 3>&1 1>&2 2>&3; }

_ask_text() { # prompt default -> stdout (empty answer keeps the default)
  local v
  v="$(_wi --inputbox "$1" 10 62 "$2")" || die "aborted at: $1"
  echo "${v:-$2}"
}

_ask_menu() { # prompt default-tag tag desc [tag desc ...] -> tag
  local prompt="$1" def="$2"; shift 2
  local args=() v
  while [[ $# -ge 2 ]]; do args+=("$1" "$2"); shift 2; done
  v="$(_wi --default-item "$def" --menu "$prompt" \
        $(( ${#args[@]} / 2 + 7 )) 62 $(( ${#args[@]} / 2 )) \
        "${args[@]}")" || die "aborted at: $prompt"
  echo "$v"
}

run_wizard() {
  local nextid netmode rname rtype rdef="${ROOTFS:-}"
  local -a rargs=()
  nextid="$(pvesh get /cluster/nextid 2>/dev/null || echo 120)"

  _wi --yesno "This will create an UNPRIVILEGED Debian 12 LXC running\nMiniLoRa-server (LAN tracker backend).\n\nDefaults: ${MEM_MB} MB RAM, ${CORES} core(s), ${DISK_GB} GB disk,\n${BRIDGE} + DHCP, web UI on :${CT_PORT}.\n\nContinue?" 13 66 \
    || die "aborted"

  if _wi --yesno "Use the default settings?\n\n(No = configure ID, resources, storage and network one by one)" 11 66; then
    return   # keep every default — CTID/password/rootfs auto-resolve below
  fi

  CTID="$(_ask_text "Container ID (next free: $nextid)" "${CTID:-$nextid}")"
  [[ "$CTID" =~ ^[0-9]+$ ]] || die "container ID must be numeric (got '$CTID')"
  CT_HOSTNAME="$(_ask_text "Hostname" "$CT_HOSTNAME")"
  PASS="$(_ask_text "Root password (empty = random, printed at the end)" "")"

  MEM_MB="$(_ask_text "Memory (MB)" "$MEM_MB")"
  CORES="$(_ask_text "CPU cores" "$CORES")"
  DISK_GB="$(_ask_text "Disk (GB)" "$DISK_GB")"

  STORAGE="$(_ask_text "Template storage (vztmpl)" "$STORAGE")"
  # default: the same preference order as the create section (local-lvm
  # first), so Enter on the menu lands on the storage pct create needs
  rdef="$(pvesm status -content rootdir 2>/dev/null | awk 'NR>1 && /active/ {print $1}' | grep -m1 '^local-lvm$' || true)"
  [[ -n "$rdef" ]] || rdef="$(pvesm status -content rootdir 2>/dev/null | awk 'NR>1 && /active/ {print $1; exit}')"
  while read -r rname rtype; do
    [[ -n "$rname" ]] || continue
    rargs+=("$rname" "$rtype storage")
  done < <(pvesm status -content rootdir 2>/dev/null | awk 'NR>1 && /active/ {print $1, $2}')
  if [[ ${#rargs[@]} -ge 2 ]]; then
    ROOTFS="$(_ask_menu "Rootfs storage (container directories)" \
                 "${rdef:-local}" "${rargs[@]}")"
  fi

  BRIDGE="$(_ask_text "Network bridge" "$BRIDGE")"
  netmode="$(_ask_menu "Network on ${BRIDGE}" "dhcp" \
               "dhcp" "DHCP (recommended)" "static" "Static IPv4 + gateway")"
  if [[ "$netmode" == "static" ]]; then
    while :; do
      CTIP="$(_ask_text "Static IPv4 (CIDR, e.g. 192.168.1.50/24)" "$CTIP")"
      [[ -n "$CTIP" ]] && break
    done
    CTGW="$(_ask_text "Gateway" "$CTGW")"
  fi

  local netdesc="DHCP"
  [[ -n "$CTIP" ]] && netdesc="$CTIP (gw ${CTGW:-none})"
  _wi --yesno "Create CT $CTID '$CT_HOSTNAME'?\n\nRAM ${MEM_MB} MB · cores ${CORES} · disk ${DISK_GB} GB on ${ROOTFS:-auto-detect}\n${BRIDGE}: $netdesc · template storage: ${STORAGE}\n\nProceed?" 14 66 \
    || die "aborted"
}

if [[ -z "$UPDATE_CT" && "$NO_WIZARD" -eq 0 && "$EXPLICIT" -eq 0 ]] \
   && have_interactive_tty; then
  run_wizard
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

  # The rootfs needs a storage with 'rootdir' content — 'local' usually
  # only holds ISOs/templates ("storage 'local' does not support container
  # directories"). Auto-detect: prefer local-lvm, else the first active
  # rootdir-capable storage; --rootfs-storage overrides, --storage pins both.
  if [[ -z "$ROOTFS" ]]; then
    msg "detecting a rootfs-capable storage"
    ROOTFS="$(pvesm status -content rootdir 2>/dev/null | awk 'NR>1 && /active/{print $1}' | grep -m1 '^local-lvm$' || true)"
    [[ -n "$ROOTFS" ]] || ROOTFS="$(pvesm status -content rootdir 2>/dev/null | awk 'NR>1 && /active/{print $1}' | head -1)"
    if [[ -z "$ROOTFS" ]]; then
      ROOTFS="$STORAGE"
      msg "no rootdir storage detected — falling back to '$STORAGE'; if pct create fails, pass --rootfs-storage <name>"
    fi
  fi
  ok "rootfs storage: $ROOTFS"

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
    --rootfs "${ROOTFS}:${DISK_GB}" --net0 "$NET" \
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

# DNS fallback: some unprivileged-LXC setups end up with a broken/empty
# resolv.conf (networkd fails, ifupdown writes the lease but not the
# nameservers) — apt dies instantly on it. If nothing resolves, use a
# public resolver for the install.
if ! getent hosts deb.debian.org >/dev/null 2>&1; then
  echo "nameserver 1.1.1.1" > /etc/resolv.conf
fi

apt-get update -qq
apt-get install -y -qq python3-venv python3-pip git curl ca-certificates tzdata whiptail >/dev/null

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

# The in-container `update` command (community-scripts convention): type
# `update` in the container shell to re-pull the code, refresh the venv
# and restart — config.toml and data/ are never touched. It re-runs THIS
# script (idempotent), then waits for /health on the configured port.
cat > /usr/local/bin/update <<'UPDATE'
#!/usr/bin/env bash
# MiniLoRa-server update: pull the latest code + restart, keeping
# config.toml and data/. Installed by the LXC setup; the heavy lifting
# is the idempotent /opt/minilora-inner.sh. Interactive menu when a
# terminal is present (community-scripts style); silent otherwise so
# cron/scripts never hang on a dialog.
set -euo pipefail
APP_DIR=/opt/minilora-server
LOG=/tmp/minilora-update.log
if [ -f /opt/minilora-deploy.env ]; then . /opt/minilora-deploy.env; fi

have_interactive_tty() {
  command -v whiptail >/dev/null 2>&1 || return 1
  (exec 3<>/dev/tty) 2>/dev/null || return 1
  return 0
}

run_update() {   # $1 = quiet | verbose
  echo "[MiniLoRa] updating (source: ${REPO_URL:-local tarball})…"
  if [ -z "${REPO_URL:-}" ]; then
    echo "[MiniLoRa] note: installed from a local tarball — this refreshes"
    echo "[MiniLoRa] deps and restarts, but NEW CODE needs a host-side:"
    echo "[MiniLoRa]   bash proxmox-lxc.sh --update <ctid>"
  fi
  if [ "$1" = quiet ]; then
    if ! bash /opt/minilora-inner.sh >"$LOG" 2>&1; then
      echo "[MiniLoRa] update FAILED — last 25 log lines ($LOG):" >&2
      tail -25 "$LOG" >&2
      exit 1
    fi
  else
    bash /opt/minilora-inner.sh
  fi
}

wait_healthy() {
  local port i
  port=$(python3 - <<PY
import tomllib
try:
    print(tomllib.load(open("$APP_DIR/config.toml", "rb"))["server"].get("port", 8080))
except Exception:
    print(8080)
PY
)
  for i in $(seq 1 15); do
    if curl -sf "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      echo "[MiniLoRa] update done — healthy on :$port"
      return 0
    fi
    sleep 1
  done
  echo "[MiniLoRa] WARNING: not healthy after 15 s — recent logs:" >&2
  journalctl -u minilora-server -n 20 --no-pager 2>/dev/null || true
  return 1
}

MODE=quiet
if have_interactive_tty; then
  CHOICE=$(whiptail --title "MiniLoRa-server update" --menu \
    "Re-pull the code, refresh the venv and restart the service.\nconfig.toml and data/ are kept. Choose:" \
    12 62 3 \
    "1" "YES — quiet output (recommended)" \
    "2" "YES — verbose output" \
    "3" "NO — cancel update" \
    --nocancel --default-item "1" 3>&1 1>&2 2>&3) \
    || { echo "[MiniLoRa] update cancelled"; exit 0; }
  case "$CHOICE" in
    2) MODE=verbose ;;
    3) echo "[MiniLoRa] update cancelled"; exit 0 ;;
  esac
fi
run_update "$MODE"
wait_healthy
UPDATE
chmod +x /usr/local/bin/update
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
ok "        update:   container shell: just type 'update'  ·  host: bash $0 --update $CTID"
ok "        backups:  PVE vzdump + cron http://$IP:$CT_PORT/api/export/db"
