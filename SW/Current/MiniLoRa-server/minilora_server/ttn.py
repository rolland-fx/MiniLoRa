"""TTN integration: webhook decoding + storage-API polling.

Both paths feed the same decoders; the poller is the same storage endpoint
the Android app uses (GET .../packages/storage/uplink_message), so TTN
credentials work identically in both.
"""

import asyncio
import base64
import json
import logging
import time
from typing import Optional

import httpx

from .decode import decode_diagnostics, decode_history, decode_telemetry

log = logging.getLogger("minilora.ttn")


def frm_to_bytes(frm: str) -> bytes:
    """TTN frm_payload is base64; some setups paste hex — accept both."""
    try:
        return base64.b64decode(frm, validate=True)
    except Exception:
        clean = frm.replace(" ", "").replace("\n", "")
        if len(clean) % 2 == 0:
            try:
                return bytes.fromhex(clean)
            except ValueError:
                pass
    raise ValueError(f"cannot decode frm_payload: {frm[:40]}…")


def received_at_to_epoch(iso: str) -> Optional[int]:
    """TTN received_at (ISO 8601, may carry >3 fractional digits) -> epoch s.
    Mirrors the Android app's parsing (TTNClient.extractReceivedAt)."""
    if not iso:
        return None
    import re as _re
    from datetime import datetime, timezone
    cleaned = _re.sub(r"\.(\d{3})\d+Z", r".\1Z", iso.strip())
    for fmt in ("%Y-%m-%dT%H:%M:%S.%fZ", "%Y-%m-%dT%H:%M:%SZ"):
        try:
            return int(datetime.strptime(
                cleaned, fmt).replace(tzinfo=timezone.utc).timestamp())
        except ValueError:
            continue
    return None


def parse_uplink_event(event: dict) -> list:
    """Extract (device_id, f_port, bytes, received_at) from a TTN event.

    Handles the webhook shape ({"end_device_ids": ..., "uplink_message": ...})
    and the storage-API NDJSON shape ({"result": {...}}).
    """
    out = []
    candidates = [event]
    if "result" in event and isinstance(event["result"], dict):
        candidates = [event["result"]]
    for ev in candidates:
        up = ev.get("uplink_message") or ev
        dev_ids = ev.get("end_device_ids") or {}
        device_id = dev_ids.get("device_id") or ev.get("device_id")
        frm = up.get("frm_payload")
        if not device_id or not frm:
            continue
        out.append({
            "device_id": device_id,
            "f_port": int(up.get("f_port", 1)),
            "bytes": frm_to_bytes(frm),
            "received_at": up.get("received_at"),
            # LoRaWAN frame counter — the delivery-ratio ground truth
            # (frames lost = counter gaps). Absent on WiFi pushes.
            "f_cnt": up.get("f_cnt"),
        })
    return out


def decode_frame(f_port: int, data: bytes) -> tuple:
    """Route a frame by fPort.

    Returns (kind, decoded) with kind in
    'telemetry' | 'diagnostics' | 'history'.
    """
    if f_port == 2:
        return "diagnostics", decode_diagnostics(data)
    if f_port == 4:
        return "history", decode_history(data)
    # fPort 1 is telemetry; unknown fPorts are treated as telemetry and will
    # raise if they are not 28 bytes.
    return "telemetry", decode_telemetry(data)


class TTNPoller:
    """Background poller for the TTN storage API."""

    def __init__(self, region: str, app_id: str, api_key: str,
                 device_ids: list, interval_s: int, limit: int,
                 window_h: int, on_frame):
        self.url = (f"https://{region}.cloud.thethings.network/api/v3/as/"
                    f"applications/{app_id}/devices/{{dev}}/packages/storage/"
                    f"uplink_message?limit={limit}&last={window_h}h"
                    f"&order=-received_at")
        self.headers = {"Authorization": f"Bearer {api_key}"}
        self.device_ids = device_ids
        self.interval_s = interval_s
        self.on_frame = on_frame  # callback(device_id, f_port, bytes, ts, f_cnt) -> new rows stored
        self._client = httpx.AsyncClient(timeout=15.0)

    async def poll_once(self) -> int:
        """Poll all devices; returns the number of NEW frames stored."""
        n = 0
        for dev in self.device_ids:
            try:
                resp = await self._client.get(self.url.format(dev=dev),
                                              headers=self.headers)
                if resp.status_code != 200:
                    log.warning("TTN poll %s -> HTTP %d: %s", dev,
                                resp.status_code, resp.text[:200])
                    continue
                for line in resp.text.splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        event = json.loads(line)
                    except ValueError:
                        continue
                    for up in parse_uplink_event(event):
                        if up["device_id"] != dev:
                            continue
                        try:
                            frame_ts = received_at_to_epoch(
                                up.get("received_at"))
                            # on_frame returns the number of NEW rows stored
                            # (dup frames dedup to 0) — fetch-now reports it.
                            n += self.on_frame(up["device_id"], up["f_port"],
                                               up["bytes"], frame_ts,
                                               f_cnt=up.get("f_cnt")) or 0
                        except (ValueError, KeyError) as e:
                            log.debug("skipping frame from %s: %s",
                                      up["device_id"], e)
            except httpx.HTTPError as e:
                log.warning("TTN poll %s failed: %s", dev, e)
        return n

    async def run_forever(self):
        log.info("TTN poller started (%s, %d device(s), every %ds)",
                 self.url.split('/api')[0], len(self.device_ids),
                 self.interval_s)
        while True:
            t0 = time.monotonic()
            try:
                await self.poll_once()
            except Exception:  # never kill the task
                log.exception("poller iteration failed")
            elapsed = time.monotonic() - t0
            await asyncio.sleep(max(1.0, self.interval_s - elapsed))
