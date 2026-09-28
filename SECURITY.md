# Security policy

This is the policy of the Omodachi fork of Sunshine. It does not speak for
upstream [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine).

## Reporting a vulnerability

Please report it privately, in either of two ways:

- **GitHub.** On this repository's **Security** tab, press **Report a
  vulnerability**
  (<https://github.com/omodachi/omodachi-sunshine/security/advisories/new>).
  That opens a private security advisory which only you and the maintainer can
  see.
- **Email** <security@omodachi.app>.

Please do not put the details in a public issue, pull request or discussion.

Reports are read by the maintainer, Leo Zhang. There is no bug bounty.

## What to include

- The release tag (`sunshine-<sha>`) or commit you looked at. The release
  package names its own commit in the `commit=` line of its `SOURCE` file.
- What an attacker needs (the same network, a paired client, a local account
  on the computer, ...) and what they get.
- Steps to reproduce it or a proof of concept, and what you expected instead.

## Scope

This repository: the fork's own changes, which [FORK.md](FORK.md) lists
(the opt-in Unix-socket control plane for managed pairing, the media-session
observer and the owned-output desktop binding), the release packages built
from it by `scripts/package_release.sh`, and upstream Sunshine's code as this
fork ships it.

The other parts of Omodachi take reports the same two ways. Use the repository
the problem is in; if you are not sure which, email.

| Repository | What it is |
| --- | --- |
| [`omodachi-core`](https://github.com/omodachi/omodachi-core/security) | the host daemon, which installs, starts and drives this fork |
| [`omodachi-plugin`](https://github.com/omodachi/omodachi-plugin/security) | the Omarchy plugin: the bar icon, the panel and the Install button's bootstrap |
| [`omodachi-ios`](https://github.com/omodachi/omodachi-ios/security) | the iPhone and iPad app |
