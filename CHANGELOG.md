# Changelog

Current release: **3.1.7**.

## 3.1.7 — 2026-11-24

- HDR uses the deep navy and bright cyan of the CPU overview. Metrics figures and the text in that color use that cyan and Bahnschrift.

## 3.1.6 — 2026-11-24

- A finished step shows its result. It no longer stays on Preparing after the work is done.

## 3.1.5 — 2026-11-24

- The health check now reads profile, game watch, hardware changes, the saved bench, approved scripts, Focus, the share file, and live metrics, along with network and audio.

## 3.1.4 — 2026-11-24

- Metrics samples on its own thread every quarter second. The window keeps drawing at the display rate, and the curve glides between those samples.

## 3.1.3 — 2026-11-24

- The Restart button is gone from Home. Full tune spans the top of the tool list.

## 3.1.2 — 2026-11-24

- Each new step fills its own pane. Network, audio, profile, games, hardware, share, scripts, and focus draw cards, bars, and gauges instead of a block of text.
- Bench draws the baseline and the latest run as paired bars for sleep, wake, DPC, CPU, memory, and disk.
- Metrics fills the pane: four gauges, CPU, memory, DPC, and GPU or clock traces, and a strip for the timer, ISR, clock, and temperature.
- The metrics curve samples every frame, slides with the clock, and uses a smooth line in the same cyan and violet as the home timing curve.
- The home list is one row per tune zone, with a green or red box. The detail is on the pointer.

## 3.1.1 — 2026-11-24

- Network, Audio, Profile, Games, Hardware, Bench, Share, Scripts, Metrics, and Focus each open from Home and run on their own, the same way CPU, Memory, and Storage do.
- Those steps are also checkboxes on the full-tune welcome screen and in Settings. A checked step runs with the full tune. An unchecked step is skipped there and can still be opened from Home.

## 3.0.2 — 2026-11-24

- HDR light blue stays blue. The wide white bloom is gone. Accents are brighter, with only a slight glow in the same color.
- Network and Audio are on the welcome list and in Settings, with the other full-tune steps.
- Settings and the tray can switch Performance and Balanced. A running game can lift Balanced, hold notification banners, and approved game scripts still run.
- A GPU, CPU, BIOS, or Windows change is reported in Settings when hardware watch is on.

## 3.0.1 — 2026-11-24

- Network and audio join the full tune as named steps: adapter power saving, interrupt moderation, Windows Update delivery, and audio enhancements, each with a restore path.
- Performance and Balanced profiles switch the tuned power plan and the timer hold without touching the rest of the tune.
- A first full tune can take a quiet baseline bench before anything is changed, so later runs have something to compare with.
- Hardware-change detection remembers GPU, driver, CPU, BIOS, and Windows signatures and says which steps to run again.
- Export, import, and share codes move step choices and preferences without device ids. Approved scripts under `Scripts\` can run after a tune or when a profile or game starts.
- Focus Assist quiet mode and the Game Bar button can be held off while Synchronus is open; both go back on uninstall.
- Live metrics and the bench history sit beside the existing timer measurement. HDR from 2.10.3 stays at the bottom left of every pane.

## 2.10.3 — 2026-11-24

- HDR color keeps the same theme and opens it onto a wider scale: deeper panels, brighter accents, and a wider glow. The light blue is brighter still, and its glow spills past the controls.
- The HDR control stays at the bottom left of every pane. The choice is remembered.

## 2.10.2 — 2026-11-05

- Stop no longer sits on the CPU step counter during a full tune, or on the same counter in the other step panes.
- The title, the home mark, and the step line in the header stay in their own columns. Prompt text stops above its buttons, and a step's name no longer runs through the value beside it.

## 2.10.1 — 2026-11-05

- Moving between panes slides and fades the one you are leaving, and brings the next one up with a light sweep. Home, Aggregate, and About use the same motion.
- The Aggregate curve keeps its axis labels above the lowest-score line.
- About sits beside Home and Aggregate. It credits Van Graham, the GitHub repositories this program is built from, and Lucius & Wayne Systems, and each credit opens its link.

## 2.9.2 — 2026-11-05

- The Aggregate history has a scrollbar. Date controls move through the days that have tune entries, and the column headings sort those entries by time, step, or what changed.
- The home screen no longer reserves a heading above the tool buttons.

## 2.9.1 — 2026-11-05

- A newer Synchronus replaces an older copy that is already running, then points startup at the newest installed executable. The boot service, the logon entry, a startup shortcut, and the startup guard follow that same copy, including after later updates.
- When the Afterburner scan finishes, the curve is applied to the current settings, saved as profile 1, and started again at every logon. The logon task runs in the signed-in session and passes that profile.

## 2.8.6 — 2026-11-02

- During a full tune, the timer pane stays large enough to read while other steps run beside it.
- That pane shows the sweep graph and the measurement lattice, sized to the pane, and follows the scan as intervals complete.

## 2.8.5 — 2026-11-01

- After more than one timer run is saved, the resolution that is set is the lowest aggregate score. A single run still uses that run's verified pick. An explicit resolution pin is kept. This applies to the boot service, a repair, and the next time the service starts.

## 2.8.4 — 2026-11-01

- The Aggregate page title separates runs and samples with a plain bar. The previous mark did not draw in the heading font.

## 2.8.3 — 2026-11-01

- Full tune panes keep the current step and its explanation fully visible above the checklist. Those lines are no longer clipped by the step list.

## 2.8.2 — 2026-11-01

- During a full tune, each running step pane shows what it is working on, step counts where they apply, and a progress bar (including download and measurement progress).

## 2.8.1 — 2026-10-30

- Each timer interval keeps a sample-weighted master measurement. A finished run is folded in by its sample count, so later runs move the curve less as the weight grows.
- The home screen shows that aggregate curve beside the 3D scan. The Aggregate tab shows the full curve and the other tuned values over time.
- After an update, the new version opens its window. Start minimized stays saved and still applies on the next normal start.

## 2.7.3 — 2026-10-27

- The Afterburner scan uses the PCI-E graphics card. Integrated graphics are not the scan target.

## 2.7.2 — 2026-10-27

- The home screen no longer shows the home control. That control remains on the other screens.

## 2.7.0 — 2026-10-27

- The sliders at the right end of the top title open Settings. It saves the driver folder, the Afterburner folder, startup and tray behavior, notifications, health monitoring, automatic maintenance, and the full-tune steps.
- Installing an update downloads the new build, leaves the window and the tray, replaces this copy, and opens the new one.
- Health scans, maintenance checks, update checks, and the gap between notifications use the intervals set there. The usual values are 6 hours, 1 minute, 1 minute, and 30 minutes.
- A measured boot service is installed on its own. Synchronus no longer asks before that install.

## 2.6.3 — 2026-10-27

- Synchronus checks for an update when it starts, and again once a minute in the background. The same version is announced only once.

## 2.6.2 — 2026-10-27

- The taskbar button uses a filled mark so it matches the size of other running programs. The tray icon is unchanged.

## 2.6.1 — 2026-10-27

- Full tune makes the restore point first and does not start anything else until that finishes. If it fails, the other steps stay stopped until you retry or continue without one.
- The timer measurement runs by itself, so later changes do not disturb it. CPU power and memory can then run together. Storage waits until the CPU step is finished. GPU settings and Afterburner wait until the driver choice is finished.
- The home screen 3D chart follows a timer scan while it is running. It shows the last finished scan only when nothing is measuring.
- Under the attention count, a short note says that not every item applies on all hardware.

## 2.5.1 — 2026-10-27

- The first time Synchronus opens, a checked box offers to start with Windows, minimized to the tray. Uncheck it if you do not want that. After that, it keeps running on its own.
- Older unused services, including the old TimerResolution service and a Synchronus service whose program file is gone, are removed. Saved preferences and the Synchronus data folder stay.
- If a monitored setting keeps switching off or returning to the default, Synchronus applies a stronger fix: the timer service restarts itself, power values are written into every plan, and services that should stay off are pinned that way. A startup guard repeats those fixes. It does not install a driver or run the Afterburner scan.

## 2.5.0 — 2026-10-27

- Synchronus asks at startup whether to keep the timing structure maintained. The same switch is on the tray menu and the home screen.
- While that is on, the monitored settings are read once a minute. A setting that has drifted is put back. A driver download and the Afterburner scan are not started by this pass.

## 2.4.1 — 2026-10-27

- The Install update button on the home screen flashes while a newer build is ready.

## 2.4.0 — 2026-10-27

- Full tune runs the selected steps together. It asks only before a graphics-driver install and before Afterburner.
- Each step that is actually running, or waiting for that choice, gets its own pane. A step is not started unless a pane is free to show it.
- Steps that share the clock, the power plan, the disk, or the graphics driver still wait. Opening one tool on its own still asks before that tool runs.

## 2.3.0 — 2026-10-26

- Each step has its own worker. Opening another tool, or going home, leaves the one you started running.
- Starting a step that is already running opens it instead of starting a second copy.
- The timer measurement and the boot-service install share the clock, so one waits.
- CPU power and storage share the power plan, so one waits.
- A restore point, the pagefile change, and a driver install share the disk, so one waits.
- Driver install, GPU settings, and Afterburner share the graphics driver, so one waits. GPU settings and Afterburner stay queued until a driver install you started has finished.
- If a driver is installed after Afterburner already scanned, that screen says to run Afterburner again. GPU settings that already ran are written again after the new driver.
- A health repair uses that same worker, so it cannot run a second copy of a step that is already going.

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
