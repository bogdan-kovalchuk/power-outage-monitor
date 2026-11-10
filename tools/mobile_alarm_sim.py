#!/usr/bin/env python3
"""Bench utility for simulating Power Outage Monitor HTTP and ntfy traffic."""

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_DEVICE_NAME = "power-monitor"

ALERT_EVENTS = ("POWER_OFF", "POWER_ON", "TEST_ALARM")

NTFY_TEMPLATES = {
    "POWER_OFF": {
        "Title": "Power Outage Alert",
        "Priority": "urgent",
        "Tags": "warning,rotating_light,zap",
    },
    "POWER_ON": {
        "Title": "Power Restored",
        "Priority": "default",
        "Tags": "white_check_mark,electric_plug",
    },
    "TEST_ALARM": {
        "Title": "Power Monitor Test",
        "Priority": "default",
        "Tags": "bell,test_tube",
    },
}
NTFY_BODY_TEMPLATE = "Event: {event}\nDevice: {device}\nDuration: {duration_s}s"


def build_payload(
    event,
    device=DEFAULT_DEVICE_NAME,
    state="ARMED",
    alert_mode="OUTAGE_AND_RESTORE",
    duration_s=None,
    uptime_s=None,
    now=None,
):
    """Build a contract-compliant HTTP/webhook JSON payload for the given event."""
    ev = event.upper()
    ts_val = int(time.time()) if now is None else int(now)

    if ev == "POWER_OFF":
        return {
            "event": "POWER_OFF",
            "device": device,
            "state": "ARMED",
            "grid": "OFFLINE",
            "power": False,
            "armed": True,
            "duration_s": 0 if duration_s is None else int(duration_s),
            "uptime_s": 120 if uptime_s is None else int(uptime_s),
            "ts": ts_val,
        }

    if ev == "POWER_ON":
        return {
            "event": "POWER_ON",
            "device": device,
            "state": state,
            "grid": "ONLINE",
            "power": True,
            "armed": (state == "ARMED"),
            "duration_s": 120 if duration_s is None else int(duration_s),
            "uptime_s": 240 if uptime_s is None else int(uptime_s),
            "alert_mode": alert_mode,
            "ts": ts_val,
        }

    if ev == "TEST_ALARM":
        return {
            "event": "TEST_ALARM",
            "device": device,
            "state": state,
            "grid": "TEST",
            "power": False,
            "armed": (state == "ARMED"),
            "duration_s": 0,
            "uptime_s": 600 if uptime_s is None else int(uptime_s),
            "ts": ts_val,
        }

    raise ValueError(f"Unknown event: {event}")


def build_ntfy_headers(event):
    """Build ntfy HTTP headers matching firmware network_mgr.cpp literals."""
    ev = event.upper()
    if ev not in NTFY_TEMPLATES:
        raise ValueError(f"Unsupported ntfy event: {event}")
    return dict(NTFY_TEMPLATES[ev])


def build_ntfy_body(event, device=DEFAULT_DEVICE_NAME, duration_s=0):
    """Build plain-text ntfy message body."""
    return NTFY_BODY_TEMPLATE.format(
        event=event.upper(),
        device=device,
        duration_s=int(duration_s),
    )


def expand_target_url(url_template, event):
    """Expand {event} placeholder into lowercase event token."""
    return url_template.replace("{event}", event.lower())


def send_http_target(url_template, payload_obj, timeout=10.0):
    """POST event to a local HTTP webhook or ntfy.sh target."""
    ev = payload_obj["event"]
    expanded_url = expand_target_url(url_template, ev)
    parsed = urllib.parse.urlparse(expanded_url)

    if parsed.hostname == "ntfy.sh":
        headers = build_ntfy_headers(ev)
        body_bytes = build_ntfy_body(
            ev,
            device=payload_obj["device"],
            duration_s=payload_obj.get("duration_s", 0),
        ).encode("utf-8")
    else:
        headers = {"Content-Type": "application/json"}
        body_bytes = json.dumps(payload_obj).encode("utf-8")

    print(f"[*] Sending HTTP POST to {expanded_url}...")
    req = urllib.request.Request(expanded_url, data=body_bytes, headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        if not (200 <= resp.status < 300):
            raise RuntimeError(f"Unexpected HTTP status {resp.status} from {expanded_url}")
        print(f"[+] Response: HTTP {resp.status} {resp.reason}")
        return resp.status


def _build_parser():
    parser = argparse.ArgumentParser(
        description="Power Outage Monitor Mobile Alarm Bench Simulator"
    )
    sub = parser.add_subparsers(dest="subcommand", required=True)

    p_http = sub.add_parser("http", help="POST event JSON to a local HTTP target (supports {event})")
    p_http.add_argument(
        "event",
        type=str.lower,
        choices=[e.lower() for e in ALERT_EVENTS],
        help="Alert event to POST",
    )
    p_http.add_argument(
        "--target",
        required=True,
        help="Target URL template (for example http://192.168.1.60:8080/{event})",
    )
    p_http.add_argument("--device", default=DEFAULT_DEVICE_NAME, help="Device name in payload")
    p_http.add_argument(
        "--state",
        choices=["ARMED", "DISARMED", "SNOOZED"],
        default="ARMED",
        help="Mode for events carrying state (default: ARMED)",
    )
    p_http.add_argument(
        "--alert-mode",
        choices=["OUTAGE_ONLY", "OUTAGE_AND_RESTORE"],
        default="OUTAGE_AND_RESTORE",
        help="Alert mode for power_on (default: OUTAGE_AND_RESTORE)",
    )
    p_http.add_argument("--duration-s", type=int, default=None, help="Outage duration in seconds")
    p_http.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout in seconds")

    p_ntfy = sub.add_parser("ntfy", help="Publish event to ntfy.sh in firmware format")
    p_ntfy.add_argument(
        "event",
        type=str.lower,
        choices=[e.lower() for e in ALERT_EVENTS],
        help="Alert event to publish",
    )
    p_ntfy.add_argument("--topic", required=True, help="ntfy.sh topic name")
    p_ntfy.add_argument("--device", default=DEFAULT_DEVICE_NAME, help="Device name")
    p_ntfy.add_argument("--duration-s", type=int, default=0, help="Outage duration in seconds")
    p_ntfy.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout in seconds")

    return parser


def main(argv=None):
    parser = _build_parser()
    args = parser.parse_args(argv)

    try:
        if args.subcommand == "http":
            payload = build_payload(
                args.event,
                device=args.device,
                state=args.state,
                alert_mode=args.alert_mode,
                duration_s=args.duration_s,
            )
            send_http_target(args.target, payload, timeout=args.timeout)
            return

        if args.subcommand == "ntfy":
            payload = build_payload(
                args.event,
                device=args.device,
                duration_s=args.duration_s,
            )
            send_http_target(f"https://ntfy.sh/{args.topic}", payload, timeout=args.timeout)
            return
    except (OSError, urllib.error.HTTPError, urllib.error.URLError, RuntimeError, ValueError) as exc:
        print(f"[-] Error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
