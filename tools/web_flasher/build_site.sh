#!/usr/bin/env bash
# Assembles the install page at danielstanciu.com/l7 into <out dir>: the page,
# and the latest L7 release's two .hex files with firmware/latest.json naming
# them. The firmware is hosted beside the page because GitHub's release
# downloads can't be fetched from another site's page.
#
#   tools/web_flasher/build_site.sh <out dir> [<release tag>]
#
# Needs gh (signed in, or GH_TOKEN set) and jq. Run by
# .github/workflows/web-flasher.yml; also works locally for a test:
#   tools/web_flasher/build_site.sh /tmp/l7site && (cd /tmp && python3 -m http.server)
# then open http://localhost:8000/l7site/ in Chrome (fonts won't load: they
# come from the portfolio site).
set -euo pipefail

out=${1:?usage: build_site.sh <out dir> [<release tag>]}
tag=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
repo=${GITHUB_REPOSITORY:-minusro/L7}

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
# The page's ?v= stamp: a hash of its code, so it changes only when the code
# does (and an unchanged daily run commits nothing)
stamp=$(cat "$here/index.html" "$here/flasher.js" "$here/halfkay.js" "$here/unzip.js" | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-10)

# A release can be published before its files finish uploading: wait for both.
json=
for i in $(seq 1 30); do
  json=$(gh release view $tag -R "$repo" --json tagName,publishedAt,url,assets)
  n=$(jq '[.assets[].name | select(test("-M8-Model0[12]\\.hex$"))] | length' <<<"$json")
  [ "$n" -ge 2 ] && break
  echo "waiting for the release's .hex files ($n of 2)…"
  sleep 10
done
asset() { jq -r --arg re "$1" '[.assets[] | select(.name | test($re))][0] // empty' <<<"$json"; }
m1=$(asset '-M8-Model01\.hex$' | jq -r '.name // empty')
m2=$(asset '-M8-Model02\.hex$' | jq -r '.name // empty')
[ -n "$m1" ] && [ -n "$m2" ] || { echo "the release has no Model:01 and Model:02 .hex files" >&2; exit 1; }
version=$(jq -r .tagName <<<"$json")

rm -rf "$out"
mkdir -p "$out/firmware"
gh release download "$version" -R "$repo" -D "$out/firmware" -p "$m1" -p "$m2"

# The page, with ?v= on its script so a deploy is never served a cached old one
sed "s|src=\"flasher.js\"|src=\"flasher.js?v=$stamp\"|" "$here/index.html" > "$out/index.html"
cp "$here/flasher.js" "$here/halfkay.js" "$here/unzip.js" "$here/favicon.svg" "$here"/favicon-*.png "$here/apple-touch-icon.png" "$here/social-card.png" "$out/"
cp "$here/../../docs/readme/demo.gif" "$out/demo.gif"   # the README's demo

# Dirtywave's firmware: the page downloads it straight from Dirtywave (their
# EULA rules out hosting it here), from the zip their downloads page links to.
# That page can't be read from the browser (no CORS), so its link is looked up
# here, followed to where it lands (a redirect without CORS headers would stop
# the browser), and checked to allow the page's download.
dw_page=https://dirtywave.com/pages/resources-downloads
dw_fallback=https://raw.githubusercontent.com/DirtyWave/M8Firmware/main/M8Firmware.zip
dw_links=$(curl -fsSL -A "Mozilla/5.0" "$dw_page" | grep -o -i -E 'href="[^"]+\.zip[^"]*"' | sed -E 's/^href="//; s/"$//; s/&amp;/\&/g' || true)
dw_link=$(grep -i 'm8firmware' <<<"$dw_links" | head -1 || true)
[ -n "$dw_link" ] || dw_link=$(head -1 <<<"$dw_links")
case "$dw_link" in //*) dw_link="https:$dw_link" ;; /*) dw_link="https://dirtywave.com$dw_link" ;; esac
if [ -z "$dw_link" ]; then
  echo "::warning::No firmware zip link found on $dw_page; using $dw_fallback"
  dw_link=$dw_fallback
fi
dw_zip=$(curl -fsSIL -o /dev/null -w '%{url_effective}' "$dw_link" || true)
if ! curl -fsSI -H "Origin: https://danielstanciu.com" "${dw_zip:-$dw_link}" | grep -qi '^access-control-allow-origin: \*'; then
  echo "::warning::$dw_link (landing at ${dw_zip:-?}) doesn't allow downloads from another site's page: the page's Restore will fail"
fi
dw_zip=${dw_zip:-$dw_link}
echo "Dirtywave firmware: $dw_zip"

entry() { jq -n --arg file "$1" --argjson bytes "$(wc -c < "$out/firmware/$1" | tr -d ' ')" --arg sha256 "$(sha256 "$out/firmware/$1")" '{$file, $bytes, $sha256}'; }
jq -n \
  --arg version "$version" \
  --arg published "$(jq -r .publishedAt <<<"$json")" \
  --arg url "$(jq -r .url <<<"$json")" \
  --arg guide "$(asset 'User-Guide\.pdf$' | jq -r '.url // empty')" \
  --arg cheatsheet "$(asset 'Cheatsheet\.pdf$' | jq -r '.url // empty')" \
  --argjson model01 "$(entry "$m1")" \
  --argjson model02 "$(entry "$m2")" \
  --arg dw_page "$dw_page" \
  --arg dw_zip "$dw_zip" \
  '{$version, $published, $url, docs: {guide: (if $guide == "" then null else $guide end), cheatsheet: (if $cheatsheet == "" then null else $cheatsheet end)}, firmware: {$model01, $model02}, dirtywave: {page: $dw_page, zip: $dw_zip}}' \
  > "$out/firmware/latest.json"

# Check the hosted files the way the page will read them
for pair in "$m1:Teensy 4.0" "$m2:Teensy MicroMod"; do
  report=$(node "$here/check.mjs" "$out/firmware/${pair%%:*}")
  echo "$report"
  grep -q "for: ${pair#*:}" <<<"$report" || { echo "${pair%%:*} isn't built for a ${pair#*:}" >&2; exit 1; }
done
cat "$out/firmware/latest.json"
