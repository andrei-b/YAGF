# Image action icons

These transparent PNGs adapt the user's supplied icon sheet with the built-in image editing tool. They use YAGF's blue, slate, and white palette and are bundled through `resources/icons.qrc`.

Initial shared editing instructions: preserve the reference's folded documents, layout panels, directional arrows and before/after compositions; extract only the requested icon; use crisp antialiased edges and restrained gradients; keep a genuinely transparent background with no labels, extra objects or shadows. Primary blue: `#216ec1`; outlines and text: `#334962`; paper: `#ffffff`; panel fills: `#9dbde2`, `#c9dff8`, and slate `#718096` instead of pink/red.

Prompt set:

- `rotate-left.png`: extract the upper-left folded document and counterclockwise arrow pointing left, preserving its layout panels and proportions.
- `rotate-right.png`: extract the upper-right folded document and clockwise arrow pointing right, matching the rotate-left companion's scale, outline weight and palette.
- `correct-skew.png`: a folded page with slightly slanted blue text, a horizontal alignment guide, and a small adjustment arrow. Match the blue/slate palette and avoid the large 90-degree rotation arrow.
- `smart-split.png`: simplify the original content blocks into three separate, slightly staggered blue regions with generous transparent gaps and a few dark text strokes. Remove the enclosing pages and tiny arrow so the segmented silhouette is clear at toolbar size.
- `clear-blocks.png`: enlarge one folded document and its dominant X over a pale blue grid. Remove the smaller blank page and use YAGF's warning accent `#d24119` for the X, with a narrow white separating edge. This distinguishes clearing from blue Smart split by both silhouette and color.

The complete revision prompts are in [distinct-prompts.json](distinct-prompts.json). The revised icons and new Correct skew icon were created with the built-in image tool.

The generated assets were trimmed to their visible artwork and downscaled to a maximum 256-pixel edge, including transparent margins, for small toolbar and high-DPI rendering.
