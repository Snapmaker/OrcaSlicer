# SnapLog: what Snapmaker Orca uploads to the Snapmaker cloud

SnapLog is the usage-event and diagnostic-log upload that Snapmaker introduced with the official
Snapmaker Orca 2.4 (upstream commit `2d1554e660`). This build carries the same client, byte for
byte, and wraps it in rules of its own (`src/slic3r/GUI/SnapLogWiring.cpp`). This page lists what
leaves the machine, under which conditions, and how to switch it off.

## When anything is sent

All three conditions must hold:

1. The build contains SnapLog (`SLIC3R_SNAPLOG=ON`; the Flatpak default is `OFF`).
2. **Join Customer Experience Improvement Program** is ticked (setup guide or Preferences > Online >
   User Experience). A new profile starts with the box unticked.
3. **Upload usage events and diagnostic logs to Snapmaker** is ticked (same place, ticked by default,
   greyed out while the programme is not joined).

On top of that the client drops every event while no Snapmaker account is signed in during the
current session. The sign-in is not remembered across restarts, so a session without a sign-in
uploads exactly one event: `flutter_run_result`, which tells whether the built-in web interface
started. It is sent without an account, signed with the public gateway key.

## How to switch it off

- Untick either of the two boxes. The queues are emptied and the spool directory
  `<data dir>/log_upload_spool` is purged within seconds. No restart is needed.
- Build with `-DSLIC3R_SNAPLOG=OFF`: no thread, no file, no traffic, and the second box is absent.

## Where it goes

`https://api.snapmaker.com`, or `https://api.snapmaker.cn` when the region is China; paths below
`/api/log/`. Batch files are uploaded with a pre-signed `PUT` to the storage URL the gateway
returns. Anything that is not `https://` is refused by this build.

## Events

Every event carries: client type `Orca`, the per-install client id, the process id, the application
version, the operating system, a batch id, the local time, and, once known, the account id, the
MQTT client id and the hashed printer serial number.

| Event | When | Extra fields |
|---|---|---|
| `flutter_run_result` | once per start: the web interface spoke within 120 s, or did not | `success` |
| `user_login_result` | a sign-in finished | `success`, `userId` or `httpStatus` |
| `user_logout` | sign-out | - |
| `slice_completed` | a slice finished without error and was not cancelled | - |
| `slice_failed` | a slice ended with an error | `reason`: first line of the error message, home directory replaced by `~`, at most 512 bytes |
| `device_connect_attempt` | a LAN printer connection starts | `use_ssl` |
| `device_discovery_start` | the printer search starts | - |
| `mqtt_client_create`, `mqtt_client_create_failed` | the web interface opens a printer connection | `connectSessionId`, `transport`, `useTls`, `server` or `reason` |
| `mqtt_connect_attempt`, `mqtt_connect_result`, `mqtt_connect_failure` | connection steps | `connectSessionId`, `success`, `reason` |
| `mqtt_subscribe_result` | a topic subscription finished | `connectSessionId`, `topic`, `qos`, `success`, `reason` |
| `device_engine_set` | the printer is bound to the connection | `connectSessionId`, `printerSN` (hashed), `clientId`, `linkMode` |
| `device_disconnect` | the connection was closed | `connectSessionId` |
| events of the web interface | the built-in Flutter pages report through the `sw_SnapLog` command | `level`, `message`, free-form fields, `source=flutter` |

`connectSessionId` is a random UUID per connection attempt. It ties the steps of one attempt
together and is forgotten on disconnect and when the web view is closed.

## Readable and hashed identifiers

Sent in readable form:

- the Snapmaker account id (`userId`);
- the MQTT client id (`connect_clientid`, `clientId`);
- the printer's network address (`server`) and the MQTT topics (`topic`);
- object and file names that are part of the first line of a slicing error message.

Sent as one-way hashes (first 16 hex digits of an HMAC-SHA256 keyed with the client id):

- printer serial numbers;
- the id of the selected device. The official build sends this one in readable form.

Removed before sending: values of keys that look like secrets (`***`), fields whose name starts
with `raw`, a path at the start of a value, and the home directory inside a slicing error message.

## Differences to the official Snapmaker Orca 2.4

All of them reduce what is sent or how often the gateway is contacted:

- the device id is hashed, the slicing error reason is shortened and scrubbed;
- `device_connect_attempt` has no `connectionType` field;
- a switch of its own next to the programme box;
- requests to anything but `https://` are refused; signed anonymous requests are refused when the
  build has no signing key;
- batch upload attempts are paced (8 at once, then one per 15 s), because the client has no
  back-off when the gateway is unreachable or rejects the build;
- the `sw_SnapLog` command limits what a page can queue (64 KB per text, 64 fields).

## Build configurations

| Configuration | Behaviour |
|---|---|
| default | as described above |
| `-DSNAP_LOG_HMAC_SECRET=` (empty) | no anonymous upload at all: `flutter_run_result` needs a sign-in like every other event, and nothing is ever signed with an empty key. Uploads of signed-in users keep working |
| `-DSLIC3R_SNAPLOG=OFF` | compiled and linked, never started; the Preferences box is not created; the `sw_SnapLog` command still answers the web interface and only writes its optional `log` text to the local log |

## On disk

`<data dir>/log_upload_spool/` (`active.log`, `batch.*.sealed`, `.lock`), at most 256 MB, removed
when the consent is withdrawn. It is not part of the log bundle that **Help > Export Logs** and the
troubleshooting dialog create; those pack `<data dir>/log` only.
