# TailCourier

**Native Taildrop file transfers for KDE Plasma 6.**

TailCourier adds a tray app, desktop notifications and Dolphin integration to an existing Tailscale installation. Connections, encryption and file transport are handled by Tailscale—even when your devices are on different networks. No extra server, API key or telemetry.

Built with C++20, Qt 6 Widgets and KDE Frameworks 6. Linux / Plasma 6 only, with Wayland as the primary desktop target. The interface is in English.

> Early release: automated tests pass, but real iPhone transfers and full Plasma/Wayland desktop acceptance are still pending. See [validation and limitations](docs/VALIDATION.md).

## Features

- Receive files into your system Downloads folder under `TailCourier`, with a configurable destination.
- Send one or multiple files from the app, drag and drop, or Dolphin.
- See device names, operating systems, online status and Tailscale's Taildrop eligibility.
- Stay in the system tray when the window closes.
- Get grouped KDE notifications with **Open File** and **Open Containing Folder** actions.
- Retry failed sends, cancel sending, pause automatic receiving and optionally start at login.
- Keep a local history of up to 500 transfers, which you can clear.
- Preserve existing files when names collide. Never automatically open received files.

Folders and symbolic links are not supported. TailCourier does not create archives or recursively send directories.

## Requirements

Runtime:

- KDE Plasma 6 and the Qt Wayland platform plugin.
- Tailscale installed, running and signed in.
- Permission for your regular user to run Tailscale file commands.

Build:

- CMake 3.22 or newer and a C++20 compiler.
- Qt 6.6 or newer: Core, Widgets and Network.
- KDE Frameworks 6: StatusNotifierItem, Notifications and DBusAddons.
- For tests: Qt Test and DBus, KF6 KIO development files, Python 3 and `dbus-run-session`.

On openSUSE, the relevant development packages include:

```text
qt6-base-devel
kf6-kstatusnotifieritem-devel
kf6-knotifications-devel
kf6-kdbusaddons-devel
kf6-kio-devel
```

Other distributions use different package names. The development baseline was Tailscale 1.102.4, Qt 6.11.2 and KF6 6.30 on openSUSE Tumbleweed.

## Build and install

Install the executable into **~/bin**, with desktop integration under **~/.local/share**:

```sh
git clone https://github.com/NuaptanEvalisk/TailCourier.git
cd TailCourier

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DCMAKE_INSTALL_BINDIR="$HOME/bin"
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build
kbuildsycoca6
```

Ensure `~/bin` is in your Plasma session's `PATH`. Launch **TailCourier** from the application launcher or run:

```sh
~/bin/tailcourier
```

For a conventional installation into `~/.local/bin`, use `-DCMAKE_INSTALL_BINDIR=bin` instead. Set `-DBUILD_TESTING=OFF` if you do not want to build the tests.

### Dolphin integration

Right-click one or more local files and choose **Send with TailCourier**, then select a device and click **Send to Selected Device**.

The action appears at the **top level of the context menu**, not inside **Share**. It is a Dolphin service menu, not a Purpose sharing plugin. Building alone does not install it.

If the action is missing:

1. Run `cmake --install build`.
2. Check that `~/.local/share/kio/servicemenus/tailcourier-send.desktop` exists and is executable.
3. Check **Settings → Configure Dolphin → Context Menu**.
4. Open a new Dolphin window and right-click a regular file.

Selecting a folder does not offer this action. The app also rejects folders dropped into its window.

## Usage

**iPhone → Linux:** in the iOS Files app, share a non-sensitive file through Tailscale and select your Linux computer. While TailCourier is running and receiving is enabled, it retrieves files from Tailscale's inbox.

**Linux → iPhone:** choose files in TailCourier or Dolphin, select your iPhone and send. Device eligibility follows Tailscale's current reported status; membership in the same tailnet alone is not enough.

**Progress:** the app shows the current file number and an indeterminate progress indicator. It does not invent percentages from terminal animations.

**Cancel:** stops the current send process and the remaining queue. The other device may already have partial or complete files; cancellation is not a remote rollback. Retry includes failed or cancelled files and files that were not sent. iOS reception does not guarantee resumable transfers, so retrying may produce duplicates.

**Pause / quit:** stops retrieving files from Tailscale's inbox. Pausing leaves outgoing transfers running, and files already retrieved may finish saving. Quitting cancels sending and local publication. Neither action stops `tailscaled`, which can still accept files into its own inbox.

**Autostart:** disabled by default. Enable or disable it in Settings.

## Permissions

The GUI must run as a regular user. It refuses to run as root.

TailCourier invokes only the public Tailscale CLI: version/help checks, `status --json`, `file cp` and `file get`. It does not change login state, ACLs, routes, exit nodes, operator settings or other VPN configuration. It does not request a password, API key or login token.

Some installations deny file retrieval to ordinary users. TailCourier reports the underlying error and backs off. The administrator must review that installation's local access policy.

Tailscale's `--operator` setting grants broad control over the local daemon; it is **not Taildrop-only authorization**. TailCourier does not configure it, add passwordless sudo rules or ship a privileged helper. Deployments requiring a new, narrowly scoped privileged service are not supported by this release.

## Safety and recovery

- Commands use `QProcess` argument arrays, never shell interpolation. Absolute file paths preserve spaces, Unicode, quotes and leading hyphens.
- Each incoming batch first goes into a private staging directory using `file get --wait --conflict=rename`.
- Only batches that finish successfully are automatically published to the download folder.
- Destination path components are opened without following symbolic links. Unsafe incoming names and non-regular staged files are rejected.
- Files are copied to private temporary files and atomically linked under an unused final name. Existing files and symbolic links are never overwritten.
- The destination filesystem must support hard links, such as ext4 or Btrfs. FAT-style destinations are not supported. Staging and publication temporarily need space for two copies.
- Local publication runs on a worker thread. Failures retain the staged source and use retry backoff, up to five minutes.
- Notifications are grouped in four-second windows. Connectivity failures update status without repeated popup errors.
- Single-instance D-Bus activation forwards Dolphin selections to the running app. A runtime lock prevents duplicate receivers.
- No file contents or credentials are stored in history, and no telemetry is added.

### Interrupted batches

The CLI can fail partway through a batch after deleting successfully retrieved files from the daemon inbox. It does not provide a reliable structured per-file completion list.

For that reason, **unmarked interrupted batches remain in staging and are not automatically treated as complete**. Open the staging folder from Settings, verify files against the sender's sizes or checksums, and recover them manually or request a resend. Do not delete staging data without checking it.

A hard crash between final publication and staging cleanup can leave a safe duplicate after restart. This release does not promise exactly-once delivery or a power-loss transaction.

## Local data

Default locations, subject to XDG settings:

| Data | Location |
| --- | --- |
| Downloads | System Downloads folder / `TailCourier` |
| Settings | `~/.config/TailCourier/tailcourier.conf` |
| History | `~/.local/share/TailCourier/tailcourier/history.json` |
| Staging | `~/.local/share/TailCourier/tailcourier/inbox` |
| Optional autostart entry | `~/.config/autostart/org.kde.tailcourier.desktop` |

## Testing

```sh
ctest --test-dir build --output-on-failure
```

Tests use a fake Tailscale CLI and a private D-Bus notification service. They do not send files to real devices. Coverage includes safe publication and name collisions, eligibility, permissions, multi-file arguments, retry/cancellation, recovery, notifications, single-instance startup and Dolphin menu discovery.

GUI tests run offscreen. They cannot replace real Plasma panel, Wayland activation, theme, scaling or device-transfer checks. See [the validation report](docs/VALIDATION.md).

## Uninstall

Quit TailCourier from its tray menu first. For the installation layout above:

```sh
rm -f ~/bin/tailcourier
rm -f ~/.local/share/applications/org.kde.tailcourier.desktop
rm -f ~/.local/share/kio/servicemenus/tailcourier-send.desktop
rm -f ~/.local/share/knotifications6/tailcourier.notifyrc
rm -f ~/.config/autostart/org.kde.tailcourier.desktop
kbuildsycoca6
```

For other prefixes, consult `build/install_manifest.txt`. Downloaded files are not removed. Settings and application data may be deleted separately, but **check staging for recoverable files first**. No Tailscale configuration needs to be reverted.

## Interface references

- [Official Taildrop documentation](https://tailscale.com/docs/features/taildrop)
- [Official Tailscale CLI reference](https://tailscale.com/docs/reference/tailscale-cli)
- [Tailscale v1.102.4 CLI implementation](https://github.com/tailscale/tailscale/blob/v1.102.4/cmd/tailscale/cli/file.go)
- [Tailscale v1.102.4 status definitions](https://github.com/tailscale/tailscale/blob/v1.102.4/ipn/ipnstate/ipnstate.go)
- [KDE Dolphin service menus](https://develop.kde.org/docs/apps/dolphin/service-menus/)

The implementation uses public CLI commands. The source references were used to check their behavior, not to depend on private APIs. TailCourier is an independent project and is not affiliated with Tailscale or KDE.

## License

Copyright (C) 2026 TailCourier contributors.

Licensed under the **GNU General Public License, version 3 only** (`GPL-3.0-only`). See [LICENSE](LICENSE) for the full terms. This software is provided without warranty.
