# Experimental paired water coordinates

Some views of the Hyrule Castle moat and Castle Town canal produce a rectangle
over solid geometry, sometimes with extreme red output. A paired coordinate
rewrite removed both artifacts in the tested views. This is an opt-in reference
implementation, not a demonstrated correction to the translator or GPU driver.
Related investigation: https://github.com/tangerineturtle1/NX2navi/issues/398.
That earlier report used a different, fragment-coordinate workaround.

## What changes

The vertex output carries undivided clip X, Y and the actual output W. The
fragment input uses perspective interpolation instead of NoPerspective and
reconstructs X/W and Y/W before the original half-scale and Y inversion. The
fragment's material and sampling calculations remain intact. There are no
empirical offsets, color clamps, or object/scene coordinates in this recipe.

The runtime requires independent stage layouts and the exact original pair:
vertex FNV64 `f6d753852c47b0de`, fragment `a39c3d09241441fa` (descriptor-set-1
form `3d3a316ab4da3bba`). Both stages change together. Separate pre-raster and
fragment library variants prevent reuse of the replacement with another pair.
Other shader pairs and the default runtime behavior remain unchanged.

## Private local preparation

Use privately generated modules from the matching translator configuration:

```sh
python tools/projective_recipe.py --vertex <local-vertex.spv> --fragment <local-fragment.spv> --output build/water-projective
spirv-val --target-env vulkan1.2 build/water-projective/f6d753852c47b0de.spv
spirv-val --target-env vulkan1.2 build/water-projective/3d3a316ab4da3bba.spv
```

Unexpected input hashes and existing output files are rejected. Rebuild the
runtime using the documented UCRT64 workflow. Set `NX2NAVI_SPV_OVERRIDE` to the
absolute output directory in the test process only, then restart the game.
Keep the original installation and use separate pipeline caches for comparisons;
disable pipeline-binary reuse and boot prebuilding during this experiment.
Unset the override and restart to disable it. The directory is checked once at
startup/first pipeline use; do not change its contents during a run.

The exact source integration is compilation-checked, but has not itself had a
gameplay run. The equivalent embedded-module diagnostic correction was tested
on versions 0.7.1 and 0.8.0, game 1.1.1, RX 9070 XT, driver 26.10.1, in repeated
short visits to the moat and market canal. No timed endurance run was recorded.
Other vendors, drivers, resolutions, water scenes and translator versions need
validation before enabling this generally. A changed module deliberately fails
the narrow identity check rather than guessing a match.

## Evidence and limits

The recorded fragment invocations with UV error above 0.001 changed as follows:

| View | Original | Paired correction |
| --- | ---: | ---: |
| Castle moat | 4,999 / 18,065 | 0 / 20,400 |
| Market canal | 7,346 / 72,720 | 0 / 72,033 |

These are instrumented fragment invocations, not unique visible pixels. Views
were revisited manually and are not pixel-identical. Visual checks found no
rectangle or red artifact in those reproductions after correction. This supports
the coordinate representation as a successful workaround; it does not establish
which translation, interpolation, clipping or driver operation first diverged.
The dramatic red output is unintended in those scenes, but its complete
arithmetic origin and portability of this workaround remain unproven.

This source contains only authored patch logic and diagnostic identifiers.
Generated modules, disassembly, caches, game executables and raw GPU captures
must stay private under ignored build directories and must not be uploaded.
