# ============================================================================
# env.sh  --  one place for the cluster paths (source it; don't execute it)
#
#   source scripts/env.sh
#
# Override any of these before sourcing, e.g.  HDF5_BLOB=/other/prefix source ...
# Defaults match the VOL repo's Swing setup (tests/env.sh, *.pbs).
# ============================================================================
module load gcc/11.4.0   2>/dev/null || true
module load cuda/12.6.0  2>/dev/null || true

export LP_VIEW="${LP_VIEW:-/home/kdougherty/libpressio_cuda/.spack-env/view}"
export HDF5_BLOB="${HDF5_BLOB:-$HOME/sw/hdf5-blob-e77c5db}"
export BENCH_DATA_ROOT="${BENCH_DATA_ROOT:-/lcrc/project/ECP-EZ/public/compression}"
export H5Z_SCRATCH="${H5Z_SCRATCH:-/lcrc/project/SDR/$USER/h5zcomp_bench}"

# lib vs lib64 differs between prefixes; resolve rather than guess.
for d in "$LP_VIEW/lib64" "$LP_VIEW/lib"; do
    [ -e "$d/liblibpressio.so" ] && { export LP_LIB="$d"; break; }
done
for d in "$HDF5_BLOB/lib64" "$HDF5_BLOB/lib"; do
    ls "$d"/libhdf5.so* >/dev/null 2>&1 && { export HDF5_BLOB_LIB="$d"; break; }
done

LP_CMAKE_DIR=""
for c in "$LP_VIEW/lib64/cmake/LibPressio" "$LP_VIEW/lib/cmake/LibPressio" \
         "$LP_VIEW/lib64/cmake/libpressio" "$LP_VIEW/lib/cmake/libpressio"; do
    if [ -f "$c/LibPressioConfig.cmake" ] || [ -f "$c/libpressio-config.cmake" ]; then
        LP_CMAKE_DIR="$c"; break
    fi
done
export LP_CMAKE_DIR

GCC_LIB=$(gcc --print-file-name=libstdc++.so | xargs dirname 2>/dev/null)
[ "$GCC_LIB" = "." ] && GCC_LIB=/usr/lib/gcc/x86_64-linux-gnu/11
export GCC_LIB

# The blob HDF5 must win over any spack-view libhdf5 at runtime.
export LD_LIBRARY_PATH="${HDF5_BLOB_LIB:-}:${GCC_LIB}:${LP_LIB:-}:${LD_LIBRARY_PATH:-}"

# The spack view on PATH drags in its own cmake/compilers; the VOL jobs strip it.
export CLEAN_PATH=$(echo "$PATH" | tr ':' '\n' | grep -v spack-env | tr '\n' ':')

echo "LP_VIEW        = $LP_VIEW"
echo "LP_LIB         = ${LP_LIB:-<NOT FOUND>}"
echo "LP_CMAKE_DIR   = ${LP_CMAKE_DIR:-<NOT FOUND>}"
echo "HDF5_BLOB      = $HDF5_BLOB  ($(cat "$HDF5_BLOB/HDF5_BLOB_COMMIT" 2>/dev/null || echo 'no HDF5_BLOB_COMMIT -- run scripts/build_hdf5_blob.sh'))"
echo "HDF5_BLOB_LIB  = ${HDF5_BLOB_LIB:-<NOT FOUND>}"
echo "BENCH_DATA_ROOT= $BENCH_DATA_ROOT"
echo "H5Z_SCRATCH    = $H5Z_SCRATCH"

# configure + build helper:  h5zc_build [builddir]
h5zc_build () {
    local b="${1:-build}"
    env PATH="$CLEAN_PATH" cmake -S . -B "$b" \
        -DHDF5_ROOT="$HDF5_BLOB" \
        -DCMAKE_PREFIX_PATH="$LP_VIEW" \
        ${LP_CMAKE_DIR:+-DLibPressio_DIR="$LP_CMAKE_DIR"} \
        -DBENCH_DATA_ROOT="$BENCH_DATA_ROOT" \
        ${USE_CUDA:+-DUSE_CUDA=ON} -Wno-dev \
      && env PATH="$CLEAN_PATH" cmake --build "$b" -j "${JOBS:-8}"
}
