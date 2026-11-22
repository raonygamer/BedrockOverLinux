# Launch and shutdown timings

The [individual timings](launch.csv) compare commit `1500a06` with the launch changes in this directory's commit. Both builds ran Minecraft for Windows 26.65.01 on Arch Linux x86_64, kernel 7.3.5, with Xodus 0.7.4 and WineGDK 11.18-15-winrt. Gamescope ran headless at 1024 × 640 on an AMD Navi 33 GPU. Xodus was already running and the game was already installed. Every profile used isolated game data.

- **New prefix and existing prefix.** Each build launched into two empty prefixes on the same local filesystem. The second launch into each prefix measured reuse. New-prefix results include Wine initialization. Filesystem and shader caches were left warm.
- **Desktop entry.** Three launches per build opened the generated shortcut with `gio launch` using a prepared prefix.
- **Latest release.** Three launches per build used `latest_release` with the online catalog and the current release already installed. Other cases used a pinned installation UUID.
- **Window and exit.** A monotonic clock measured from CLI or shortcut invocation to the first mapped Minecraft window on the private X display. After requesting a normal close with `WM_DELETE_WINDOW`, the benchmark checked window disappearance and the prefix lock every 20 ms. The unlock time includes Wine shutdown. These timings measure reaching a window, before loading a world.
- **Busy prefix.** Five additional CLI launches per session targeted the active profile. Each measurement includes process startup, rejection and desktop notification dispatch. The benchmark checked the exit status and the reported reason.

The separate library test used 203 profiles sharing one prefix. With the prefix available, the median of 12 `bedrock profile list` calls dropped from 170 ms to 5.4 ms. With the prefix busy, the median of 15 calls dropped from 3.5 ms to 1.7 ms. These measurements include CLI startup and text output.
