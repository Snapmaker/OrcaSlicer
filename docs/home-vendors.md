# Home tab: Vendors connectors

The Home tab's Vendors section lists the models a vendor's REST API gives you access to. EdgeSlicer
ships no vendors. Each connector is a small JSON spec that says where the list is, how it is paged,
how its fields map and how a file is downloaded. You create one with **Add connector** (a form, or
**Edit as JSON**), or import a spec someone shared with **Import**.

The engine is `src/slic3r/Utils/VendorConnector.*`, with tests in
`tests/slic3rutils/vendor_connector_tests.cpp`. The slicer side is `src/slic3r/GUI/HomeVendors.*`.

## Credentials

A spec never holds a secret. It names the slots it needs: the auth key, a basic-auth user name, or a
header marked `"secret": true`. You set each one in the app with **Set** on the connector card.

- Values are kept in the system's credential store: Credential Manager on Windows, Keychain on
  macOS, or the Secret Service/libsecret on Linux. Where there is none, they are kept in memory
  for the session only, and the card says so.
- They are sent only in request headers or the query string, and only to the connector's own
  host.
- They never appear in `app_config`, the cache, logs, error messages, exported specs or CSV
  files.

Remove a connector's stored credentials with **Forget credentials**.

## Spec format

```json
{
  "format": "edgeslicer-vendor-connector", "version": 1,
  "name": "Shown on the card", "vendor": "Vendor tag (same as the Library's)",
  "license": "Your license for this vendor's models, used when a model gives none",
  "base_url": "https://api.vendor.example/v1",
  "auth": {"type": "none | bearer | header | query | basic", "name": "header or query name"},
  "headers": [{"name": "X-Fixed", "value": "..."}, {"name": "X-Secret", "secret": true}],
  "list": {
    "path": "/models", "items": "path.to.array", "query": {"sort": "name"},
    "paging": {"type": "none | page | offset | cursor | next", "param": "page", "start": 1,
               "size_param": "limit", "size": 100, "has_more": "has_next", "total": "total",
               "cursor": "next_cursor"},
    "since": {"param": "updated_since", "field": "updated_at"}
  },
  "fields": {"id": "id", "name": "name", "thumbnail": "images.0.url",
             "page_url": "https://vendor.example/models/{slug}", "designer": "creator.name",
             "license": "license.name", "tags": "tags", "updated": "updated_at", "description": "summary"},
  "files": {"path": "files", "fields": {"id": "id", "name": "name", "variant": "variant",
            "size": "bytes", "plates": "plates", "print_time": "seconds",
            "colours": "colours", "colour": "hex"}},
  "download": {"path": "/models/{id}/download?file={sub.id}", "url_field": "url",
               "direct_field": "", "limited": false},
  "quota": {"used": "X-Calls-Used", "limit": "X-Calls-Limit", "resets": "X-Calls-Reset"}
}
```

- **Addresses.** `base_url` must be `https://`. Plain `http://` is accepted only for `localhost`,
  for testing. List and download paths start with `/`.
- **Field paths.** Fields are dotted paths into each item, and `0` indexes an array. `page_url`
  may instead be a template on the item's fields, such as `{slug}`.
- **Paging.**
  - `page` and `offset` stop at `has_more`, at `total`, or on a short page.
  - `cursor` sends the token found at `cursor` back as `param`.
  - `next` follows a full URL, but only on the API's own host.
- **Incremental sync.** When `since` is set, **Refresh list** asks only for models changed after
  the newest `field` value seen so far. **Fetch everything again** does a full sync, which also
  drops models that are gone.
- **Downloads.** `download.path` is an endpoint whose JSON answer has the file URL at
  `url_field`. Alternatively, `direct_field` names a field of the file (or item) that holds its
  URL. Set `limited` when the vendor counts downloads; the app then asks before each one.
- **License.** A model's `license` field, when mapped, is shown and filtered on. Otherwise the
  connector's own `license` is used, such as "Commercial" when your membership lets you sell
  prints. It is also a column in the CSV export.
- **Quota.** `quota` names the response headers that report API usage. The connector card shows
  them, and a sync stops when the quota is spent.

## Example: CPL3D

An example for [CPL3D's API](https://www.cpl3d.com/developers). EdgeSlicer does not include it.
Save it as a `.json` file and use **Import**, then set the API key (`pk_...`) and the X-App-Key
on the connector card. Change `license` to match your membership (for example "Commercial"),
since CPL3D's API does not report it per model. CPL3D counts API calls (100 per 30 days for members), so the list is
fetched only when you press **Refresh list**, 100 models per request, and later refreshes ask
only for changes.

```json
{
  "format": "edgeslicer-vendor-connector", "version": 1,
  "name": "CPL3D", "vendor": "CPL3D", "license": "Personal use only",
  "base_url": "https://www.cpl3d.com/api/v1",
  "auth": {"type": "bearer"},
  "headers": [{"name": "X-App-Key", "secret": true}],
  "list": {
    "path": "/library", "items": "models",
    "paging": {"type": "page", "param": "page", "start": 1, "size_param": "limit", "size": 100,
               "has_more": "has_next", "total": "total"},
    "since": {"param": "updated_since", "field": "files_updated_at"}
  },
  "fields": {"id": "id", "name": "name", "thumbnail": "thumbnail",
             "page_url": "https://www.cpl3d.com/models/{slug}", "tags": "type",
             "updated": "files_updated_at"},
  "files": {"path": "print_profiles", "fields": {"id": "id", "name": "name", "variant": "variant",
            "size": "fileSize", "plates": "numberOfPlates", "print_time": "estimatedPrintTime",
            "colours": "filamentColors", "colour": "hex"}},
  "download": {"path": "/models/{id}/download?profile={sub.id}", "url_field": "download_url",
               "limited": true},
  "quota": {"used": "X-Api-Calls-Used", "limit": "X-Api-Calls-Limit",
            "resets": "X-Api-Period-Resets"}
}
```

CPL3D's download links are single-use, expire after 5 minutes and are tied to your IP address.
The app resolves each link right before it saves the file.
