# Contributing To BlueALSA

## Code and manual pages

This project welcomes contributions of code, documentation and testing.

To submit code or manual page contributions please use GitHub Pull Requests.
The GitHub source code repository is at [https://github.com/arkq/bluez-alsa](https://github.com/arkq/bluez-alsa)

For help with creating a Pull Request (PR), please consult the GitHub
documentation. In particular:

* [creating forks](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/working-with-forks/about-forks)

* [creating pull requests](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request)

The commit message for each commit that you make to your branch should include
a clear description of the change introduced by that commit. That will make the
change history log easier to follow when the PR is merged.

If the PR is for a new feature, extensive change or non-trivial bug fix please
if possible add a simple unit test. If that is not possible then please include
in the PR description instructions on how to test the code manually.

Before submitting a pull request, if possible please configure your build with
`--enable-test`; and to catch as many coding errors as possible please compile
with:

```sh
make CFLAGS="-Wall -Wextra -Wshadow -Werror"
```

and then run the unit test suite:

```sh
make check
```

When submitting the PR, please provide a description of the problem and its
fix, or the new feature and its rationale. Help the reviewer understand what to
expect.

If you have an issue number, please reference it with a syntax `Fixes #123`.

If you wish to help by testing PRs or by making review comments please do so by
adding comments to the PR.

## Wiki

The project [wiki](https://github.com/arkq/bluez-alsa/wiki) is "public" and
contributions there are also welcome.
