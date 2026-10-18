# AudioScope Plugin

EuroScope plugin for the **AudioScope** app (TrackAudio Fluganzeige). It connects EuroScope to the app through a local WebSocket server. The app shows the aircraft you click in EuroScope, and you can issue clearances and set the ground status from the app.

Current version: **0.26**

## Features

- Click an aircraft in EuroScope and it appears in the app. Tap an aircraft in the app and it is selected in EuroScope.
- Sends flight plan data to the app: departure, destination, SID, route, remarks, squawk and cleared altitude (CFL).
- Set or remove the clearance flag from the app.
- Set the ground status from the app, using the same states as the Ground Radar plugin: Pending, On Freq, Startup, Push, Taxi, Line Up and Departure. Changes made in EuroScope are pushed back to the app immediately.
- Reports the active airports and the number of open AudioScope radar screens to the app.
- Reports its own version to the app, so the app can tell you when a newer plugin version is available.

## Installation

1. Download `AudioScope.dll` from the [latest release](../../releases/latest) and start EuroScope. When updating, replace the old `AudioScope.dll`.
2. Open **OTHER SET → Plug-ins ...**, click **Load** and select `AudioScope.dll`. EuroScope should report: `Plugin v0.26 geladen, WebSocket auf Port 49081`.
3. Select **AudioScope Plugin** in the list. Move **Ground Radar display** from "Forbidden to draw on types" to "Allowed to draw on types" (`<<`).
4. Then move **Standard ES radar screen** the same way. Both entries should now be under "Allowed to draw on types", and "Forbidden to draw on types" should be empty.
5. Select **AudioScope Plugin** and click **Up** until it is at the very top of the plugin list. Close the dialog with **Close**.
6. In the app, enter the IP address of the PC running EuroScope. The plugin uses TCP port **49081**.

A step-by-step guide with screenshots is available in the app (Settings) and on the update page linked in the app.

The plugin needs an open AudioScope radar screen to trigger EuroScope functions such as the clearance flag. Open one with **Open → New display → AudioScope**. This empty screen is only an anchor for the plugin. Save your display settings in EuroScope and it opens again at the next start. Without an open AudioScope radar screen the plugin still works for selection and flight plan data, but it cannot set the clearance flag.

## Network

- The iPad/iPhone and the PC must be on the same network.
- Allow inbound TCP port 49081 in the Windows firewall.
- On iOS, allow "Local Network" access for the app.

## Protocol

The plugin runs a WebSocket server on port 49081. All messages are JSON text frames. Opening `http://<host>:49081/` in a browser returns the current selection as JSON, which is handy for a quick connection test.

**Plugin → app**

| Message | Meaning |
| --- | --- |
| `{"callsign":"DLH2LY","seq":3}` | Currently selected aircraft. `seq` increases on every change. |
| `{"type":"status","screens":1}` | Number of open AudioScope radar screens. |
| `{"type":"version","version":"0.26"}` | Plugin version, sent right after the connection is established. Plugins older than 0.26 do not send it. |
| `{"type":"airports","list":"EDDH,EDHI"}` | Active airports in EuroScope. |
| `{"type":"info","callsign":"DLH2LY", ...}` | Flight plan data: `dep`, `arr`, `sid`, `route`, `remarks`, `squawk`, `clearedAlt`, `finalAlt`, `clearance`, `gs`, `gsKey`, `airborne`, `speed`. |

**App → plugin**

| Message | Meaning |
| --- | --- |
| `{"select":"DLH2LY"}` | Select the aircraft in EuroScope. |
| `{"info":"DLH2LY"}` | Request flight plan data. |
| `{"airports":"1"}` | Request the active airports. |
| `{"cmd":"clearance","callsign":"DLH2LY","arg":"on"}` | Set (`on`) or remove (`off`) the clearance flag. |
| `{"cmd":"gs","callsign":"DLH2LY","arg":"push"}` | Set the ground status: `pending`, `onfreq`, `startup`, `push`, `taxi`, `lineup` or `depa`. |

The ground status keys map to these EuroScope values: `pending` = NSTS, `onfreq` = ONFREQ, `startup` = STUP, `push` = PUSH, `taxi` = TAXI, `lineup` = LINEUP, `depa` = DEPA.

Callsigns and arguments are restricted to letters, digits, `_` and `-` (max. 16 characters).

## Debug command

Select an aircraft in EuroScope and type the following in the command line:

```
.asgs TEXT
```

This writes `TEXT` to the aircraft's scratchpad and reports whether EuroScope derived a new ground status from it. If nothing changed, the previous scratchpad content is restored. It is meant for trying out ground status candidates without recompiling.

## Building

- Visual Studio with the C++ desktop workload (EuroScope is a 32-bit application, so build for **Win32**).
- The EuroScope plugin SDK (`EuroScopePlugIn.h` and `EuroScopePlugInDll.lib`). It is **not** included in this repository.
- `ws2_32.lib` and `bcrypt.lib` are linked automatically through `#pragma comment`.

Compile-time switches at the top of the source file:

| Macro | Purpose |
| --- | --- |
| `AS_VERSION` | Plugin version string. It is also reported to the app. |
| `AS_VERBOSE` | `1` = detailed diagnostic log in EuroScope, `0` = only important messages. |
| `AS_GS_VIA_SCRATCHPAD` | `1` = set the ground status through the scratchpad (works without a radar screen). |
| `AS_GS_FUNCTION` | Built-in EuroScope tag function used when the scratchpad route is off. |

## Security

The server listens on all network interfaces and has **no authentication**. Anyone on the same network can read the selected aircraft and send commands. Only use it in a trusted local network.

## Disclaimer

This is not an official EuroScope or VATSIM product. For simulation use only.

## License

Released under the GNU General Public License v3.0. See [LICENSE.txt](LICENSE.txt).
