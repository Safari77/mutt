#!/bin/sh

# Switch to directory where this script lives so that further commands are run
# from the root directory of the source.  The script path and srcdir are double
# quoted to allow the space character to appear in the path.
srcdir=$(dirname "$0") && cd "$srcdir" || exit 1

# Ensure that we have a repo here.
# If not, just cat the VERSION file; it contains the latest release number.
{ [ -r ".git" ] && command -v git >/dev/null 2>&1; } \
|| exec cat VERSION

rel_suffix="-rel"

latesttag=$(git describe --tags --match "mutt-*${rel_suffix}" --abbrev=0 2>/dev/null) \
  && [ -n "$latesttag" ] \
  || exec cat VERSION

version=$(printf '%s\n' "$latesttag" | sed -e "s/^mutt-//" -e "s/${rel_suffix}$//" -e "s/-/./g")

distance=$(git rev-list --count "${latesttag}.." 2>/dev/null) \
  && [ -n "$distance" ] \
  || exec cat VERSION

commitid=$(git rev-parse --short HEAD 2>/dev/null) \
  && [ -n "$commitid" ] \
  || exec cat VERSION

if [ 0 -eq "$distance" ]; then
  distance=
else
  distance="+$distance"
fi

echo "${version}${distance} (${commitid})"
