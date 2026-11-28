#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'Package Wave: %s\n' "$*" >&2
    exit 1
}

if (( $# != 6 )); then
    fail 'Usage: package.sh TAG ARCH BACKEND_BUILD SITE OUTPUT_DIRECTORY SOURCE_URL'
fi

tag=$1
arch=$2
backend_dir=$3
site_dir=$4
output_dir=$5
source_url=$6
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

[[ $tag =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-beta)?$ ]] || fail "Invalid release tag $tag."
case "$arch" in
    x86_64|aarch64) ;;
    *) fail "Unsupported architecture $arch." ;;
esac
[[ $(uname -m) == "$arch" ]] || fail "Package on a native $arch build host."
[[ $source_url =~ ^https://[^[:space:]]+$ ]] || fail 'Provide an HTTPS source archive link.'
version=$("$backend_dir/wave-backend" --version) || fail 'Could not run the built backend.'
[[ "v$version" == "$tag" ]] || fail "Backend version $version differs from tag $tag."

for asset in index.html index.js index.wasm; do
    [[ -s $site_dir/$asset ]] || fail "Missing frontend asset $asset."
done
[[ -s $site_dir/licenses/musl.txt ]] || fail 'Missing the musl copyright notice.'
site_dir=$(realpath -e -- "$site_dir")
unexpected=$(find "$site_dir" -mindepth 1 ! -type f ! -type d -print -quit)
[[ -z $unexpected ]] || fail "Unexpected frontend file type at $unexpected."

mkdir -p -- "$output_dir"
output_dir=$(realpath -e -- "$output_dir")
work_dir=$(mktemp -d -- "$output_dir/.wave-package.XXXXXXXX")
trap 'rm -rf -- "$work_dir"' EXIT

staging="$work_dir/staging"
mkdir -p -- "$staging/site" "$staging/licenses"
install -m 0755 -- "$backend_dir/wave-backend" "$staging/wave-backend"
install -m 0755 -- "$repo_dir/scripts/manage.sh" "$staging/wave-manager"
install -m 0644 -- "$repo_dir/LICENSE" "$staging/LICENSE"
for notice in argon2 cpp-httplib miniz utf8proc zxcvbn-c; do
    install -m 0644 -- "$backend_dir/licenses/$notice.txt" "$staging/licenses/$notice.txt"
done
cp -R -- "$site_dir/." "$staging/site/"
printf 'Wave %s for Linux %s\n\nCorresponding Wave source code\n%s\n' \
    "$tag" "$arch" "$source_url" > "$staging/SOURCE.txt"

archive="wave-$tag-linux-$arch.zip"
(
    cd -- "$staging"
    zip -q -r -X "../$archive" LICENSE SOURCE.txt licenses site wave-backend wave-manager
)
mv -fT -- "$work_dir/$archive" "$output_dir/$archive"
printf '%s\n' "$output_dir/$archive"
