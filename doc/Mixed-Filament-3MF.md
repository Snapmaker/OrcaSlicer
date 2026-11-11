# Mixed-filament storage in 3MF

Project saves include `Metadata/mixed_filaments.json` as the authoritative mixed-recipe representation. They also write the existing `mixed_filament_definitions` string inside `Metadata/project_settings.config` for older Snorca versions. Both are generated from the same recipe snapshot.

This is an additive change to the existing project format. Meshes, objects, parts, facet painting, physical filament presets, printer/process settings, and their assignments keep their existing storage. There is no new material catalog, manifest, or global identity map.

## Archive layout

```text
3D/3dmodel.model
3D/Objects/...
Metadata/project_settings.config    # Existing settings, including legacy recipes
Metadata/model_settings.config      # Existing object/part settings
Metadata/mixed_filaments.json       # Authoritative recipes, version 1
Metadata/...                        # Existing thumbnails and slice information
```

Full project saves write the JSON even when there are no recipes: `{"version":1,"recipes":[]}`. Geometry-only exports without project configuration do not add it. Incremental backups retain their existing configuration/restore mechanism; they do not introduce a separate recipe snapshot.

## Recipe format

```json
{
  "version": 1,
  "recipes": [
    {
      "id": "77",
      "distribution": "simple",
      "components": [
        { "filament": 1, "percent": 75 },
        { "filament": 2, "percent": 25 }
      ]
    },
    {
      "id": "78",
      "distribution": "layer_cycle",
      "components": [
        { "filament": 1, "percent": 40 },
        { "filament": 2, "percent": 35 },
        { "filament": 3, "percent": 25 }
      ],
      "gradient": {
        "order": [1, 3, 2],
        "start": 1.0,
        "end": 0.0,
        "stops": [0.0, 0.25, 0.5, 0.75, 1.0],
        "solid_widths": [0.0, 0.03, 0.0]
      },
      "local_z": { "max_sublayers": 4 }
    }
  ]
}
```

- `id` is the existing stable recipe identity, encoded as a nonzero decimal string to preserve all 64 bits. IDs must be unique.
- `filament` is a **one-based physical project filament slot**, never an AMS tray or nozzle ID. Physical settings still live in the existing project configuration arrays.
- `components` contains at least two distinct physical slots. Integer percentages are between 0 and 100 and sum to 100. Zero-weight components remain valid.
- Component order retains the primary A/B pair in its first two entries. Gradient traversal has its own `order`, containing every component once.
- **Recipe array order is significant.** Active recipes receive virtual filament slots after the physical filaments, in this order. Existing object, part, and painted-facet assignments continue to use those slots.
- `distribution` is `simple` or `layer_cycle`. Retired pointillism data is migrated using the existing legacy rules.

Optional fields:

| Field | Meaning and default |
| --- | --- |
| `pattern` | Arrays of physical filament IDs, one per perimeter group; e.g. `[[1,12,2],[2,1,10]]`. Absent means a weighted recipe. Groups must be nonempty. |
| `gradient` | Present for an enabled gradient. `start` and `end` are A-component fractions in `[0,1]`, differing by at least 0.05. |
| `gradient.stops` | Optional ordered curve with `2 * component_count - 1` entries, starting at 0 and ending at 1. Absent uses the existing default curve. |
| `gradient.solid_widths` | Optional full normalized solid-color widths, one per ordered component. Absent inherits the existing process setting. |
| `local_z.max_sublayers` | Nonnegative cap; absent or zero means uncapped. Other Local-Z settings remain in project/process configuration. |
| `surface_bias` | Signed `a_mm` and `b_mm` offsets, each in `[-2,2]`; absent means zero. |
| `state` | `active` (default), `disabled`, or `deleted`. Deleted rows are tombstones, retained to prevent regeneration. Disabled and deleted rows do not receive virtual slots. |
| `origin` | `manual` (default), `generated`, or `customized_generated`. Preserves existing regeneration behavior. |
| `origin_auto` | Rare compatibility override for historical generated rows whose origin flag was false. Normally inferred from `origin` and omitted. |
| `ui_mode` | Existing editor mode: `-1` unknown (default), `0` ratio, `1` cycle, `2` match, `3` gradient. |
| `pair_percent_b` | Independent legacy A/B percentage when it differs from the weighted B percentage. Needed for recipes with inactive multicolor weights. |

Display colors, derived cadence counts, Local-Z plans, slice caches, and hardware AMS assignments are not stored here. Unknown additional fields in version 1 are ignored; changes that require different interpretation must increment `version`.

## Loading and saving

1. Load ordinary project settings.
2. If `mixed_filaments.json` is absent, retain the legacy string and existing old-project loading behavior.
3. If present, validate the JSON and replace the recipe configuration with its contents. ZIP entry order does not affect precedence. An empty recipe array intentionally clears legacy recipes.
4. Invalid JSON, an unsupported version, duplicate recipe IDs or archive entries, invalid weights, or unavailable physical references stop loading with an error identifying the JSON file. There is no silent legacy fallback.
5. On save, build one typed recipe snapshot, validate it, and generate both the JSON and the legacy string. Failure to create either representation fails the save. The existing exporter writes to a temporary archive before replacing the destination, so a failed recipe export does not replace the original project. The caller's configuration is not mutated.

The codec in `src/libslic3r/MixedFilament/Serialization.cpp` reads and writes the existing `MixedFilamentDefinition` types. The current GUI/config pipeline still transports recipes through the compact string: loading JSON converts typed definitions into that transport value, and saving obtains its typed snapshot from it. JSON authority is an **on-disk precedence rule**, not a second mutable in-memory recipe store.

The archive boundary uses `MixedFilamentManager::LoadMode::Exact` when decoding that transport value. It retains stored auto rows without depending on the user's auto-generation preference or appending newly generated pairs. It rejects saves that would discard invalid rows and shift subsequent virtual assignments. Legacy text may normalize on save; recipe meaning and assignment order are the compatibility contract.

## Older Snorca versions

Older readers use the legacy string and do not interpret the new JSON. The checked importer extracts `Metadata/*.json` to temporary storage, but its writer rebuilds the archive from known data rather than preserving arbitrary metadata files.

Consequently, saving in an older version drops `mixed_filaments.json`. A newer version can still open that project through the legacy fallback and recreate the JSON on its next save. This preserves only recipe features that the older version itself understands. No core 3MF version or required-extension change is introduced.

## Image maps: reserved for a later feature port

This branch does **not** implement image mapping, write `image_maps.json`, or create `image_map_ref` metadata. The agreed future format is documented here so it can be introduced with the model and rendering support it needs:

```text
Metadata/image_maps.json
Metadata/image_assets/<asset-id>.rgba8
```

```json
{
  "version": 1,
  "assets": [
    {
      "id": "texture_1",
      "path": "Metadata/image_assets/texture_1.rgba8",
      "width": 512,
      "height": 512
    }
  ],
  "maps": [
    {
      "id": "map_1",
      "mesh_fingerprint": "8249361725501904",
      "zones": [
        {
          "id": "zone_1",
          "name": "Front artwork",
          "settings": { "render_mode": "normal_mix" },
          "palette": [
            { "target_rgba": [1.0, 0.0, 0.0, 1.0], "filament": 1 },
            { "target_rgba": [0.75, 0.0, 0.25, 1.0], "recipe": "77" }
          ]
        }
      ],
      "triangle_bindings": [
        {
          "triangle": 0,
          "zone": "zone_1",
          "source": {
            "kind": "texture",
            "asset": "texture_1",
            "uv": [[0.0, 0.0], [1.0, 0.0], [0.0, 1.0]],
            "wrap_u": "repeat",
            "wrap_v": "repeat"
          }
        }
      ]
    }
  ]
}
```

A palette target has exactly one of a physical `filament` slot or a stable `recipe` ID. Triangle indices are zero-based. Assets contain raw RGBA8 pixels, compressed by the ZIP container. The fingerprint above is illustrative; its algorithm and the complete image settings contract belong to the later port.

The intended link is per owning model part in `model_settings.config`, not keyed solely by a potentially shared mesh ID:

```xml
<part id="1" subtype="normal_part">
  <metadata key="image_map_ref" value="map_1"/>
</part>
```

Older versions would ignore this link and lose image maps and source assets on re-save. The legacy recipe string cannot represent image maps. A future facet-painting fallback could preserve supported printable assignments, but not the original image, UVs, or editable image settings.

## Regression coverage

`tests/libslic3r/test_mixed_filament_serialization.cpp` covers typed/legacy equivalence, grouped multi-digit patterns, gradient settings, unavailable/generated recipes, full-width IDs, validation failures, JSON precedence, empty recipes, legacy-only loading, duplicate archive entries, and production export/import with shared meshes and object/part/paint assignments. It also migrates the existing `CubeLines.3mf` fixture and checks its recipes, object assignments, and every painted facet after reload. The existing product-port persistence test exercises repeated saves through the new archive path.
