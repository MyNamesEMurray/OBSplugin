#!/usr/bin/env python3
"""Populate the uploaded TestFlight build's "What to Test" section.

Runs right after the TestFlight upload: waits for App Store Connect to
register the build (matched by CFBundleVersion), then writes the latest
GitHub release's "What's Changed" section — converted to plain text —
into the build's betaBuildLocalizations `whatsToTest` attribute, which
is exactly what testers see under "What to Test" in the TestFlight app.

Environment:
  ASC_KEY_ID / ASC_ISSUER_ID / ASC_KEY_P8   App Store Connect API key
  BUNDLE_ID                                 app bundle id
  BUILD_NUMBER                              CFBundleVersion just uploaded
  GITHUB_TOKEN / GITHUB_REPOSITORY          provided by Actions
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testflight_feedback import ASC_BASE, asc_get, asc_token, gh, request

MAX_LEN = 4000        # ASC limit for whatsToTest
POLL_SECONDS = 60
POLL_LIMIT = 30       # ~30 min; processing usually takes 5-15


def release_notes(repo):
    """Latest release's tag + the changelog part of its body."""
    status, rel = gh(f"/repos/{repo}/releases/latest")
    if status != 200:
        return "", ""
    body = rel.get("body") or ""
    # The release body is install boilerplate followed by the
    # auto-generated "What's Changed" section; testers only need the
    # second part.
    m = re.search(r"^#+\s*What's Changed\s*$", body, re.M | re.I)
    if m:
        body = body[m.end():]
    return rel.get("tag_name") or "", body


def plain_text(md):
    """Markdown → the plain text TestFlight displays."""
    out = []
    for line in md.splitlines():
        line = line.strip()
        if re.match(r"\**Full Changelog", line, re.I):
            continue
        line = re.sub(r"^#+\s*", "", line)                     # headers
        line = re.sub(r"^[*-]\s+", "• ", line)                 # bullets
        line = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", line)   # links
        line = line.replace("**", "").replace("`", "")
        out.append(line)
    return re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()


def main():
    repo = os.environ["GITHUB_REPOSITORY"]
    bundle_id = os.environ["BUNDLE_ID"]
    build_number = os.environ["BUILD_NUMBER"]

    tag, body = release_notes(repo)
    notes = plain_text(body) or "General improvements and bug fixes."
    text = (f"Changes in this build ({tag or 'latest'}):\n\n" + notes)[:MAX_LEN]

    token = asc_token()
    apps = asc_get("/v1/apps", token, **{"filter[bundleId]": bundle_id,
                                         "fields[apps]": "bundleId"})
    if not apps or not apps.get("data"):
        print(f"::warning::no app found for bundle id {bundle_id}")
        return 1
    app_id = apps["data"][0]["id"]

    # The upload finishes before ASC registers the build; poll for it.
    # Tokens live ~10 min, so mint a fresh one per attempt.
    build_id = None
    for attempt in range(POLL_LIMIT):
        token = asc_token()
        builds = asc_get("/v1/builds", token,
                         **{"filter[app]": app_id,
                            "filter[version]": build_number, "limit": 1})
        if builds and builds.get("data"):
            build_id = builds["data"][0]["id"]
            break
        print(f"build {build_number} not registered yet "
              f"({attempt + 1}/{POLL_LIMIT}) — retrying in {POLL_SECONDS}s")
        time.sleep(POLL_SECONDS)
    if not build_id:
        print("::warning::build never appeared in App Store Connect — "
              "set What to Test manually")
        return 1

    locs = asc_get(f"/v1/builds/{build_id}/betaBuildLocalizations", token)
    existing = (locs or {}).get("data", [])
    if existing:
        loc_id = existing[0]["id"]
        status, resp = request(
            f"{ASC_BASE}/v1/betaBuildLocalizations/{loc_id}", token,
            method="PATCH",
            body={"data": {"type": "betaBuildLocalizations", "id": loc_id,
                           "attributes": {"whatsToTest": text}}})
    else:
        status, resp = request(
            f"{ASC_BASE}/v1/betaBuildLocalizations", token, method="POST",
            body={"data": {"type": "betaBuildLocalizations",
                           "attributes": {"whatsToTest": text,
                                          "locale": "en-US"},
                           "relationships": {"build": {"data": {
                               "type": "builds", "id": build_id}}}}})
    if status in (200, 201):
        print(f"What to Test set for build {build_number} "
              f"({len(text)} chars, from {tag or 'latest release'})")
        return 0
    print(f"::warning::setting What to Test failed ({status}): "
          f"{str(resp)[:300]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
