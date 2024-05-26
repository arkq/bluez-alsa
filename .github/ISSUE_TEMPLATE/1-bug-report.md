---
name: "\U0001F41B Bug report"
about: Create a report to help improve bluez-alsa.
title:
---

> Please read the [troubleshooting guide](../blob/master/TROUBLESHOOTING.md)
> before raising a new issue.

### Problem

> A clear and concise description of what the bug is.
> If possible, please check if the bug still exists in the master branch (or
> the latest release if you are not using it already).

### Reproduction steps

> Provide a minimal example of how to reproduce the problem. State `bluealsad`
> command line arguments and the content of .asoundrc file (if PCM alias with
> "type bluealsa" was added to that file).

### Setup

> - the OS distribution and version
> - the version of BlueALSA (`bluealsad --version`)
> - the version of BlueZ (`bluetoothd --version`)
> - the version of ALSA (`aplay --version`)
> - if self-built from source, please state the branch and commit
> (`git log -1 --oneline`), and the used configure options.

### Additional context

> Add any other context about the problem here, e.g. log messages printed by
> `bluealsad` and/or client application.
>
> Please delete instructions prefixed with '>' to prove you have read them.
