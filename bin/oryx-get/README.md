# oryx-get

Fetch a package and make it installable, with no Oryx server involved.
Runs entirely on this machine.

```sh
oryx-get <pkg>...            # from [oryx], Debian hurd-i386, or PyPI, in
                              # the order configured in /etc/oryx-get.conf
oryx-get --pypi <pkg>        # pure-Python PyPI wheel specifically
oryx-get --search <term>     # across every configured source
oryx-get --info <pkg>
oryx-get --dry-run <pkg>...
oryx-get --export <dir> --export-name <name> <pkg>...
                              # convert (without installing locally) and
                              # write a servable pacman repo to <dir>
oryx-get --allow-protected <pkg>...
                              # still asks for interactive confirmation;
                              # never bypassable from a script
```

Full design, the trust chain, and everything found building it: see
`FIXES.md` §19 in the main `oryx/` tree. `/etc/oryx-get.conf` is written on
first run (or by `oryx-postinst.sh oryx_get`) if missing.
