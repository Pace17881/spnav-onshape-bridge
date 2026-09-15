# Security Policy

This daemon terminates TLS and parses untrusted WebSocket/JSON input from a
browser tab, so security issues here are taken seriously even though the
project is small and young (see [CHANGELOG.md](CHANGELOG.md) for release
status).

## Reporting a vulnerability

Please report security issues privately rather than opening a public issue:

**polsterseb@protonmail.com**

Include:
- A description of the issue and its potential impact.
- Steps to reproduce, or a proof of concept if you have one.
- The affected version/commit.

You should get an acknowledgement within a few days. There is no bug bounty;
this is a small personal-use-turned-open-source project, not a company.
Please give a reasonable amount of time to fix an issue before any public
disclosure.

## Supported versions

Only the latest tagged release is supported. There is no long-term-support
branch at this project's current size.

## Scope and known limitations

The [README's "Security model" section](README.md#security-model) documents
the deliberate design decisions (Origin allow-list, per-install certificates,
loopback-only binding, systemd sandboxing). Please read
[POSTMORTEM.md](POSTMORTEM.md) and [BROWSER_TEST.md](BROWSER_TEST.md) before
reporting something already tracked there - in particular, long-running
connection stability is explicitly **not** yet proven (BROWSER_TEST.md notes
one unexplained disconnect after ~160s in testing).

Out of scope: attacks that require an attacker who already has local code
execution as the same OS user this daemon runs as, or physical access to the
machine - at that point they can already read spacenavd's socket, the
daemon's certificate/key files (mode 0600 under a 0700 directory, but still
readable by the same user), and everything else this daemon could protect.

## Dependencies

- `libspnav` / `spacenavd`: see the [spacenav project](https://spacenav.sourceforge.net/)
  for their own security posture.
- `cJSON` (vendored, pinned to v1.7.18 in `third_party/`): report cJSON-
  specific issues upstream at https://github.com/DaveGamble/cJSON as well,
  since a fix there won't reach this project until the vendored copy is
  updated.
- OpenSSL: report OpenSSL-specific issues to the OpenSSL project; this
  project only uses OpenSSL for TLS and X.509 generation via its public API.
