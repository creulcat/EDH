# EDH (Euro Truck Log)

EDH is a **telemetry plugin** for **Euro Truck Simulator 2** and **American Truck Simulator**. When you **complete a delivery** (job delivered gameplay event), it sends a **Discord webhook** request with a rich embed: player name, origin and destination cities, distance, truck, cargo, and job reward. The in-game name used in materials is **Euro Truck Log** (see embedded embed templates).

The plugin uses the **SCS Telemetry SDK** (API version 1.01), listens for **configuration** updates (current job and truck) and **gameplay** events, and posts **HTTPS JSON** to Discord via **WinHTTP** on Windows.

## What it does

- Subscribes to telemetry **configuration** for the active **job** (source/destination city, cargo) and **truck** (brand and model).
- On **`job_delivered`**, builds a JSON body from an embedded Discord embed template, fills placeholders (see below), and **POST**s it to the URL in `discord.webhook`.
- Writes short status lines to **`EDH_webhook.log`** in your game profile folder (success/failure of the webhook call — not full JSON or continuous gameplay spam).
- Optionally creates **`edh_webhook.cfg`** the first time, using the **embedded default** from the build, if the file does not exist yet.

## Installing in the game

1. Obtain **`edh_plugin.dll`** (build from this repository or use a provided release).
2. Copy the DLL into the game’s **64-bit plugins** directory:
   - **ETS2:** `...\Euro Truck Simulator 2\bin\win_x64\plugins\`
   - **ATS:** `...\American Truck Simulator\bin\win_x64\plugins\`
3. Start the game. The plugin loads with the telemetry API; check the in-game log or `EDH_webhook.log` to confirm it initialized.

Details on plugin paths and developer commands (for example SDK reload) are in `tools/scs_sdk/readme.txt` once you have the SDK unpacked locally.

## Configuration

### Where the config lives

Settings are read from **`edh_webhook.cfg`** in your **Documents** game profile folder:

- **ETS2:** `%USERPROFILE%\Documents\Euro Truck Simulator 2\edh_webhook.cfg`
- **ATS:** `%USERPROFILE%\Documents\American Truck Simulator\edh_webhook.cfg`

If **`edh_webhook.cfg` is missing** when the plugin starts, it is **created** in that folder from the **embedded default** (`resources/default.cfg` at build time). Edit the file in a text editor; lines starting with `#` are comments.

### Config keys

| Key | Meaning |
|-----|---------|
| `language` | Chooses the embedded Discord template: **`EN`**, **`NL`**, or any other value (falls back to the generic `embed.json` template). Matching is case-insensitive for `EN` and `NL`. |
| `discord.webhook` | Full **HTTPS** URL of your [Discord incoming webhook](https://support.discord.com/hc/en-us/articles/228383668-Intro-to-Webhooks). If empty, deliveries are still detected but nothing is sent (a line is written to the log). |
| `discord.embed.color` | Value substituted into the embed JSON as `{cfg.color}` (default `000000`). Must match what your embed JSON expects (the shipped templates use this as a string placeholder in JSON). |
| `discord.embed.playername` | Display name substituted as `{cfg.player}`. |

Values can be quoted, for example `language = "EN"`.

### Discord webhook

In Discord: channel **Settings → Integrations → Webhooks → New Webhook**, then copy the webhook URL into `discord.webhook`. Anyone with this URL can post to the channel, so treat it like a secret.

### Embed templates and languages

At **build time**, these files are embedded into the DLL (`cmake/edh_gen_embedded.cmake`):

- `resources/default.cfg` — default `edh_webhook.cfg` when created on disk
- `resources/embed.json` — fallback template when `language` is not `EN` or `NL`
- `resources/embed_EN.json` — used when `language` is **EN**
- `resources/embed_NL.json` — used when `language` is **NL** (Dutch field labels such as *Speler*, *Startpunt*, *Eindpunt*, etc.)

To change the layout or text of Discord messages, edit those JSON files and **rebuild** the plugin.

### Placeholders in the embed JSON

The plugin performs simple string replacement on the embedded template before sending:

| Placeholder | Source |
|-------------|--------|
| `{cfg.color}` | `discord.embed.color` |
| `{cfg.player}` | `discord.embed.playername` |
| `{timestamp}` | Current time (UTC, ISO-8601 style) |
| `{position_start}` | Job source city from telemetry |
| `{position_end}` | Job destination city from telemetry |
| `{distance}` | Delivered distance, formatted using your game UI preferences (see below) |
| `{truck}` | Truck brand and model from telemetry |
| `{cargo}` | Cargo name from telemetry |
| `{gains}` | Job revenue, with currency prefix from your game settings (see below) |

### Distance and currency (aligned with the game)

The plugin reads **`config.cfg`** in the same **Documents** profile folder as the game. It uses:

- **`uset g_mph`** — if you use **miles** in the UI, distance is shown in **mi**; otherwise **km**.
- **`uset g_currency`** — maps the game’s currency index to a **prefix** for `{gains}` (e.g. € for ETS2 defaults, `$` for ATS when unset), consistent with the game’s own mapping in the plugin source.

If something is missing from telemetry, the template may show `?` for that field.

## Logs and troubleshooting

| Location | What you see |
|----------|----------------|
| **`EDH_webhook.log`** | Same profile folder as `edh_webhook.cfg`. Timestamped lines: plugin load, config load, webhook send success/failure. |
| **`game.log.txt`** | Under the game’s Documents folder; SCS log callback messages from the plugin. |
| **DebugView** (Windows) | With global Win32 capture, `OutputDebugString` lines from the plugin. |

If you change `edh_webhook.cfg`, restart the game (or use SDK reload/reinit **only if** telemetry plugins are loaded — see SCS SDK readme) so settings are read again.

## Repository layout and ignored paths

The following paths are in **`.gitignore`** and are not committed:

- **`build/`** — local CMake build tree.
- **`tools/`** — place the **SCS SDK** here as `tools/scs_sdk` with headers under `tools/scs_sdk/include`.

## Building from source

### Prerequisites

- **CMake** 3.16 or newer  
- **C++11** toolchain on **Windows** (the plugin uses **WinHTTP**)  
- **SCS Telemetry SDK** extracted so that `tools/scs_sdk/include` exists (e.g. contains `scssdk.h`)

### Commands

From the repository root:

```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

With Visual Studio–style generators, the DLL is typically:

`build/Release/edh_plugin.dll`

Optional install (install prefix as usual for CMake):

```powershell
cmake --install . --config Release
```

Copy **`edh_plugin.dll`** into the game’s `bin\win_x64\plugins\` folder as described above.
