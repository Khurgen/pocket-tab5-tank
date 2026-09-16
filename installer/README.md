# Browser installer

Plug the board in, open a page, click Install: the same "flash it from the
browser" flow ESPHome and Home Assistant use ([ESP Web
Tools](https://esphome.github.io/esp-web-tools/), Apache-2.0, vendored under
`vendor/`). Chrome or Edge on a desktop; it uses Web Serial, which Safari
and Firefox don't have.

## Build the upload folder

```
idf.py -B ~/.cache/pocket-tank/fw-build build     # in firmware/ (or any -B dir)
tools/make_installer.py                            # -> installer/dist/
python3 -m http.server -d installer/dist 8765      # local check at http://localhost:8765
```

The page follows stratobuilds.com's design (the `#f8f8f8` page, white /
`#222` / lavender cards at 10px, Inter Tight and Roboto Mono from Google
Fonts, the red button). When only the page changed, re-uploading
`index.html` is enough - the binaries and `vendor/` are untouched.

`installer/dist/` is the whole thing: `index.html`, `manifest.json`,
`firmware/*.bin` (bootloader, partition table, app, model), `vendor/`. The
offsets in the manifest come from the build's `flasher_args.json` and from
`firmware/partitions.csv`, and the page carries the git version. It is
gitignored: rebuild it for every release.

## It updates itself (GitHub Pages)

`.github/workflows/installer.yml` in the PUBLIC repo builds the firmware
with ESP-IDF v5.4.1 on every push to `main` that touches `firmware/`,
`common/`, `model/out/`, `installer/` or the assembler, runs
`tools/make_installer.py`, and deploys the folder to
**https://mediacutlet.github.io/pocket-tank/**. Pages serves HTTPS with
`Access-Control-Allow-Origin: *`, so the copy on stratobuilds.com points
its button at that manifest:

```
tools/make_installer.py --manifest-url https://mediacutlet.github.io/pocket-tank/manifest.json --out /tmp/site
```

and only that `index.html` lives on the site. It fetches the manifest on
load and shows the version and build date of what it will actually flash,
so pushing to the public repo is the whole release step: no upload, no
cache purge. (Manual failure mode: the Actions run is red - `gh run list
--repo mediacutlet/pocket-tank`.)

## Host it

Web Serial needs a secure context, so the page must be on **HTTPS** (or
`localhost`). Any static host works; the manifest and the `.bin` files must be
fetchable from the page's origin (or send CORS headers).

**stratobuilds.com (WordPress behind Cloudflare):** upload `installer/dist/`
as a folder next to WordPress, e.g. `public_html/pocket-tank/`, and link
`https://stratobuilds.com/pocket-tank/`. Being a plain folder it is outside
WordPress, so themes, caching and security plugins don't touch it. Things to
check once:

- Open the page, the button must say *Install Pocket Tank*, not the red
  unsupported text. If it never appears, Cloudflare's Rocket Loader is
  rewriting the module script: exclude `/pocket-tank/*` from it (a
  Configuration Rule), or turn it off.
- In the browser's network tab `manifest.json` and `firmware/*.bin` must be
  200. A 403 on `.bin` means the host blocks the type: the `.htaccess` in the
  folder adds it for Apache/LiteSpeed; on nginx add
  `types { application/octet-stream bin; }`.
- Cloudflare caches `.bin` and `.js` by default. After uploading a new build,
  purge `/pocket-tank/*` (the manifest carries the version, so a stale
  cache shows an old version string on the page).

**GitHub Pages** (the public repo) is the other easy option: push `dist/` to
a `gh-pages` branch or a `docs/installer/` folder. Pages serves HTTPS and
`Access-Control-Allow-Origin: *`, so the button can even live on a WordPress
page (Custom HTML block with the `<script type="module">` tag and the
`<esp-web-install-button manifest="https://...manifest.json">` element)
while the binaries stay on GitHub.

## What the user sees

Click → the browser's port picker (`USB JTAG/serial debug unit`) → a dialog
that asks whether to erase first (yes = a brand-new tank; no = keep an
existing tank's fish and history) → a progress bar over the four parts →
*Installation complete*, and the board resets into the tank. The page's
"If something's off" section covers the usual snags: a sleeping tank hides
its USB port (press PWR), holding BOOT while plugging in forces download
mode, Linux group permissions, charge-only cables.

## Verified

The artifact set in `dist/firmware/` at the manifest's offsets was written to
the real board with `esptool.py write_flash` (the exact operation ESP Web
Tools performs, from the same files), and the tank booted; the page, manifest
and vendor bundle were checked from a local server. The browser's own port
picker is a native dialog, so the click-through itself is a human test.
