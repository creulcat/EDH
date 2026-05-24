# Tow Hitch

Tow Hitch is a telemetry plugin for **Euro Truck Simulator 2** and **American Truck Simulator**. When something happens in-game — a job delivered, a fine paid, a ferry taken — it sends a message to a webhook you configure. The included templates are set up for Discord, but any service that accepts a JSON POST will work.

## Installation

### Download

Download the latest release from [GitHub Releases](https://github.com/creulcat/EDH/releases) as a `.zip` file containing `Towhitch.dll` and example config files.

### Prerequisites

- Euro Truck Simulator 2 or American Truck Simulator (64-bit)
- Windows: [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) (x64) — install if Windows reports a missing runtime when loading the plugin

### Plugin (DLL)

**Windows**

Copy `Towhitch.dll` to:

```
Euro Truck Simulator 2\bin\win_x64\plugins\
```

Create the `plugins` folder if it does not exist. For ATS, use the equivalent path under `American Truck Simulator\`.

**Linux / macOS**

See the [SCS Telemetry SDK readme](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry) for plugin install paths on your platform.

### Config and JSON files

Copy `Towhitch.cfg` and the `.json` payload files to your **game home directory** (the folder that contains `config.cfg`), or to `plugins/` inside that folder.

Paths in `jsonFile` are relative to the folder where `Towhitch.cfg` lives.

Restart the game after changing config — settings are loaded when the plugin starts.

## Functionality

Tow Hitch listens for gameplay events and POSTs a JSON payload to the matching webhook URL. You can configure one or more webhooks per event.

### Supported events

| Event | Also accepted as |
|---|---|
| Job cancelled | `job.cancelled`, `Canceljob` |
| Job delivered | `job.delivered`, `Finishjob` |
| Player fined | `player.fined` |
| Tollgate paid | `player.tollgate.paid` |
| Ferry used | `player.use.ferry` |
| Train used | `player.use.train` |

### Placeholders

Placeholders go in your JSON template files and are replaced when the webhook is sent. If a value is missing, the locale `notAvailable` text is used instead (default: `-N/A-`).

**Available on all events** (once truck/job data is known):

| Placeholder | Description |
|---|---|
| `{truck}` | Truck brand and model |
| `{cargo}` | Cargo name |
| `{startingLocation}` | Job source city |
| `{destination}` | Job destination city |
| `{timestamp}` | Current UTC time (ISO 8601) |

**Job cancelled**

| Placeholder | Description |
|---|---|
| `{penalty}` | Cancellation penalty (formatted currency) |

**Job delivered**

| Placeholder | Description |
|---|---|
| `{revenue}` | Delivery revenue (formatted currency) |
| `{earnedXp}` | XP earned |
| `{cargoDamage}` | Cargo damage (e.g. `2%`) |
| `{distanceDriven}` | Distance driven in km |
| `{deliveryTime}` | Time spent on the job (in-game duration) |
| `{autoparkUsed}` | Auto-park used (`true` / `false`) |
| `{autoloadUsed}` | Auto-load used (`true` / `false`) |

**Player fined**

| Placeholder | Description |
|---|---|
| `{offence}` | Offence type |
| `{fineAmount}` | Fine amount (formatted currency) |

**Tollgate paid**

| Placeholder | Description |
|---|---|
| `{tollAmount}` | Toll amount (formatted currency) |

**Ferry / train used**

| Placeholder | Description |
|---|---|
| `{useAmount}` | Fare amount (formatted currency) |
| `{useStartingLocation}` | Departure location |
| `{useDestination}` | Arrival location |

Currency symbols are taken from your active profile's in-game currency setting.

### Locale

An optional `locale:` block controls translated text for durations (day/hour/minute labels) and the fallback shown for empty values.

Built-in presets: `en`, `nl`, `de`, `fr`. You can also override individual keys on top of a preset.

```yaml
locale:
    name: "nl"
    notAvailable: "-n.v.t.-"
```

## Configuration

Edit `Towhitch.cfg` to set your webhook URLs and choose which events to send. Each `event:` block maps one in-game event to one webhook.

Example:

```yaml
locale:
    name: "en"

event:
    name: "JobDelivered"
    webhookURL: "https://discord.com/api/webhooks/YOUR_ID/YOUR_TOKEN"
    jsonFile: "JobDelivered.json"
```

You can add multiple `event:` blocks for the same event type if you want to notify more than one webhook.

Example JSON templates for Discord embeds are in the [`resources/`](resources/) folder:

- `JobCancelled.json`
- `JobDelivered.json`
- `PlayerFined.json`
- `PlayerTollgatePaid.json`
- `PlayerUseFerry.json`
- `PlayerUseTrain.json`

A full commented example config is in [`resources/Towhitch.cfg`](resources/Towhitch.cfg).

## Discord & webhooks

### Discord

1. In your Discord server, go to **Server Settings → Integrations → Webhooks**.
2. Create a webhook and copy its URL.
3. Paste the URL into the `webhookURL` field in `Towhitch.cfg` for the events you want.

The sample JSON files use [Discord's webhook embed format](https://discord.com/developers/docs/resources/webhook). Customize the title, fields, and colors to taste.

Keep your webhook URL private — anyone with the URL can post to that channel.

### Other services

Tow Hitch sends a `POST` request with `Content-Type: application/json`. Any endpoint that accepts JSON in the request body can be used; shape the payload in your `.json` file to match what your service expects.

## Building from source

If you prefer to build the plugin yourself:

**Requirements**

- CMake 3.16 or newer
- A C++17 compiler
- Windows: Visual Studio 2019 or newer (or Ninja + MSVC)
- Linux / macOS: libcurl development libraries

**Build**

Windows:

```powershell
.\scripts\build.ps1
```

Linux / macOS:

```bash
./scripts/build.sh
```

The built plugin is copied to `dist/` (e.g. `dist/Towhitch.dll`).

To build from source you also need the SCS Telemetry SDK unpacked under `tools/scs_sdk/`. Download it from the [SCS modding wiki](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry).
