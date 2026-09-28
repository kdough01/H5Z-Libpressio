#!/bin/bash
# ============================================================================
# build_hdf5_blob.sh  --  build + install the blob-branch HDF5 H5Zcomp needs
#
#   bash scripts/build_hdf5_blob.sh [PREFIX] [REF]
#
#   PREFIX  install prefix          (default: $HOME/sw/hdf5-blob-<ref>)
#   REF     brtnfld/hdf5 commit/ref (default: e77c5db26d -- the commit
#           H5Z_comp.cc was developed against on branch blobComp)
#
# Only needs READ access to https://github.com/brtnfld/hdf5. Run it once on a
# login node; then configure this repo with -DHDF5_ROOT=<PREFIX>.
#
# Notes
#   - HDF5 on this branch requires CMake >= 3.26. If `cmake --version` is
#     older, `module load cmake` or `pip install --user cmake`.
#   - Later commits on `blob` changed the H5Z_class3_t layout (init/term +
#     a `state` argument to filter2). H5Z_comp.cc builds against both; the
#     configure step prints which layout it found.
#   - Tools (h5dump, h5repack, ...) are built so you can inspect files and
#     test plugin loading from the command line.
#   - zlib is only needed for bench_comp_timing --deflate. If configure
#     cannot find it: HDF5_ZLIB=OFF bash scripts/build_hdf5_blob.sh
# ============================================================================
set -euo pipefail

REF="${2:-e77c5db26d}"
PREFIX="${1:-$HOME/sw/hdf5-blob-${REF:0:7}}"
SRC="${HDF5_BLOB_SRC:-$HOME/src/hdf5-blob}"
JOBS="${JOBS:-8}"

echo "ref    : $REF"
echo "src    : $SRC"
echo "prefix : $PREFIX"

need=3.26
have=$(cmake --version | awk 'NR==1{print $3}')
if [ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | head -1)" != "$need" ]; then
    echo "ERROR: cmake $have < $need (module load cmake, or pip install --user cmake)"; exit 1
fi

if [ ! -d "$SRC/.git" ]; then
    git clone https://github.com/brtnfld/hdf5.git "$SRC"
fi
git -C "$SRC" fetch origin blob
git -C "$SRC" checkout --detach "$REF"
echo "HEAD   : $(git -C "$SRC" log --oneline -1)"

BUILD="$SRC/build-${REF:0:7}"
cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TESTING=OFF \
    -DHDF5_BUILD_EXAMPLES=OFF \
    -DHDF5_BUILD_FORTRAN=OFF \
    -DHDF5_BUILD_JAVA=OFF \
    -DHDF5_BUILD_CPP_LIB=OFF \
    -DHDF5_BUILD_HL_LIB=OFF \
    -DHDF5_BUILD_TOOLS=ON \
    -DHDF5_ENABLE_ZLIB_SUPPORT="${HDF5_ZLIB:-ON}" \
    -DHDF5_ENABLE_SZIP_SUPPORT=OFF

cmake --build "$BUILD" -j "$JOBS"
cmake --install "$BUILD"

# Record exactly what was built next to it.
git -C "$SRC" log -1 --format='%H %s' > "$PREFIX/HDF5_BLOB_COMMIT"
echo
echo "installed: $PREFIX  ($(cat "$PREFIX/HDF5_BLOB_COMMIT"))"
echo "next:      cmake -S . -B build -DHDF5_ROOT=$PREFIX -DCMAKE_PREFIX_PATH=<libpressio view>"
