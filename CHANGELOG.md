# Changelog

Current release: **2.2.0**.

## 2.2.0 — 2026-10-26

- A house icon is centered at the top of every screen and returns to home. It is wide, and its edges fade into the window so it reads as a laser etch rather than a button.
- Tweaks on the home screen sit in a separate rounded group for each step, held against the right edge. Groups wrap into the next column instead of scrolling off the window. A box lights green when that tweak is applied and red when it is not.
- The center of the home screen describes whichever tool, tweak, or the house the pointer is over.
- The lower left of the home screen draws the 3D lattice from the most recent timer-resolution scan.
- Tools, groups, and the heatmap are larger so they fill the default window.

## 2.1.0 — 2026-10-26

- The tray tip uses plain text. The icon is drawn to fill the tray slot.
- The first launch still walks through setup, and asks whether to keep the tray monitor and start with Windows. The default answer is yes.
- Later launches, and a normal click of the tray icon, open a home screen with a button for each function and a live list of every tweak.
- The health check reads every processor-power, storage-power, and graphics setting the tuner writes, plus the timer, memory, TRIM, and SysMain checks. A tweak that matches is live. A tweak that does not match needs to be put back.
- The step choices from the first run are remembered. An available update pulses the tray icon and posts a Windows notification.

## 2.0.0 — 2026-10-26

- Synchronus stays in the system tray after you close the window. Exit is a separate tray command. A second launch brings the existing window back.
- Health checks read the live timer service, timer flag, power-plan values, GPU scheduling, Game DVR, display-driver version, Afterburner, RivaTuner, pagefile, memory compression, storage power settings, SysMain, and TRIM. A previous "applied" flag is not treated as proof.
- Passing, needs attention, informational, unknown, and inaccessible are separate. Repairs call the existing apply steps and then recheck. Routine scans do not change the machine.
- Start with Windows and notifications are off until you turn them on from the tray menu. The default background check is every six hours.
- `Synchronus.exe --health` prints a read-only report. `Synchronus.exe --self-test` checks the status rules without touching the system.

## 1.12.8 — 2026-10-25

- The BIOS checklist is no longer its own step. Windows cannot change those settings.
- The restart screen, and the console finish, list what to set by hand: a locked multiplier, deep C-states off, XMP or DOCP, and Above 4G if you want Resizable BAR.

## 1.12.7 — 2026-10-24

- The OC scanner opens on its own and then sits on its Scan button. That button is pressed once the scanner is connected to Afterburner, so the test starts without a click.
- The step watches the scanner's report and finishes when the curve is exported.
- The Afterburner step says that the download and the OC scan take about 30 minutes.
- The scanned curve is saved as Afterburner profile 1 and as the startup profile. The logon task starts Afterburner with that profile.

## 1.12.6 — 2026-10-24

- Afterburner is its own step, separate from the GPU driver settings. Uncheck it, or pass `--no-afterburner`, to skip it. `--apply-afterburner` runs only that step.
- The step downloads MSI Afterburner 4.6.6 and installs RivaTuner Statistics Server with it when either is missing.
- On an NVIDIA GPU it starts Afterburner and runs MSI's OC scanner. The scan can take about 30 minutes.
- Afterburner and RivaTuner are set to start at every logon. Afterburner starts minimized and applies the saved overclock.

## 1.12.5 — 2026-10-24

- If 7-Zip is not already installed, the driver step downloads the official 7-Zip 26.05 installer from 7-zip.org and installs it silently before unpacking.
- The NVIDIA package (display driver and PhysX only) and the AMD display-driver package both use that step.

## 1.12.4 — 2026-10-23

- The measurement lattice axes are late percent, measured latency, and sample count.
- Late percent is the running share of missed ticks for the resolution being measured. Measured latency is that sample's `Sleep(1)` overshoot. Sample count is how many samples landed in the cell.

## 1.12.3 — 2026-10-23

- Published so a 1.12.2 install can update.
- The project, solution, and source files are named Synchronus. Messages about the running service say Synchronus.
- An older install whose service is still named TimerResolution is still recognized and replaced.

## 1.12.2 — 2026-10-23

- Opening the window checks `https://luciuswayne.com/synchronus/latest.json` and offers a newer build.
- The installer may be `https://media.luciuswayne.com/Synchronus.exe` or `https://media.luciuswayne.com/Synchronus-<release>.exe`.
- A verified update download is reported to the site download counter.
- The update offer is shown as soon as the check finishes.

## 1.12.1 — 2026-10-23

- First release in this line.
- One executable measures `Sleep(1)` from 0.50–0.60 ms, installs a boot service at the winner, and then applies the named CPU, GPU, memory, and storage steps.
- This build does not check the website for a newer version.
