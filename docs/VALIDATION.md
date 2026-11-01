# Validation

## Development baseline

Initial validation: 2026-11-01, openSUSE Tumbleweed, Qt 6.11.2, KDE Frameworks 6.30 and Tailscale 1.102.4. The desktop session was Plasma Wayland. Automated GUI tests ran offscreen with a private D-Bus session.

The local Tailscale version, CLI help, structured status and available file targets were inspected before implementation. Official documentation and the matching CLI source were checked. An empty-inbox `file get --wait --conflict=rename` probe successfully waited as a regular user and was stopped after two seconds. It did not send or move files.

## Automated coverage

All four CTest suites passed before initial publication:

| Suite | Coverage |
| --- | --- |
| `core` | Filename validation, safe publication, same-name conflicts, symlink rejection, unavailable destinations, device eligibility, login state, multi-file argument preservation, send permissions, retry, cancellation, receiving, history persistence/clearing, completed-batch recovery, interrupted-batch isolation and receive permissions |
| `gui` | Tray menu entries, close-to-tray behavior, file selection, English interface text, actual D-Bus notification payloads and action callbacks, open-file confirmation and grouped notifications |
| `single-instance` | Second invocation exits while the first instance remains running; graceful termination and restart in a private D-Bus session |
| `dolphin` | KF6 `KFileItemActions` discovers the top-level action for one or multiple regular files and excludes folders |

Tests use fake Tailscale commands and non-sensitive temporary fixtures. Notification file-opening callbacks are intercepted. No test sends files to real devices.

The Dolphin test was also run against the actual user-installed service menu. Its executable permission was verified as 755, and KDE's service cache was refreshed. This is a service-menu integration, not a Purpose plugin in the Share submenu.

The executable, desktop entry, service menu and notification configuration were installed successfully. The application desktop entry passed `desktop-file-validate`; the KDE-specific `Type=Service` entry was validated through KDE's own menu loader. Application help output was checked.

## Desktop and device acceptance

Nuaptan Evalisk has confirmed completion of manual acceptance testing, including real iPhone transfers and KDE Plasma 6 / Wayland desktop use.

This manual validation complements the automated suites above, which use simulated transfers and an offscreen GUI.

## Known boundaries

- Installations requiring root-only retrieval are not supported without a separate authorization design. The app does not elevate privileges or grant broad operator access.
- Interrupted CLI batches can contain both complete and partial files. Unmarked batches are retained for manual verification rather than automatically published.
- Download destinations must support hard links. Staging and publication require temporary additional disk space.
- Stopping TailCourier does not stop tailscaled from accepting files into its own inbox.
- Hard crashes may cause duplicate publication; exactly-once delivery is not promised.

See the [README](../README.md) for installation, permissions, recovery and uninstall instructions.
