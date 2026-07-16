# Security policy

## Reporting a vulnerability

Please use GitHub's **private vulnerability reporting** (this repository's
**Security** tab → *Report a vulnerability*) instead of a public issue, so
a fix can ship before the details do.

## Threat model — what's in scope

LensLink is designed for **trusted home/studio networks**:

- The video/audio stream is **unencrypted on the local network** by
  design, and the phone's listener accepts connections from that network.
  Interception or unwanted viewing on a hostile LAN is a documented
  limitation (see the README), not a reportable vulnerability.
- The browser control panel binds to **localhost only**.

In scope — things worth reporting:

- Memory corruption or crashes in the plugin reachable via malformed
  network packets (header parsing, decoder input).
- Anything that lets a device on the network execute code, read files, or
  escape the broadcast extension's sandbox.
- The control panel or its API becoming reachable from other machines.

## Supported versions

Only the [latest release](../../releases/latest) is supported; fixes ship
as a new release rather than backports.
