# Licensing

## neomg's own code

neomg is a fork of [`mg`][mg] (OpenBSD Micro Emacs), which is **public domain**.
All of neomg's own source — the C editor, the C++23 modules under `src/magit/`,
the bridge, and the tests — is likewise released into the **public domain**. Do
whatever you like with it.

## Linked dependencies (carry their own licenses)

neomg is *not* a bundle — these are separate libraries it links against at
build/run time. A **binary** you build or distribute is a combined work and
conveys these libraries under their own terms; include their notices if you
redistribute binaries. (On a distro the libraries are separate packages with
their own license metadata, so a source package such as the AUR `PKGBUILD` only
needs to declare neomg's own public-domain license.)

| Dependency | License | Notes |
|------------|---------|-------|
| **libgit2** | GPLv2 **with a linking exception** | The exception explicitly permits linking libgit2 from a program under *any* license and distributing the combined result under your own terms; libgit2's own files remain under its license. So neomg staying public domain is fine. |
| **utf8proc** | MIT / Unicode-style (permissive) | UTF-8 / grapheme handling. |
| **ncurses** (or system curses) | MIT/X11-style (permissive) | Terminal I/O. |

In short: **neomg's code is public domain**; a distributed binary additionally
conveys libgit2 (GPLv2-w/-linking-exception) and the permissively-licensed
utf8proc/ncurses. There is no license conflict — the linking exception is what
makes a public-domain front end over libgit2 legitimate.

[mg]: https://github.com/troglobit/mg
