# In-car install capture kit

This folder is Syncthing-synced between desktop and laptop. Run the capture
on the laptop (the machine that goes in the car); the output lands in
`./captures/<timestamp>/` and rounds back to the desktop automatically.

## What it captures

Three streams in parallel, all timestamped:

1. **Frida hooks** on `APManager.exe`'s Windows crypto APIs + file I/O + the
   static AES tables in the binary. Catches any PC-side crypto operation if
   one fires.
2. **USBPcap** on the AccessPort's USB bus. Every byte going PC↔AP across
   the install.
3. **Joint JSONL event log** so we can correlate Frida events with USB packets
   by wall-clock time.

## One-time setup on the laptop

1. **Wireshark + USBPcap** (free):
   - Download Wireshark Win64 installer from https://www.wireshark.org/download.html
   - During install, tick "USBPcap" in the components list — installs the kernel driver
   - Reboot when prompted (the driver needs it)
   - Verify: open Wireshark, you should see `USBPcap1`, `USBPcap2`, etc. in the interface list

2. **Python + Frida**:
   ```
   pip install frida-tools
   ```
   Already installed on the desktop; install on the laptop too.

3. **Locate APManager.exe** on the laptop. Default path is `C:/Program Files/Accessport Manager/APManager.exe`
   or `C:/Program Files (x86)/...`. The script auto-detects but you can override with `--apm PATH`.

## Running the capture

In a normal CMD or PowerShell on the laptop:

```
cd C:\Users\Cornelio\Desktop\Subuwu\Tools\ap_install_capture
python capture_install.py --label fehr-install
```

The script will:
- Show you the USB buses it sees (pick the one the AP is on — usually the
  one labeled with a hub the AP plugs into)
- Spawn APManager under Frida hooks
- Start USBPcap recording
- Print a "READY TO CAPTURE" banner

Then:

1. Make sure the AP is connected to the laptop via the long USB cable, AND
   to the car's OBD port via the OBD cable
2. On the AP / in APManager, kick off the install
3. Let it finish (do not unplug or disconnect until done)
4. Back at the laptop terminal, press **Ctrl-C**
5. The script writes everything to
   `./captures/<timestamp>_fehr-install/` and shuts down

## What lands in the capture folder

```
captures/20260606_180000_fehr-install/
├── session_info.json     # host/timestamp/label metadata
├── frida_events.jsonl    # every crypto/file event, with _ts (epoch seconds)
├── usb.pcapng            # raw USB capture (open in Wireshark)
└── usbpcap.log           # USBPcap stderr (for debugging)
```

## What to do after

Don't analyze on the laptop. Just let Syncthing carry the capture folder
back to the desktop:

```
D:\Subuwu\code\tools\ap_install_capture\captures\<timestamp>_<label>\
```

Then tell Claude on the desktop and I'll:

1. Walk the Frida event log for any crypto API hits that didn't fire in the
   library/idle captures
2. Parse the USB pcap to identify endpoint structure, frame timing, and
   payload patterns
3. Find the AES key + IV if PC-side decryption fires, OR confirm AP-only
   decryption and pin down the wire protocol

## Skipping pieces

- `--no-frida`: USB only (in case Frida itself causes trouble)
- `--no-usb`: Frida only (no USBPcap dependency)
- `--bus '\\.\USBPcap1'`: skip the interactive bus picker

## Troubleshooting

- **USBPcap not found**: re-run the Wireshark installer and tick USBPcap in
  the components list. Reboot.
- **Frida says "unsupported architecture"**: APManager.exe is 32-bit i386.
  Make sure you have a 32-bit Python or use 64-bit Python with Frida >= 17
  (which loads the 32-bit agent automatically).
- **No bytes captured on USB**: you picked the wrong bus. Stop, restart, and
  pick the USB controller the AP is actually plugged into. (Tip: in Device
  Manager, with the AP plugged in, the parent hub of the AP's USB device is
  the one to capture.)
