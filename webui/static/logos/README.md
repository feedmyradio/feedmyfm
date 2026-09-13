# Station logos

Only `_default.svg` (the generic fallback glyph) is baked into the webui
image here. The real operator-supplied broadcaster logos live in
[`deploy/logos/`](../../../deploy/logos/) and are bind-mounted over this
directory at runtime by `compose.web.yml` -- see that dir's README for how
the label -> file matching and `aliases.json` work.

Running webui outside the compose stack (no bind mount) falls back to
`_default.svg` for every station until you point a mount or drop files here.
