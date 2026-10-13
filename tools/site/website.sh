#!/bin/sh
# Assemble the deployed site in build/website: website/ at the root (ak1122.cc), site/ under /sayooshi/.
# The builder page gets the commit and its date at the bottom. Netlify runs this (netlify.toml); so does make website.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/build/website"
repo=https://github.com/aa022/sayooshi

hash=$(git -C "$root" rev-parse HEAD 2>/dev/null || printf '%s' "${COMMIT_REF:-}")
date=$(git -C "$root" log -1 --format=%cd --date=short 2>/dev/null || date -u +%F)
if [ -n "$hash" ]; then
  stamp="<br><small>build <a href=\"$repo/commit/$hash\">$(printf '%.7s' "$hash")</a>, last update $date</small>"
else
  stamp="<br><small>last update $date</small>"
fi

rm -rf "$out"
mkdir -p "$out"
cp -R "$root/website/." "$out/"
cp -R "$root/site" "$out/sayooshi"
rm -f "$out/sayooshi/README.md"
# The base makes the page's relative paths work at /sayooshi as well as /sayooshi/.
sed -e 's|<meta charset="utf-8">|&\
<base href="/sayooshi/">|' -e "s|<!--stamp-->|$stamp|" "$root/site/index.html" > "$out/sayooshi/index.html"
printf 'website: build/website (%s, %s)\n' "$(printf '%.7s' "${hash:-none}")" "$date"
