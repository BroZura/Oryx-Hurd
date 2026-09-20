# /etc/oryx-get.conf parsing.
#
# INI, via configparser, matching the rest of Oryx's config-file style
# (pacman.conf, i2pd.conf). One [source:NAME] section per source, tried in
# the order they appear in the file (NOT alphabetically -- configparser
# preserves insertion order, which we rely on). Adding a new source type
# means adding a resolver in sources.py and a new [source:...] section here,
# never touching oryx-get's own code path.

from __future__ import annotations

import configparser
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_CONF_PATH = Path("/etc/oryx-get.conf")

DEFAULT_CONF = """\
# /etc/oryx-get.conf -- oryx-get source and policy configuration.
#
# Sources are tried in the order they appear below. Add a new one by adding
# a [source:NAME] section -- oryx-get's own code never needs to change.

[source:oryx]
type = repo
# Matches [oryx] in /usr/local/etc/pacman.conf. If a package is already
# resolvable there, oryx-get just runs `pacman -S` -- no conversion needed.

[source:debian]
type = debian
url = http://deb.debian.org/debian-ports
suite = unstable
component = main
arch = hurd-i386
keyring = /usr/share/keyrings/debian-ports-archive-keyring.gpg
# gpgv + this keyring must both be installed (pacman -S gpgv debian-ports-archive-keyring)
# before this source can be trusted. oryx-get refuses to use it otherwise --
# see the trust note in FIXES.md's oryx-get section. Never disable
# verification to work around that; fix the missing keyring/gpgv instead.

[source:pypi]
type = pypi
url = https://pypi.org
# Pure-Python (py3-none-any) wheels only -- anything needing a compiled
# extension has no way to build on hurd-i386 without a cross toolchain
# (see linux-personality/README.md: the emulator itself had to be compiled
# ON the target for exactly this reason). oryx-get will refuse a wheel that
# is not py3-none-any rather than install something that will not import.

[protected]
# Refused by default. --allow-protected AND an interactive confirmation are
# both required to install or upgrade any package in this list, or anything
# that would pull one in as a dependency. See FIXES.md 14 (the sshd
# lockout) and the T400 remote-access rule for why sshd/glibc/pfinet/netdde
# are here -- this list exists because of a real incident, not caution for
# its own sake.
packages =
    glibc
    libc6
    libc0.3
    hurd
    hurd-libs0.3
    hurd-dev
    gnumach
    gnumach-1.8-486-smp
    pfinet
    netdde
    openssh
    openssh-server
    openssh-client
    openssh-common
    grub
    grub-common
    grub-pc
    mig
    pacman

[cache]
dir = /var/cache/oryx-get
log = /var/cache/oryx-get/oryx-get.log
"""


@dataclass
class SourceConfig:
    name: str
    type: str
    options: dict[str, str] = field(default_factory=dict)


@dataclass
class OryxGetConfig:
    sources: list[SourceConfig]
    protected: set[str]
    cache_dir: Path
    log_path: Path
    raw: configparser.ConfigParser


def ensure_default(path: Path = DEFAULT_CONF_PATH) -> None:
    if not path.exists():
        path.write_text(DEFAULT_CONF)


def load(path: Path = DEFAULT_CONF_PATH) -> OryxGetConfig:
    ensure_default(path)
    cp = configparser.ConfigParser()
    cp.read(path)

    sources = []
    for section in cp.sections():
        if section.startswith("source:"):
            name = section.split(":", 1)[1]
            opts = dict(cp.items(section))
            stype = opts.pop("type", "")
            sources.append(SourceConfig(name=name, type=stype, options=opts))

    protected = set()
    if cp.has_section("protected"):
        raw = cp.get("protected", "packages", fallback="")
        protected = {p.strip() for p in raw.splitlines() if p.strip()}

    cache_dir = Path(cp.get("cache", "dir", fallback="/var/cache/oryx-get"))
    log_path = Path(cp.get("cache", "log", fallback=str(cache_dir / "oryx-get.log")))

    return OryxGetConfig(sources=sources, protected=protected,
                          cache_dir=cache_dir, log_path=log_path, raw=cp)
