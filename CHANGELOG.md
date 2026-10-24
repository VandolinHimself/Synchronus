# Changelog

Current release: **1.12.6**.

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
