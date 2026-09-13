# Station logos

This directory is the operator-supplied logo set for the listener UI.
`compose.web.yml` bind-mounts it read-only onto the path webui serves from
(`/static/logos/`), so logos can be added or swapped with a
`docker restart feedmyfm-webui` -- no image rebuild.

The webui image itself only bakes `_default.svg`; everything else lives here.

Real logos don't have to live in *this* directory: `compose.web.yml`'s
`LOGOS_DIR` env var points the mount at any directory with the same layout
(a `_default.svg`/`_default.png`/`aliases.json` plus real files) -- useful
when the real logo set belongs in a separate, private per-deployment repo
rather than this one (see Licensing below).

## Adding a logo

`webui` holds no config, so it can't be *told* which logo belongs to which
station -- it matches on the station **label** (from `stations.yml`) instead.
Name the file after the label, slugified: lowercased, every run of
non-alphanumeric characters collapsed to `-`, ends trimmed.

    "Radio One"     ->   radio-one.png
    "Pop Wave"      ->   pop-wave.svg

Accepted extensions, first match wins: `.svg` `.png` `.webp` `.jpg` `.jpeg`.
Square-ish, >=64 px, transparent background looks best (rendered at 34/28 px).

## When the slug doesn't match

Add an override to `aliases.json` mapping the exact label to a filename or
bare slug in this directory:

    {
      "Golden Oldies+": "golden-oldies-plus",
      "City Radio 107.4": "city-radio"
    }

Needed whenever two labels slugify to the same thing (e.g. `Golden Oldies`
and `Golden Oldies+`) or a label carries characters the slug drops.

## Fallback

Any station with no match renders `_default.svg` (a generic radio glyph).
`onerror` in the template also falls back to it if a resolved file 404s.

## Licensing

Broadcaster logos are trademarks. Fine for a private LAN instance; do **not**
ship them in a public image without clearing rights. Only `_default.svg` and
`aliases.json` are tracked in git -- real logos are a local, operator-supplied
drop-in, and `.gitignore` keeps them untracked.
