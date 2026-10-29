# HDMI-CEC Display Control

Your TV follows your Windows PC over HDMI-CEC. It turns off when the PC sleeps,
shuts down, or turns its screen off, and comes back on when you return. The
volume buttons and the TV remote work with the PC, and Switch Input puts any
device on the TV on screen from the PC.

Graphics cards do not speak CEC over HDMI, but DisplayPort can carry it, and a
few DisplayPort to HDMI adapters pass it through. This app drives the TV through
one of those adapters, on NVIDIA, AMD, and Intel graphics.

## What you need

- A DisplayPort to HDMI adapter that carries CEC. Most do not, and the box will
  not say. These two are known to work:
  - UGREEN DP134 8K@60Hz Active DisplayPort to HDMI adapter (model 85564), for
    a PC with a DisplayPort socket.
  - A USB-C to HDMI 2.1 cable built on the Chrontel CH7218 chip, for a PC or
    laptop with USB-C video out.
- A TV with CEC turned on in its own settings. It is usually off on a new TV,
  and most makers rename it: SIMPLINK on LG, Anynet+ on Samsung, BRAVIA Sync on
  Sony, HDMI-CEC on Fire TV, and just CEC on TCL and Vizio.
- 64-bit Windows 11, with an NVIDIA, AMD, or Intel graphics card. Windows 10
  is supported in the code but has not been tested.

## Installing

1. Download `CecSetup.exe` from the Releases page and run it.
2. Windows may show a blue box saying "Windows protected your PC". That is
   SmartScreen, which appears for any app that has not yet been downloaded by
   many people, or that is not signed with a paid certificate. Click "More
   info", and then "Run anyway".
3. Windows then asks for permission and shows the publisher as unknown, for the
   same reason. Click Yes. The installer needs that permission because it sets
   up a small background service, which is what lets the TV follow the PC even
   before anyone has signed in.

To remove it, open Settings, Apps, Installed apps, find HDMI-CEC Display
Control, and choose Uninstall. Everything it added goes, including its settings
and its log.

## What it does

- Turns the TV off when the PC shuts down, sleeps, or turns its screen off, each
  of which can be switched off in the menu, and back on when someone uses the
  PC again. A TV showing another input is left alone.
- A volume panel from the tray icon, with a live level on TVs that report one,
  and the keyboard's volume keys sent to the TV if you want them to be.
- The TV remote drives the PC: arrows, select, back, and the media keys, with
  any button able to send a different key or a mouse click.
- Switch Input lists everything on the TV's HDMI ports and puts it on screen,
  including another PC running this app.
- A hotkey (Ctrl+Alt+Shift+P) that brings the TV back to this PC from anything.
- Keep PC and TV awake, for a long video or presentation.
- A guide in the menu that answers the common questions.

There is no network code in the app. It does not check for updates, report
anything, or send any information anywhere.

## How it works

CEC is a one-wire bus that every device on a TV's HDMI ports shares. DisplayPort
1.3 and later can tunnel it through the AUX channel, a slow side channel next to
the video, and an adapter built on a chip such as the Chrontel CH7218 puts it on
the HDMI side. Windows has no API for any of this, so the app reaches the AUX
channel through each graphics vendor's own driver library and reads and writes
the CEC tunnelling registers (DPCD 0x3000 to 0x3021) directly.

| Part | What it is |
|---|---|
| `CecService` | A service running as SYSTEM. It owns the adapter, follows power and display events, answers the TV, and keeps the list of devices on the bus. |
| `CecTray` | The tray app: the menu, the volume panel, the remote and keyboard keys, and the guide. It talks to the service over a named pipe and never touches the hardware. |
| `CecTest` | A command line tool for bench work: probing ports, sending single frames, and watching the bus. Not installed. |
| `CecSetup` | A single-file installer and uninstaller, with the service and tray app built into it. |
| `CecTest/CecDevice.*` | The CEC protocol over DPCD, shared by all of the above. |
| `CecTest/NvidiaBackend.*`, `AmdBackend.*`, `IntelBackend.*` | AUX access through NVAPI, AMD ADL, and Intel IGCL. |
| `tests/` | Logic tests: copies of the service's decisions run against scripted hardware. |

## Building

1. Install Visual Studio 2022 or later with the "Desktop development with C++"
   workload.
2. Clone NVIDIA's NVAPI SDK into `third_party\nvapi`:

   ```
   git clone https://github.com/NVIDIA/nvapi third_party/nvapi
   ```

   Or point the build at a copy elsewhere with `/p:NvApiDir=C:\path\to\nvapi\`.
   The AMD and Intel libraries are loaded at run time and need nothing at build
   time.
3. Run `build-installer.ps1`. It builds every project in Release and leaves the
   installer at `CecSetup\x64\Release\CecSetup.exe`.

To work on the service, `rebuild-service.ps1` (from an elevated PowerShell)
builds it and reinstalls it in place. Its log is at
`C:\ProgramData\CecService\service.log`, and setting
`HKLM\SOFTWARE\CecService\LogCecTraffic` to 1 adds every CEC frame to it.

Run the logic tests with `tests\run-tests.cmd`. `tests\README.md` says what each
one covers.

`tv-report.ps1` collects one TV's worth of evidence (what it switched to, what
it reported, and every frame) into a single file, which is the most useful thing
to attach to a report about a TV that misbehaves.

## Rules that each cost a real bug

Read these before changing the power or volume paths.

- `<Standby>` is a power toggle on some TVs, so it is sent once. It is only sent
  again after the TV has answered "on" to a question asked at least 2.5 seconds
  after the first, and never inside the few seconds a suspend allows.
- Nothing is announced between a volume press and its release, and a release
  always goes where its press went. A lost release leaves the TV repeating the
  key.
- A send touches only the transmit bits of DPCD 0x3004. The receive bits belong
  to the receive path, and clearing them loses the TV's messages.
- The logical address mask is never left empty, and `<Menu Request>` and
  forwarded remote keys are never answered with `<Feature Abort>`.
- The adapter's transmit status says the frame went out, not that the TV acted
  on it. Ask the TV with `<Give Device Power Status>`.
- The adapter's receive buffer tears frames: one frame's header can arrive in
  front of another's body. A frame is believed only at its exact length, from
  the device a question went to, and another PC running this app only once it
  has answered a question sent to its own address.
- Source files stay ASCII.

## Contact

Write to business.pdm@proton.me. The service log helps more than anything else:
it is at `C:\ProgramData\CecService\service.log`, and the guide in the app says
how to send it.

## Licence

Copyright (C) 2026 PDM-27.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the full terms.

In short: anyone may use, change, and share it, and anyone who shares a
changed version must publish its source under the same licence.

The build links NVIDIA's NVAPI library, which is under the MIT licence; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
