# Flush Options

## Flush into objects' infill

Purging after filament change will be done inside objects' infills. This may lower the amount of waste and decrease the print time. If the walls are printed with transparent filament, the mixed color infill will be seen outside. This option also works with the prime tower disabled. Compatible sparse infill is printed first after a filament change, using the flushing-volume matrix for shared-nozzle printers or the prime volume for independent-nozzle printers. Only the volume actually assigned to infill is deducted from the filament-change G-code's flushing-volume placeholders; any remainder still uses that printer routine. No extra infill is created when a layer has too little or no sparse infill. Soluble and support-material transitions are excluded. Local-Z subdivided layers retain their normal tool-change behavior.

## Flush into objects' support

Purging after filament change will be done inside objects' support. This may lower the amount of waste and decrease the print time. It will not take effect, unless the prime tower is enabled.
