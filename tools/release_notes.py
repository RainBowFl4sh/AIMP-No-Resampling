#!/usr/bin/env python3
"""Release notes for GitHub from CHANGELOG.md, laid out like the releases of the author's Discord RP plugin:
the version's section ("### New" / "### Changed" / "### Fixed" ...), the downloads, the PayPal line and the
"Full Changelog" link at the bottom.

usage: python3 tools/release_notes.py <version> [--title "short description"] [--changelog CHANGELOG.md]
The previous version (for the Full Changelog link) is the next section below in CHANGELOG.md."""
import argparse, re, sys

REPO = "https://github.com/RainBowFl4sh/AIMP-No-Resmapling"
DONATE = "https://www.paypal.com/donate/?hosted_button_id=TNTR6BR72JZ6E"  # same as AR_URL_DONATE (src/Updater.h)

ap = argparse.ArgumentParser()
ap.add_argument("version")
ap.add_argument("--title", default="")
ap.add_argument("--changelog", default="CHANGELOG.md")
a = ap.parse_args()

lines = open(a.changelog, encoding="utf-8").read().splitlines()
heads = [i for i, l in enumerate(lines) if l.startswith("## ")]
try:
    start = next(i for i in heads if lines[i][3:].strip() == a.version)
except StopIteration:
    sys.exit("version %s not in %s" % (a.version, a.changelog))
later = [i for i in heads if i > start]
end = later[0] if later else len(lines)
previous = lines[later[0]][3:].strip() if later else ""

v = a.version
body = lines[start + 1:end]
while body and not body[0].strip():
    body.pop(0)
out = ["## %s%s" % (v, " " + a.title if a.title else ""), ""]
for l in body:
    m = re.match(r"^([A-Z][A-Za-z ]*):$", l)  # "New:", "Changed:", "Fixed:", "Improved:"
    out.append("### " + m.group(1) if m else l)
while out and not out[-1].strip():
    out.pop()
out += ["",
        "### Downloads",
        "- `PreventResampling-%s.aimppack` - AIMP 4.70, 5 and 6 (Windows 32 / 64-bit and Linux), install by double-click or in AIMP" % v,
        "- `PreventResampling-%s.aimppack.sha256` - checksum of the package (the update check on the About tab uses it)" % v,
        "- `PreventResampling-%s-win64.zip` / `PreventResampling-%s-win32.zip` - manual install (32-bit also for AIMP 4)" % (v, v),
        "- `PreventResampling-%s-linux-x64.zip` - AIMP for Linux, manual install" % v,
        "",
        "If you like the plugin and want to say thanks: [Support it via PayPal](%s) - completely optional, bug reports and translations help just as much." % DONATE,
        ""]
if previous:
    out.append("**Full Changelog**: %s/compare/v%s...v%s" % (REPO, previous, v))
print("\n".join(out))
