# as_status (Plugify)

Player list command for the admin_system plugin, ported from the Pisex Metamod module '[AS] Status'. Prints 'ID. Name - SteamID64' for every non-bot slot to the console command 'mm_status' (server console always allowed, a player needs '@admin/status') and to chat via the chat command '!status' (needs '@admin/status'). No phrases: the original printed literal English text. No exported methods.

Plugify port (fork) of **Status ([AS] Status)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.0
- Plugify dependencies: s2sdk (>=2.18.2), admin_system (>=1.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `as_status-1.2.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_status`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

