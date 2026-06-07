"""capture_install.py — laptop-side orchestrator for an in-car AP install.

Runs THREE captures simultaneously while you do the in-car install with AP
tethered via long USB cable:

  1. Frida hooks on APManager.exe — catches any PC-side crypto API call,
     ReadFile/WriteFile on .ptm/.img/.rom, and reads of the static AES tables.
  2. USBPcap on the AccessPort's USB interface — captures every byte
     PC↔AP across the install.
  3. A combined event log tying both streams together by timestamp.

Outputs go to ./captures/<timestamp>/ which is on a Syncthing-synced folder
so the desktop sees them once the laptop is back online.

Usage (on the laptop):
    cd C:\\Users\\Cornelio\\Desktop\\Subuwu\\Tools\\ap_install_capture
    python capture_install.py [--apm PATH] [--no-frida] [--no-usb]

The script will:
  - Print which USB bus the AP is on (prompts to confirm)
  - Spawn APManager under Frida
  - Start USBPcapCMD.exe to record the AP's USB bus
  - Print a clear "GO" banner — that's when you start the install
  - Hit Ctrl-C when the install completes; the script stops both captures
    and prints a summary
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).parent
HOOK = HERE / "apm_crypto_hook.js"
CAPTURES = HERE / "captures"

DEFAULT_APM = r"D:/Programs/Accessport Manager/APManager.exe"
LAPTOP_APM = r"C:/Program Files/Accessport Manager/APManager.exe"
LAPTOP_APM_X86 = r"C:/Program Files (x86)/Accessport Manager/APManager.exe"
USBPCAPCMD_CANDIDATES = [
    r"C:/Program Files/USBPcap/USBPcapCMD.exe",
    r"C:/Program Files (x86)/USBPcap/USBPcapCMD.exe",
]


def find_apm(user_path: str | None) -> str:
    candidates = [user_path] if user_path else [
        DEFAULT_APM, LAPTOP_APM, LAPTOP_APM_X86,
    ]
    for c in candidates:
        if c and Path(c).exists():
            return c
    sys.exit(
        "Could not locate APManager.exe. Specify with --apm PATH. Checked:\n  "
        + "\n  ".join(c for c in candidates if c)
    )


def find_usbpcap() -> str | None:
    for c in USBPCAPCMD_CANDIDATES:
        if Path(c).exists():
            return c
    return None


def list_usb_buses(usbpcap_cmd: str):
    """List all USB buses USBPcap can see. Print their devices."""
    result = subprocess.run([usbpcap_cmd, "--extcap-interfaces"],
                            capture_output=True, text=True, timeout=10)
    return result.stdout


def pick_usb_bus(usbpcap_cmd: str) -> tuple[str, str]:
    """Walk the user through picking the AP's USB bus.
    Returns (bus_filter, friendly_label).
    """
    print("\n=== USB buses visible to USBPcap ===")
    out = list_usb_buses(usbpcap_cmd)
    print(out)

    # Parse output lines like: interface {value=\\.\USBPcap1}{display=USBPcap1}
    buses = []
    for line in out.splitlines():
        m = re.search(r"value=(\\\\\.\\USBPcap\d+)\}\{display=([^}]+)", line)
        if m:
            buses.append((m.group(1), m.group(2)))

    if not buses:
        print("WARNING: no USBPcap interfaces detected. Is the USBPcap driver installed?")
        print("Install: https://desowin.org/usbpcap/  (also installs as a Wireshark add-on)")
        return None, None

    print(f"Found {len(buses)} USB bus(es):")
    for i, (val, disp) in enumerate(buses):
        print(f"  [{i}] {disp}  ({val})")

    print()
    print("In Device Manager (devmgmt.msc), expand 'Universal Serial Bus controllers',")
    print("right-click each USB Root Hub → Properties → Details → 'Bus number' to find the AP's bus.")
    print("Or just pick the bus the AccessPort is plugged into.")
    sel = input(f"Pick bus [0-{len(buses)-1}] (default 0): ").strip() or "0"
    try:
        idx = int(sel)
    except ValueError:
        sys.exit("Invalid selection.")
    return buses[idx]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--apm", default=None, help="Path to APManager.exe")
    p.add_argument("--no-frida", action="store_true", help="Skip Frida hooking")
    p.add_argument("--no-usb", action="store_true", help="Skip USB capture")
    p.add_argument("--bus", default=None,
                   help="USBPcap interface (e.g. \\\\.\\USBPcap1) — skip the interactive picker")
    p.add_argument("--label", default="install",
                   help="Label for this capture session (default 'install')")
    args = p.parse_args()

    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = CAPTURES / f"{ts}_{args.label}"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[*] capture output dir: {out_dir}")
    (out_dir / "session_info.json").write_text(json.dumps({
        "timestamp": ts,
        "label": args.label,
        "host": os.environ.get("COMPUTERNAME"),
        "user": os.environ.get("USERNAME"),
    }, indent=2))

    procs = []
    usbpcap_log = None

    # --- USB capture setup ---
    if not args.no_usb:
        usbpcap_cmd = find_usbpcap()
        if not usbpcap_cmd:
            print("[!] USBPcapCMD.exe not found. Install USBPcap from https://desowin.org/usbpcap/")
            print("[!] (Wireshark with USBPcap add-on is the simplest path.)")
            print("[!] Continuing WITHOUT USB capture. Use Wireshark GUI separately if you want USB.")
        else:
            if args.bus:
                bus_val = args.bus
                bus_label = bus_val
            else:
                bus_val, bus_label = pick_usb_bus(usbpcap_cmd)
            if bus_val:
                pcap_out = out_dir / "usb.pcapng"
                print(f"[*] starting USBPcap on {bus_label} → {pcap_out}")
                usbpcap_log = (out_dir / "usbpcap.log").open("w")
                # USBPcapCMD.exe: -d <interface> -o <output>
                # Pcap-ng format by default in recent builds; older builds use libpcap
                p_usb = subprocess.Popen(
                    [usbpcap_cmd, "-d", bus_val, "-o", str(pcap_out)],
                    stdout=usbpcap_log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
                )
                procs.append(("usbpcap", p_usb))
                time.sleep(1)  # let it start

    # --- Frida hook setup ---
    if not args.no_frida:
        if not HOOK.exists():
            sys.exit(f"Hook script missing: {HOOK}")
        try:
            import frida  # type: ignore
        except ImportError:
            print("[!] Frida not installed. Run: pip install frida-tools")
            print("[!] Continuing WITHOUT Frida capture. USB-only.")
        else:
            apm = find_apm(args.apm)
            print(f"[*] spawning APManager: {apm}")
            device = frida.get_local_device()
            pid = device.spawn([apm])
            session = device.attach(pid)
            print(f"[*] PID {pid}")
            script = session.create_script(HOOK.read_text(encoding="utf-8"))

            frida_log = (out_dir / "frida_events.jsonl").open("w", buffering=1)

            def on_message(message, data):
                if message.get("type") == "send":
                    payload = message["payload"]
                    payload["_ts"] = time.time()
                    frida_log.write(json.dumps(payload, ensure_ascii=False) + "\n")
                elif message.get("type") == "error":
                    err = {"_meta": "frida_error", **message, "_ts": time.time()}
                    frida_log.write(json.dumps(err, default=str) + "\n")
                    print(f"[frida-error] {message.get('description')}",
                          file=sys.stderr)

            script.on("message", on_message)
            script.load()
            device.resume(pid)
            procs.append(("frida", (session, script, frida_log)))
            print("[*] Frida hooks loaded and APManager resumed")

    if not procs:
        sys.exit("No captures active. Use --no-frida and --no-usb together to skip all (pointless).")

    # --- ready banner ---
    print()
    print("=" * 60)
    print(" READY TO CAPTURE")
    print(" ─────────────────────────────────────────────────────")
    print(" 1. Make sure the long USB cable is connecting the AP")
    print("    to this laptop while the AP's OBD-II cable goes to")
    print("    the car.")
    print(" 2. Start the install on the AP/APManager as usual.")
    print(" 3. When the install finishes, press Ctrl-C here.")
    print(" 4. Output goes to:")
    print(f"      {out_dir}")
    print(" 5. It'll Syncthing back to the desktop automatically.")
    print("=" * 60)
    print()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[*] stopping captures...")

    # --- teardown ---
    for kind, proc in procs:
        try:
            if kind == "usbpcap":
                proc.send_signal(signal.CTRL_BREAK_EVENT)
                proc.wait(timeout=10)
            elif kind == "frida":
                session, script, log = proc
                script.unload()
                session.detach()
                log.close()
        except Exception as e:
            print(f"[!] cleanup error on {kind}: {e}")

    if usbpcap_log:
        usbpcap_log.close()

    print(f"[*] done. captures in: {out_dir}")
    print()
    print("Files saved:")
    for p in out_dir.iterdir():
        size = p.stat().st_size
        print(f"  {p.name}  ({size:,} bytes)")
    print()
    print("Once Syncthing catches up, the desktop will have:")
    print(f"  D:\\Subuwu\\code\\tools\\ap_install_capture\\captures\\{ts}_{args.label}\\")


if __name__ == "__main__":
    main()
