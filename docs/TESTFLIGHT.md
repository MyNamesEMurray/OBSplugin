# TestFlight setup (one-time)

The [`TestFlight` workflow](../.github/workflows/testflight.yml) builds,
signs, and uploads the iOS app — including the screen-mirror broadcast
extension — to TestFlight. Signing is cloud-managed: no certificates or
provisioning profiles live in the repo, only an App Store Connect API key
in repository secrets. Requires a paid Apple Developer account.

## 1. Create an App Store Connect API key

1. [App Store Connect](https://appstoreconnect.apple.com) → **Users and
   Access** → **Integrations** → **App Store Connect API** → **Team Keys**.
2. **Generate API Key**: name e.g. `lenslink-ci`, access **App Manager**.
3. Note the **Key ID** and the **Issuer ID** (shown at the top of the page),
   and **download the `.p8` file** (single chance — keep it safe).

## 2. Add the repository secrets

GitHub repo → **Settings → Secrets and variables → Actions → New
repository secret**, four times:

| Secret | Value |
|---|---|
| `APP_STORE_CONNECT_KEY_ID` | the Key ID from step 1 |
| `APP_STORE_CONNECT_ISSUER_ID` | the Issuer ID from step 1 |
| `APP_STORE_CONNECT_KEY_P8` | the *entire contents* of the `.p8` file (open it in a text editor, copy everything including the BEGIN/END lines) |
| `APPLE_TEAM_ID` | your 10-character Team ID ([developer.apple.com/account](https://developer.apple.com/account) → Membership details) |

## 3. Create the app record (once)

1. App Store Connect → **Apps** → **+** → **New App**.
2. Platform iOS, name **LensLink**, bundle ID **com.exaltedpixels.LensLinkCamera**
   (register it under Identifiers if it isn't offered; the CI's
   `-allowProvisioningUpdates` registers the broadcast extension's child id
   `…LensLink.broadcast` automatically on first run).
3. SKU: anything, e.g. `lenslink`.

## 4. Run it

- Manually: **Actions → TestFlight → Run workflow**, or
- Automatically: it runs on every **published release**.

The first upload takes a few extra minutes of App Store Connect processing;
after that the build appears under the app's **TestFlight** tab. Add
yourself (internal testing group) and it lands on your phone via the
TestFlight app immediately — external tester groups need a one-time beta
review by Apple.

## Why this also matters for the screen-mirror extension

Broadcast upload extensions are sensitive to how the app was signed:
re-signing tools frequently produce an appex that iOS refuses to launch
(installed, visible in the picker, but the broadcast never starts).
TestFlight builds are provisioned through Apple's own pipeline, which
removes that whole failure class — and removes the 7-day re-sign cycle for
everyone.
