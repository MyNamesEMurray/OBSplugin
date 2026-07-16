#!/usr/bin/env python3
"""Mirror TestFlight beta feedback into GitHub issues.

Polls the App Store Connect Feedback API (screenshot + crash submissions)
for the app and opens one GitHub issue per submission, labelled
`testflight`. Already-mirrored submissions are recognised by an
`asc-feedback-id` marker in existing issue bodies, so re-runs are
idempotent and closing/editing the issues is safe.

Privacy: the ASC records include the tester's email — it is deliberately
NEVER written into the (public) issue. Screenshot download URLs expire,
so issues link to App Store Connect instead of embedding dead images.

Environment:
  ASC_KEY_ID / ASC_ISSUER_ID / ASC_KEY_P8   App Store Connect API key
  BUNDLE_ID                                 app bundle id
  GITHUB_TOKEN / GITHUB_REPOSITORY          provided by Actions
"""

import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import jwt  # PyJWT

ASC_BASE = "https://api.appstoreconnect.apple.com"
LABEL = "testflight"
MARKER = "asc-feedback-id"
CRASH_LOG_LIMIT = 8000  # chars of crash log inlined per issue


def request(url, token, method="GET", body=None):
    req = urllib.request.Request(url, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    if body is not None:
        req.add_header("Content-Type", "application/json")
        req.data = json.dumps(body).encode()
    try:
        with urllib.request.urlopen(req) as resp:
            return resp.status, json.load(resp)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def asc_token():
    return jwt.encode(
        {
            "iss": os.environ["ASC_ISSUER_ID"],
            "iat": int(time.time()) - 30,
            "exp": int(time.time()) + 600,
            "aud": "appstoreconnect-v1",
        },
        os.environ["ASC_KEY_P8"],
        algorithm="ES256",
        headers={"kid": os.environ["ASC_KEY_ID"]},
    )


def asc_get(path, token, **params):
    url = f"{ASC_BASE}{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params)
    status, data = request(url, token)
    if status != 200:
        print(f"::warning::ASC GET {path} -> {status}: "
              f"{json.dumps(data)[:300]}")
        return None
    return data


def gh(path, method="GET", body=None):
    status, data = request(
        f"https://api.github.com{path}",
        os.environ["GITHUB_TOKEN"], method=method, body=body)
    return status, data


def existing_marker_ids(repo):
    """Scan issues labelled `testflight` for already-mirrored feedback."""
    seen, page = set(), 1
    while True:
        status, issues = gh(f"/repos/{repo}/issues?labels={LABEL}"
                            f"&state=all&per_page=100&page={page}")
        if status != 200 or not issues:
            break
        for issue in issues:
            seen.update(re.findall(rf"{MARKER}: (\S+)", issue.get("body") or ""))
        page += 1
    return seen


def build_versions(payload):
    """Map build-relationship ids -> human build number, from `included`."""
    return {
        inc["id"]: inc.get("attributes", {}).get("version", "?")
        for inc in (payload.get("included") or [])
        if inc.get("type") == "builds"
    }


def gb(n):
    return f"{n / 1e9:.1f} GB" if isinstance(n, (int, float)) else "?"


def meta_table(attrs, build_no, extra_rows=()):
    rows = [
        ("App build", build_no),
        ("Device", f'{attrs.get("deviceModel", "?")} — '
                   f'{attrs.get("devicePlatform", "")} '
                   f'{attrs.get("osVersion", "?")}'),
        ("Locale / TZ", f'{attrs.get("locale", "?")} / '
                        f'{attrs.get("timeZone", "?")}'),
        ("Network", str(attrs.get("connectionType", "?"))),
        ("Battery / free disk", f'{attrs.get("batteryPercentage", "?")}% / '
                                f'{gb(attrs.get("diskBytesAvailable"))}'),
        *extra_rows,
    ]
    lines = ["| | |", "|---|---|"]
    lines += [f"| {k} | {v} |" for k, v in rows]
    return "\n".join(lines)


def make_issue(repo, title, body):
    status, data = gh(f"/repos/{repo}/issues", method="POST",
                      body={"title": title, "body": body,
                            "labels": [LABEL, "bug"]})
    if status != 201:
        print(f"::error::creating issue failed ({status}): "
              f"{json.dumps(data)[:300]}")
        return False
    print(f"created: {data['html_url']}  {title}")
    return True


def main():
    repo = os.environ["GITHUB_REPOSITORY"]
    bundle_id = os.environ["BUNDLE_ID"]
    token = asc_token()

    apps = asc_get("/v1/apps", token, **{"filter[bundleId]": bundle_id,
                                         "fields[apps]": "bundleId"})
    if not apps or not apps.get("data"):
        print(f"::warning::no app found for bundle id {bundle_id}")
        return 0
    app_id = apps["data"][0]["id"]

    # Ensure the label exists (422 = already there).
    gh(f"/repos/{repo}/labels", method="POST",
       body={"name": LABEL, "color": "0366d6",
             "description": "Mirrored from TestFlight feedback"})

    seen = existing_marker_ids(repo)
    created = skipped = 0

    def common(kind, sub):
        attrs = sub["attributes"]
        when = attrs.get("createdDate", "")[:19].replace("T", " ")
        header = (f"<!-- {MARKER}: {sub['id']} -->\n"
                  f"**TestFlight {kind}** — {when} UTC\n\n")
        comment = (attrs.get("comment") or "").strip()
        quoted = "\n".join(f"> {line}" for line in comment.splitlines())
        return attrs, header, quoted

    # ---- screenshot feedback ----------------------------------------
    payload = asc_get(f"/v1/apps/{app_id}/betaFeedbackScreenshotSubmissions",
                      token, sort="-createdDate", limit=200,
                      include="build", **{"fields[builds]": "version"})
    if payload:
        builds = build_versions(payload)
        for sub in payload.get("data", []):
            if sub["id"] in seen:
                skipped += 1
                continue
            build_no = builds.get(
                (sub.get("relationships", {}).get("build", {})
                 .get("data") or {}).get("id"), "?")
            attrs, header, quoted = common("feedback", sub)
            shots = len(attrs.get("screenshots") or [])
            title_hint = (attrs.get("comment") or "").strip().splitlines()
            title = (f'[TestFlight] “{title_hint[0][:60]}”' if title_hint else
                     f'[TestFlight] Screenshot feedback '
                     f'({attrs.get("deviceModel", "?")}, '
                     f'iOS {attrs.get("osVersion", "?")})')
            body = header
            if quoted:
                body += quoted + "\n\n"
            body += meta_table(attrs, build_no, extra_rows=[(
                "Screenshots",
                f"{shots} — view in [App Store Connect]"
                f"(https://appstoreconnect.apple.com/apps/{app_id}"
                f"/testflight/screenshots) (download links expire)")])
            body += ("\n\n_Tester identity is omitted on purpose; "
                     "it is visible in App Store Connect._")
            created += make_issue(repo, title, body)

    # ---- crash feedback ---------------------------------------------
    payload = asc_get(f"/v1/apps/{app_id}/betaFeedbackCrashSubmissions",
                      token, sort="-createdDate", limit=200,
                      include="build", **{"fields[builds]": "version"})
    if payload:
        builds = build_versions(payload)
        for sub in payload.get("data", []):
            if sub["id"] in seen:
                skipped += 1
                continue
            build_no = builds.get(
                (sub.get("relationships", {}).get("build", {})
                 .get("data") or {}).get("id"), "?")
            attrs, header, quoted = common("crash", sub)
            title = (f'[TestFlight] Crash on {attrs.get("deviceModel", "?")}, '
                     f'iOS {attrs.get("osVersion", "?")} (build {build_no})')
            body = header
            if quoted:
                body += quoted + "\n\n"
            body += meta_table(attrs, build_no)
            log = asc_get(f"/v1/betaFeedbackCrashSubmissions/{sub['id']}"
                          f"/crashLog", token)
            text = ((log or {}).get("data", {}).get("attributes", {})
                    .get("logText") or "").strip()
            if text:
                clipped = text[:CRASH_LOG_LIMIT]
                more = ("\n… (truncated — full log in App Store Connect)"
                        if len(text) > CRASH_LOG_LIMIT else "")
                body += (f"\n\n<details><summary>Crash log</summary>\n\n"
                         f"```\n{clipped}{more}\n```\n</details>")
            body += (f"\n\n[View in App Store Connect]"
                     f"(https://appstoreconnect.apple.com/apps/{app_id}"
                     f"/testflight/crashes) — tester identity omitted "
                     f"on purpose.")
            created += make_issue(repo, title, body)

    print(f"done: {created} issue(s) created, {skipped} already mirrored")
    return 0


if __name__ == "__main__":
    sys.exit(main())
