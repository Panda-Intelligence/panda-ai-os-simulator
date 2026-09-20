# Round accessory and runnable per-board review

Base:45b0e54. Keep LICENSE, registry hardware mappings and publication settings unchanged.

Replace the stretched radial-gradient PaperS3 hanging-loop representation with
a centered, aspect-preserving circle. This is the mechanical hook from the
manufacturer reference, NOT a second input button. Keep the PWR side key.
Preserve the LilyGo front ring aspect too. Reserve accessory clearance in Fit
and test real DOM geometry at Fit/1x/2x and multiple viewports.

Unavailable status badges show NA; keep full reason in title/logs. Loading or
missing per-board artifacts must never enable an invalid launch. Add a strict
required-board packing option so full review packages cannot omit a board silently.
Use explicit local runtime/guest sources, validate hashes/board mappings, assemble
a standalone site, and boot every advertised board in isolated browser contexts.
Do not pass a placeholder framebuffer as firmware evidence.

Local review additionally retains unmerged PR15 button mapping and PR16 London
through a separate preview composition; neither review PR is auto-merged.
