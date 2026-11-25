# G-Code Output

These settings control how G-code is generated and formatted. They impact readability, file size, print behavior, and compatibility with firmware and post-processing tools.

- [Reduce Infill Retraction](#reduce-infill-retraction)
- [Add line number](#add-line-number)
- [Verbose G-code](#verbose-g-code)
- [Label Objects](#label-objects)
- [Exclude Objects](#exclude-objects)
- [Filename Format](#filename-format)
- [Snapmaker 2.0 thumbnails](#snapmaker-20-thumbnails)

## Reduce Infill Retraction

When enabled, the slicer will skip retractions for travel moves that occur entirely inside infill regions. This reduces the number of retractions and can speed up printing for complex models, but it may increase oozing or stringing inside infill. Slicing time may also increase slightly.

**Recommended** when internal cosmetic quality is not critical and you want fewer retractions.

## Add line number

Prefix each G-code line with a sequential line number (N1, N2, ...). Useful for debugging or tools that expect numbered G-code.

## Verbose G-code

Include descriptive comments for G-code lines and blocks to make the file human-readable and easier to debug.  
Verbose mode produces much larger files and may slow down SD-card printing on some printers.

## Label Objects

Insert comments that label moves with the object they belong to (object index or name). This is useful for integrations such as OctoPrint's Cancel Object plugin and for human inspection of the G-code.

> [!IMPORTANT]
> Object labelling is not compatible with Single-Extruder Multi-Material configurations or with "Wipe into Object" / "Wipe into Infill" strategies.  
> When those features are active, labels may be omitted.

## Exclude Objects

Add an `EXCLUDE OBJECT` marker or command in the exported G-code for objects flagged as excluded. This helps post-processors or custom scripts recognise excluded parts.

## Filename Format

Define a filename template for exported G-code. Templates may include tokens like project name, date, or other metadata to produce consistent and informative filenames on export.

For example:

```c++
{input_filename_base}_{filament_type[initial_tool]}_{print_time}.gcode
```

Can be used to generate filenames like `OrcaCube_PLA_1h15m.gcode`.

> [!TIP]
> Check [Built in placeholders variables](built-in-placeholders-variables) for available tokens and their meanings.

## Snapmaker 2.0 thumbnails

Snapmaker A250 and A350 profiles, including dual-extruder and upgrade-kit variants, export a Luban-compatible header for the
Snapmaker 2.0 touchscreen. The header contains the estimated print time in seconds, the final file line count, and a single
PNG thumbnail encoded as a data URL. The first configured thumbnail size is used; its image is encoded as PNG regardless of
the configured image format. Standard thumbnail blocks are also retained for G-code import and print-host compatibility.
An empty thumbnail setting disables the image without removing the header metadata.

The header also supplies normal and tool-change retraction distances, initial nozzle and bed temperatures, and print bounds.
The touchscreen requires retraction metadata as well as a line count to avoid scanning the entire file for 3D prints.

Other printer models retain their existing thumbnail formats. Re-slice and export existing projects to generate the new header.
