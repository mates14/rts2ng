#!/bin/bash
set -e

GPG_CONF=~/.gnupg-repo.conf
test -e $GPG_CONF && source $GPG_CONF
# GNUPGHOME=~/.gnupg-repo
# REPO_GPG_KEY=something

usage() {
    cat <<USAGE
usage: $0 [-c] [-r] [-d] [-s] [module ...]
  (none)  build in <module>/build; configures only if that has no cache yet,
          so choices made in ccmake survive between runs
  -c      open ccmake on each module's build dir before building
  -r      start over: ask for the SDK paths again (previous answers
          prefilled) and wipe the build dirs
  -d      build .deb packages
  -s      build source packages
modules: base db web gui python (default: all)
USAGE
}

build_deb=0
build_src=0
run_ccmake=0
reconfigure=0
while getopts "dscrh" optn; do
case $optn in
    d)
        build_deb=1;
        ;;
    s)
        build_src=1;
        ;;
    c)
        run_ccmake=1;
        ;;
    r)
        reconfigure=1;
        ;;
    *)
        usage
        exit 1
        ;;
esac
done
shift $(($OPTIND - 1))

# build everything by default, but limit to a particular module if listed on the command line
modules="base db web gui python"
[ $# -gt 0 ] && modules="$*"

# sudo apt install libgsl-dev qtbase5-dev
# sudo apt install build-essential debhelper devscripts dpkg-dev

# Vendor SDK locations - one site file per machine, read with `cmake -C`
# by every tree's first configure (db, web and gui each nest base in their
# own build dir, so a path set in one tree's ccmake would not reach the
# others) and exported for base/debian/rules. The paths used to be
# hardcoded exports here, and went stale silently: a wrong path only
# skipped the driver, with a STATUS line among a hundred others to say so.
#
# Now each SDK is looked for with locate, the best candidate is proposed,
# and the path is used only once confirmed (or typed in, if nothing was
# found). Asked once per machine; -r asks again. Without a terminal
# (cron, dpkg-buildpackage from another script) nothing is asked: the site
# file is used as is, falling back to BASE_*_SDK_DIR from the environment.
#
# The two SDKs have different layouts: Andor wants an SDK root with
# include/ and lib/, paracl the source dir with libmks3.c. A wrong tree
# once silently produced an empty driver package - hence the layout check
# below before a path is accepted.
#
# libfli and libgxccd are not asked for: they are bundled in
# base/external/fli and base/external/gxccd (gxccd for x86-64 only). A
# BASE_FLI_SDK_DIR/BASE_GXCCD_SDK_DIR still in an older site file
# overrides the bundled copy; delete that line to go back to it.
SITE="${RTS2NG_SITE:-$HOME/.config/rts2ng/site.cmake}"

# Prints the file that makes <dir> a usable <sdk> tree (its library, or
# libmks3.c for paracl); fails if the tree is incomplete. The library also
# has to be for this machine - several old trees around are 32-bit builds.
sdk_key_file() {
    local sdk=$1 d=$2 f
    case $sdk in
        ANDOR)  [ -f "$d/include/atmcdLXd.h" ] && f=$(ls "$d"/lib/libandor*x86_64.so* 2>/dev/null | head -n 1) ;;
        PARACL) [ -f "$d/libmks3.h" ] && f="$d/libmks3.c" ;;
    esac
    [ -n "$f" ] && [ -f "$f" ] || return 1
    case $f in
        *.c) ;;
        *)  if [ "$(uname -m)" = x86_64 ] && command -v objdump >/dev/null; then
                objdump -f "$f" 2>/dev/null | grep -q 'architecture: i386:x86-64' || return 1
            fi ;;
    esac
    echo "$f"
}

# Candidate trees for <sdk>, best first: those under $HOME before other
# users' (which may vanish on a shared machine), then newest library.
sdk_candidates() {
    local sdk=$1 probe up=0 f d key
    case $sdk in
        ANDOR)  probe=atmcdLXd.h; up=1 ;;
        PARACL) probe=libmks3.c ;;
    esac
    command -v locate >/dev/null || return 0
    locate -b "\\$probe" 2>/dev/null | grep -v -e '/\.svn/' -e '/\.git/' | while IFS= read -r f; do
        d=$(dirname "$f")
        [ $up = 1 ] && d=$(dirname "$d")
        d=$(realpath -q "$d") || continue
        key=$(sdk_key_file $sdk "$d") || continue
        case $d/ in "$HOME"/*) r=0 ;; *) r=1 ;; esac
        printf '%d\t%d\t%s\n' $r "$(stat -L -c %Y "$key")" "$d"
    done | sort -u | sort -t $'\t' -k1,1n -k2,2nr | cut -f3
}

site_has() { [ -f "$SITE" ] && grep -q "^set($1 " "$SITE"; }
site_get() { [ -f "$SITE" ] && sed -n "s/^set($1 \"\(.*\)\" CACHE.*/\1/p" "$SITE"; }
site_put() {
    local tmp
    tmp=$(mktemp)
    [ -f "$SITE" ] && grep -v "^set($1 " "$SITE" > "$tmp"
    printf 'set(%s "%s" CACHE PATH "%s")\n' "$1" "$2" "$3" >> "$tmp"
    mv "$tmp" "$SITE"
}

# ask_sdk <sdk> <cache var> <description>
ask_sdk() {
    local sdk=$1 var=$2 desc=$3 cur="" def ans cands n i
    if site_has $var; then
        [ $reconfigure = 1 ] && [ -t 0 ] || return 0
        cur=$(site_get $var)
    fi
    if [ ! -t 0 ]; then
        # not recorded when empty, so that the next run on a terminal asks
        [ -n "${!var}" ] && site_put $var "${!var}" "$desc"
        return 0
    fi
    mapfile -t cands < <(sdk_candidates $sdk)
    def=${cur:-${!var:-${cands[0]}}}
    echo "==> $desc"
    n=${#cands[@]}
    if [ $n = 0 ]; then
        echo "    nothing found by locate"
    else
        for ((i = 0; i < n && i < 10; i++)); do
            echo "    found: ${cands[$i]}"
        done
        [ $n -gt 10 ] && echo "    ... and $((n - 10)) more"
    fi
    while :; do
        read -e -i "$def" -p "$desc path [empty = skip]: " ans
        ans="${ans/#\~/$HOME}"
        [ -z "$ans" ] && break
        if sdk_key_file $sdk "$ans" >/dev/null; then
            ans=$(realpath "$ans")
            break
        fi
        echo "    $ans is not a usable $sdk tree for this machine, try again"
        def=$ans
    done
    site_put $var "$ans" "$desc"
}

mkdir -p "$(dirname "$SITE")"
[ -f "$SITE" ] || echo "# Written by rts2ng buildme.sh - per-machine settings, read with cmake -C" > "$SITE"
ask_sdk ANDOR  BASE_ANDOR_SDK_DIR "Andor SDK (include/atmcdLXd.h, lib/libandor*)"
ask_sdk PARACL BASE_PARACL_DIR    "paracl source (libmks3.c)"
# An override of a bundled library whose tree is gone would fail the
# configure (the options are ON) instead of falling back to the bundled
# copy - drop it from the site file.
for var in BASE_FLI_SDK_DIR BASE_GXCCD_SDK_DIR; do
    cur=$(site_get $var)
    if [ -n "$cur" ] && [ ! -d "$cur" ]; then
        echo "==> $var: $cur does not exist, removed from $SITE - using the bundled copy"
        tmp=$(mktemp)
        grep -v "^set($var " "$SITE" > "$tmp"
        mv "$tmp" "$SITE"
    fi
done
# base/debian/rules picks these up via `?=`
export BASE_FLI_SDK_DIR="$(site_get BASE_FLI_SDK_DIR)"
export BASE_ANDOR_SDK_DIR="$(site_get BASE_ANDOR_SDK_DIR)"
export BASE_GXCCD_SDK_DIR="$(site_get BASE_GXCCD_SDK_DIR)"
export BASE_PARACL_DIR="$(site_get BASE_PARACL_DIR)"


if [ $build_deb == 0 ] && [ $build_src == 0 ]; then
    for tree in $modules; do
        # python/ (and any future non-CMake tree) has nothing to
        # configure/build here - it only ever gets touched by the -d/-s
        # (dpkg-buildpackage) branches below, via its own debian/rules.
        if [ ! -f "$tree/CMakeLists.txt" ]; then
            echo "==> Skipping cmake build for $tree (no CMakeLists.txt)"
            continue
        fi
        # The cache is kept between runs: options changed in ccmake stay,
        # and cmake --build re-runs configure by itself when a
        # CMakeLists.txt changes. The site file only seeds a fresh cache -
        # it never overrides a value already in one, hence the wipe on -r.
        [ $reconfigure = 1 ] && rm -rf $tree/build
        if [ ! -f $tree/build/CMakeCache.txt ]; then
            cmake -S $tree -B $tree/build -C "$SITE" -DCMAKE_BUILD_TYPE=RelWithDebInfo
        fi
        [ $run_ccmake = 1 ] && ccmake $tree/build
        cmake --build $tree/build -j4
    done
fi

if [ $build_deb == 1 ] || [ $build_src == 1 ]; then
    # Output is a flat apt repository per distribution, dist/<os>/<codename>/
    # (e.g. dist/ubuntu/noble/), rsynced as-is to lascaux once deemed final:
    #   deb [trusted=yes] https://lascaux.asu.cas.cz/m/rts2ng/ubuntu/noble ./
    # Each distribution is built on its own host into its own directory, so
    # they never collide. Everything that is not a .deb (.ddeb, .buildinfo,
    # .changes, source packages) goes to dist/meta/<os>/<codename>/ instead -
    # apt-ftparchive recurses into subdirectories and would index the .ddebs.
    . /etc/os-release
    DIST_DIR="$(pwd)/dist/$ID/$VERSION_CODENAME"
    META_DIR="$(pwd)/dist/meta/$ID/$VERSION_CODENAME"
    mkdir -p "$DIST_DIR" "$META_DIR"

    # Package version = <upstream from debian/changelog>-<YYYYMMDD>~<o><ver>,
    # e.g. 0.1.0-20260917~u2404 or 0.1.0-20260917~d13. The date makes each
    # day's build supersede the previous one for apt; a second build on the
    # same day reuses the version and overwrites the files (install such a one
    # by hand with dpkg -i). The suffix makes a dist-upgraded machine pick up
    # the new release's build instead of keeping the old one as "same version".
    # The changelog entry is temporary - every debian/changelog is restored on
    # exit, so the committed changelogs stay untouched.
    BUILD_VERSION_REV="$(date +%Y%m%d)~${ID:0:1}${VERSION_ID//./}"

    stamp_changelog() {
        local cl="$1/debian/changelog" src ver maint
        src=$(dpkg-parsechangelog -l "$cl" -S Source)
        ver=$(dpkg-parsechangelog -l "$cl" -S Version)
        maint=$(dpkg-parsechangelog -l "$cl" -S Maintainer)
        [ -f "$cl.buildme-orig" ] || cp "$cl" "$cl.buildme-orig"
        {
            printf '%s (%s-%s) %s; urgency=medium\n\n' "$src" "${ver%-*}" "$BUILD_VERSION_REV" "$VERSION_CODENAME"
            printf '  * Automated build by buildme.sh.\n\n'
            printf ' -- %s  %s\n\n' "$maint" "$(date -R)"
            cat "$cl.buildme-orig"
        } > "$cl"
    }
    restore_changelogs() {
        local t
        for t in base db web gui python; do
            if [ -f "$t/debian/changelog.buildme-orig" ]; then
                mv -f "$t/debian/changelog.buildme-orig" "$t/debian/changelog"
            fi
        done
    }
    trap restore_changelogs EXIT
fi

if [ $build_deb == 1 ]; then
    echo "==> Building .deb packages ($BUILD_VERSION_REV)"

    for tree in $modules; do
        stamp_changelog $tree
        ( cd $tree && dpkg-buildpackage -us -uc -b )
        ( cd $tree && dh_clean )	# removes debian/.debhelper, obj-*, etc.
    done

    # dpkg-buildpackage drops its output one level up from the source dir
    # (i.e. here, at the repo root) - collect it into the repo directory,
    # replacing any older build of the same package (a flat repo could hold
    # several versions, but only the newest is ever wanted)
    for f in *.deb; do
        [ -e "$f" ] || continue
        rm -f "$DIST_DIR/${f%%_*}"_*.deb
        mv "$f" "$DIST_DIR/"
    done
    mv *.ddeb *.buildinfo *.changes "$META_DIR/" 2>/dev/null || true

    # apt index: Packages (per-.deb SHA256) and Release (hashes of Packages).
    # Signed InRelease only if REPO_GPG_KEY is set; otherwise clients need
    # [trusted=yes] (or sign on lascaux before publishing).
    echo "==> Indexing $DIST_DIR"
    (
        cd "$DIST_DIR"
        apt-ftparchive packages . > Packages
        gzip -9kf Packages
        rm -f Release InRelease
        apt-ftparchive -o APT::FTPArchive::Release::Origin=rts2ng \
                       -o APT::FTPArchive::Release::Codename="$VERSION_CODENAME" \
                       release . > "$META_DIR/Release"	# not in ., or it hashes itself
        mv -f "$META_DIR/Release" Release
        if [ -n "$REPO_GPG_KEY" ]; then
            gpg --batch --yes --local-user "$REPO_GPG_KEY" --clearsign -o InRelease Release
        fi
    )
fi

if [ $build_src == 1 ]; then
    echo "==> Building source packages"

    for tree in $modules; do
        stamp_changelog $tree
        ( cd $tree && dpkg-buildpackage -us -uc -S )
        ( cd $tree && dh_clean )
    done

    # -S output (the .dsc, orig/debian tarballs, and a source-only
    # .changes distinct from -b's binary .changes) lands in the same place
    # as the binary build's output above
    mv *.dsc *.tar.* "$META_DIR/" 2>/dev/null || true
    mv *.changes "$META_DIR/" 2>/dev/null || true
fi

if [ $build_deb == 1 ] || [ $build_src == 1 ]; then
    echo "==> Packages in $DIST_DIR:"
    ls -la "$DIST_DIR"
fi
