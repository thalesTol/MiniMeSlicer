# Printer profiles from OrcaSlicer

MiniMe's printer profiles for brands other than Bambu Lab (`resources/profiles/<brand>`) are OrcaSlicer's, converted to
MiniMe's (Bambu Studio's) settings. OrcaSlicer's profiles are licensed under the GNU AGPL v3, like MiniMe.

To bring in newer ones:

1. Get OrcaSlicer's `resources/profiles` folder (a sparse checkout is enough):
   ```
   git clone --depth 1 --filter=blob:none --sparse https://github.com/OrcaSlicer/OrcaSlicer.git orca
   cd orca && git sparse-checkout set resources/profiles
   ```
2. Convert (from the MiniMe checkout):
   ```
   python tools/orca_profiles/convert_orca.py . <orca>/resources/profiles <out dir> <report.txt>
   ```
   It keeps only the settings MiniMe knows, renames Orca's values that MiniMe has under another name, and copies the
   generic filaments of Orca's shared filament library into the brands that use them. `report.txt` lists what was dropped.
   Printers it cannot use are left out: printers with several nozzles (IDEX, tool changers), belt printers, and the models
   listed in `excluded_printers.txt` (they did not slice in MiniMe).
3. Check that every profile's parent is there (a missing one stops MiniMe loading the whole brand):
   ```
   python tools/orca_profiles/check_parents.py <out dir>
   ```
4. Replace the brand folders and `<brand>.json` files in `resources/profiles` (never `BBL`, `blacklist.json`) with the
   converted ones.
5. Start MiniMe with the Claude bridge on, and slice a test cube on every printer:
   ```
   python tools/orca_profiles/slice_every_printer.py resources/profiles <cube.stl> results.jsonl [Brand1,Brand2]
   ```
   Every line of `results.jsonl` is one printer: `stage` is `ok` when it sliced and produced G-code.
