# VOL vs H5Zcomp filter

Inputs:

- chunk: results/final, results/vs_vol_60172/, results/vs_vol_60177/, results/vs_vol_60178/, results/vs_vol_60179/
- direct: results/vs_vol_60179/
- group: results/vs_vol_60177/, results/vs_vol_60178/
- prog_write: results/vs_vol_60178/
- vol: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/20260811_140354_52888, /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/device_resident_tests
- VOL progressive: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/prog_20260818_140442_53500
- VOL progressive: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/prog_20260818_134437_53492
- VOL progressive: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/prog_20260818_131707_53488

Times are wall clock; write = create+write+flush+sync+close+csync, read = open+read (cold cache). Reps averaged.

## 1. Ratio parity (N=1)

78 pairs; median |filter/VOL - 1| = 0.00%, worst 0.03%.

| path | dataset | codec | VOL ratio | filter ratio |
|---|---|---|---|---|
| chunk | s3d | sz3_1e3 | 19195.33 | 19201.23 |
| chunk | ocean_temp | zfp_gpu_r4 | 7.56 | 7.56 |
| chunk | ocean_temp | zfp_cpu_r4 | 7.56 | 7.56 |
| chunk | miranda | sz3_1e3 | 164.57 | 164.57 |
| chunk | scale-T | sz3_1e3 | 254.02 | 254.03 |

## 2. Filter / VOL time (geometric mean over datasets)

| N | codec | write chunk | write direct | read chunk | read direct |
|---|---|---|---|---|---|
| 1 | cuszp_1e3 | 2.22 | - | 7.74 | - |
| 1 | cuszp_1e6 | 1.99 | - | 5.12 | - |
| 1 | noop | 2.39 | - | 2.02 | - |
| 1 | sz3_1e3 | 1.92 | - | 1.49 | - |
| 1 | sz3_1e6 | 1.65 | - | 1.20 | - |
| 1 | szx_1e3 | 1.50 | - | 1.26 | - |
| 1 | szx_1e6 | 1.51 | - | 1.28 | - |
| 1 | zfp_cpu_r12 | 1.41 | - | 1.31 | - |
| 1 | zfp_cpu_r16 | 1.42 | - | 1.26 | - |
| 1 | zfp_cpu_r4 | 1.44 | - | 1.37 | - |
| 1 | zfp_cpu_r8 | 1.41 | - | 1.35 | - |
| 1 | zfp_gpu_r12 | 2.42 | - | 2.91 | - |
| 1 | zfp_gpu_r16 | 2.69 | - | 2.40 | - |
| 1 | zfp_gpu_r4 | 2.42 | - | 4.17 | - |
| 1 | zfp_gpu_r8 | 2.42 | - | 3.27 | - |
| 8 | cuszp_1e3 | 1.52 | 1.11 | 5.99 | 2.07 |
| 8 | noop | 2.31 | 1.39 | 1.73 | 0.42 |
| 8 | sz3_1e3 | 1.26 | 1.13 | 1.52 | 1.08 |
| 8 | sz3_1e6 | 1.23 | 1.15 | 1.20 | 1.07 |
| 8 | szx_1e3 | 1.13 | 0.98 | 1.61 | 0.97 |
| 8 | szx_1e6 | 1.30 | 0.96 | 1.55 | 1.09 |
| 8 | zfp_cpu_r12 | 1.19 | 1.03 | 1.08 | 0.98 |
| 8 | zfp_cpu_r16 | 1.19 | 1.23 | 1.18 | 1.08 |
| 8 | zfp_cpu_r4 | 1.23 | 1.07 | 1.08 | 0.89 |
| 8 | zfp_cpu_r8 | 1.12 | 1.02 | 1.28 | 0.95 |
| 8 | zfp_gpu_r4 | 3.46 | 1.25 | 4.12 | 0.81 |

## 3. Decoupling: ratio vs number of storage chunks

**sz3_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 164.57 | 164.57 | 164.56 |
| miranda | 2 | - | 162.29 | 164.56 |
| miranda | 4 | - | 112.72 | 164.54 |
| miranda | 8 | 159.59 | 159.59 | 164.51 |
| miranda | 16 | 147.11 | 147.12 | 164.44 |
| miranda | 32 | - | 138.43 | 164.32 |
| s3d | 1 | 19195.33 | 19201.23 | 19168.84 |
| s3d | 2 | - | 18962.38 | 19136.56 |
| s3d | 4 | - | 17239.30 | 19072.32 |
| s3d | 8 | 13524.66 | 9539.71 | 18945.13 |
| s3d | 16 | 5274.76 | 4752.81 | 18695.78 |
| s3d | 32 | - | 3120.48 | 18216.26 |
| scale-T | 1 | 254.02 | 254.03 | 254.02 |
| scale-T | 2 | - | 244.74 | 254.01 |
| scale-T | 4 | - | 179.96 | 253.99 |
| scale-T | 8 | 216.88 | 170.76 | 253.95 |
| scale-T | 16 | 178.46 | 178.47 | 253.88 |
| scale-T | 32 | - | 108.42 | 253.77 |
| nyx_baryon | 1 | 7.85 | 7.85 | 7.85 |
| nyx_baryon | 2 | - | 8.08 | 7.85 |
| nyx_baryon | 4 | - | 8.08 | 7.85 |
| nyx_baryon | 8 | 7.87 | 7.87 | 7.85 |
| nyx_baryon | 16 | 7.91 | 7.91 | 7.85 |
| nyx_baryon | 32 | - | 7.80 | 7.85 |
| ocean_temp | 1 | 6.49 | 6.49 | 6.49 |
| ocean_temp | 2 | - | 6.44 | 6.49 |
| ocean_temp | 4 | - | 5.94 | 6.49 |
| ocean_temp | 8 | 5.85 | 5.85 | 6.49 |
| ocean_temp | 16 | 5.75 | 5.75 | 6.49 |
| ocean_temp | 32 | - | 6.21 | 6.49 |
| einspline37 | 1 | 3.38 | 3.38 | 3.38 |
| einspline37 | 2 | - | - | 3.38 |
| einspline37 | 4 | - | 3.38 | 3.38 |
| einspline37 | 8 | 3.37 | 3.37 | 3.38 |
| einspline37 | 16 | 3.38 | 3.38 | 3.38 |
| einspline37 | 32 | - | 3.37 | 3.38 |

**szx_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 21.51 | 21.51 | 21.51 |
| miranda | 2 | - | 21.51 | 21.51 |
| miranda | 4 | - | 21.51 | 21.51 |
| miranda | 8 | 21.51 | 21.51 | 21.51 |
| miranda | 16 | 21.51 | 21.51 | 21.51 |
| miranda | 32 | - | 21.51 | 21.51 |
| s3d | 1 | 26.12 | 26.12 | 26.12 |
| s3d | 2 | - | 26.12 | 26.12 |
| s3d | 4 | - | 26.12 | 26.12 |
| s3d | 8 | 26.12 | 26.10 | 26.12 |
| s3d | 16 | 26.12 | 26.05 | 26.12 |
| s3d | 32 | - | 26.05 | 26.12 |
| scale-T | 1 | 5.33 | 5.33 | 5.33 |
| scale-T | 2 | - | 5.33 | 5.33 |
| scale-T | 4 | - | 5.32 | 5.33 |
| scale-T | 8 | 5.33 | 5.31 | 5.33 |
| scale-T | 16 | 5.33 | 5.33 | 5.33 |
| scale-T | 32 | - | 5.32 | 5.33 |
| nyx_baryon | 1 | 2.33 | 2.33 | 2.33 |
| nyx_baryon | 2 | - | 2.33 | 2.33 |
| nyx_baryon | 4 | - | 2.33 | 2.33 |
| nyx_baryon | 8 | 2.33 | 2.33 | 2.33 |
| nyx_baryon | 16 | 2.33 | 2.33 | 2.33 |
| nyx_baryon | 32 | - | 2.33 | 2.33 |
| ocean_temp | 1 | 2.35 | 2.35 | 2.35 |
| ocean_temp | 2 | - | 2.35 | 2.35 |
| ocean_temp | 4 | - | 2.35 | 2.35 |
| ocean_temp | 8 | 2.35 | 2.35 | 2.35 |
| ocean_temp | 16 | 2.35 | 2.35 | 2.35 |
| ocean_temp | 32 | - | 2.35 | 2.35 |
| einspline37 | 1 | 1.60 | - | - |
| einspline37 | 2 | - | - | 1.60 |
| einspline37 | 4 | - | 1.60 | 1.60 |
| einspline37 | 8 | 1.60 | 1.60 | 1.60 |
| einspline37 | 16 | 1.60 | 1.60 | 1.60 |
| einspline37 | 32 | - | 1.60 | 1.60 |

**zfp_cpu_r8**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 8.00 | 8.00 | 8.00 |
| miranda | 2 | - | 8.00 | 8.00 |
| miranda | 4 | - | 8.00 | 8.00 |
| miranda | 8 | 8.00 | 8.00 | 8.00 |
| miranda | 16 | 8.00 | 8.00 | 8.00 |
| miranda | 32 | - | 8.00 | 8.00 |
| s3d | 1 | 8.00 | 8.00 | 8.00 |
| s3d | 2 | - | 7.94 | 8.00 |
| s3d | 4 | - | 7.81 | 8.00 |
| s3d | 8 | 7.87 | 7.81 | 8.00 |
| s3d | 16 | 8.00 | 7.81 | 8.00 |
| s3d | 32 | - | 7.81 | 8.00 |
| scale-T | 1 | 3.92 | 3.92 | 3.92 |
| scale-T | 2 | - | 3.77 | 3.92 |
| scale-T | 4 | - | 3.50 | 3.92 |
| scale-T | 8 | 3.27 | 3.06 | 3.92 |
| scale-T | 16 | 3.50 | 3.50 | 3.92 |
| scale-T | 32 | - | 3.92 | 3.92 |
| nyx_baryon | 1 | 4.00 | 4.00 | 4.00 |
| nyx_baryon | 2 | - | 4.00 | 4.00 |
| nyx_baryon | 4 | - | 4.00 | 4.00 |
| nyx_baryon | 8 | 4.00 | 4.00 | 4.00 |
| nyx_baryon | 16 | 4.00 | 4.00 | 4.00 |
| nyx_baryon | 32 | - | 4.00 | 4.00 |
| ocean_temp | 1 | 3.78 | 3.78 | 3.78 |
| ocean_temp | 2 | - | 3.78 | 3.78 |
| ocean_temp | 4 | - | 2.83 | 3.78 |
| ocean_temp | 8 | 2.83 | 2.83 | 3.78 |
| ocean_temp | 16 | 1.89 | 1.89 | 3.78 |
| ocean_temp | 32 | - | 3.40 | 3.78 |
| einspline37 | 1 | 4.00 | 4.00 | 4.00 |
| einspline37 | 2 | - | - | 4.00 |
| einspline37 | 4 | - | 4.00 | 4.00 |
| einspline37 | 8 | 4.00 | 4.00 | 4.00 |
| einspline37 | 16 | 4.00 | 4.00 | 4.00 |
| einspline37 | 32 | - | 4.00 | 4.00 |

**cuszp_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 21.06 | 21.06 | 21.06 |
| miranda | 2 | - | 21.06 | 21.06 |
| miranda | 4 | - | 21.06 | 21.06 |
| miranda | 8 | 21.06 | 21.06 | 21.06 |
| miranda | 16 | 21.06 | 21.06 | 21.06 |
| miranda | 32 | - | 21.06 | 21.06 |
| s3d | 1 | 46.22 | 46.22 | 46.22 |
| s3d | 2 | - | 46.22 | 46.22 |
| s3d | 4 | - | 46.22 | 46.22 |
| s3d | 8 | 46.21 | 46.15 | 46.22 |
| s3d | 16 | 46.19 | 45.99 | 46.22 |
| s3d | 32 | - | 45.96 | 46.21 |
| scale-T | 1 | 12.45 | 12.45 | 12.45 |
| scale-T | 2 | - | 12.45 | 12.45 |
| scale-T | 4 | - | 12.42 | 12.45 |
| scale-T | 8 | 12.45 | 12.37 | 12.45 |
| scale-T | 16 | 12.45 | 12.45 | 12.45 |
| scale-T | 32 | - | 12.42 | 12.45 |
| nyx_baryon | 1 | 4.29 | 4.29 | 4.29 |
| nyx_baryon | 2 | - | 4.29 | 4.29 |
| nyx_baryon | 4 | - | 4.29 | 4.29 |
| nyx_baryon | 8 | 4.29 | 4.29 | 4.29 |
| nyx_baryon | 16 | 4.29 | 4.29 | 4.29 |
| nyx_baryon | 32 | - | 4.29 | 4.29 |
| ocean_temp | 1 | 3.71 | 3.71 | 3.71 |
| ocean_temp | 2 | - | 3.71 | 3.71 |
| ocean_temp | 4 | - | 3.71 | 3.71 |
| ocean_temp | 8 | 3.71 | 3.71 | 3.71 |
| ocean_temp | 16 | 3.71 | 3.71 | 3.71 |
| ocean_temp | 32 | - | 3.71 | 3.71 |
| einspline37 | 1 | 5.26 | - | 5.26 |
| einspline37 | 2 | - | - | 5.26 |
| einspline37 | 4 | - | 3.37 | 5.26 |
| einspline37 | 8 | 3.37 | 3.37 | 5.26 |
| einspline37 | 16 | 3.37 | 3.37 | 5.26 |
| einspline37 | 32 | - | 3.37 | 5.26 |

## 4. Residency arms: end-to-end write time (ms)

| codec | dataset | arm | VOL | filter | filter / VOL |
|---|---|---|---|---|---|
| cuszp_1e3 | miranda | devres | 231.0 | 368.2 | 1.59 |
| cuszp_1e3 | miranda | rtrip | 505.4 | 823.9 | 1.63 |
| cuszp_1e3 | miranda | hostsim | 314.6 | 624.4 | 1.98 |
| cuszp_1e3 | s3d | devres | 358.2 | 893.1 | 2.49 |
| cuszp_1e3 | s3d | rtrip | 1024.3 | 2063.7 | 2.01 |
| cuszp_1e3 | s3d | hostsim | 485.4 | 1915.3 | 3.95 |
| cuszp_1e3 | scale-T | devres | 374.7 | 641.0 | 1.71 |
| cuszp_1e3 | scale-T | rtrip | 744.6 | 1280.7 | 1.72 |
| cuszp_1e3 | scale-T | hostsim | 550.9 | 1197.0 | 2.17 |
| cuszp_1e3 | nyx_baryon | devres | 511.0 | 671.1 | 1.31 |
| cuszp_1e3 | nyx_baryon | rtrip | 950.1 | 1554.3 | 1.64 |
| cuszp_1e3 | nyx_baryon | hostsim | 569.6 | 1259.8 | 2.21 |
| cuszp_1e3 | ocean_temp | devres | 372.3 | 352.3 | 0.95 |
| cuszp_1e3 | ocean_temp | rtrip | 472.9 | 510.1 | 1.08 |
| cuszp_1e3 | ocean_temp | hostsim | 356.9 | 514.2 | 1.44 |
| cuszp_1e3 | einspline37 | hostsim | 4703.8 | 6884.4 | 1.46 |
| cuszp_1e6 | miranda | devres | 401.9 | 520.3 | 1.29 |
| cuszp_1e6 | miranda | rtrip | 629.6 | 808.5 | 1.28 |
| cuszp_1e6 | miranda | hostsim | 463.3 | 723.6 | 1.56 |
| cuszp_1e6 | s3d | devres | 441.0 | 950.7 | 2.16 |
| cuszp_1e6 | s3d | rtrip | 1167.9 | 2235.4 | 1.91 |
| cuszp_1e6 | s3d | hostsim | 559.6 | 1838.9 | 3.29 |
| cuszp_1e6 | scale-T | devres | 513.8 | 767.4 | 1.49 |
| cuszp_1e6 | scale-T | rtrip | 953.3 | 1679.5 | 1.76 |
| cuszp_1e6 | scale-T | hostsim | 553.3 | 1323.6 | 2.39 |
| cuszp_1e6 | nyx_baryon | devres | 710.5 | 814.3 | 1.15 |
| cuszp_1e6 | nyx_baryon | rtrip | 1074.6 | 1830.4 | 1.70 |
| cuszp_1e6 | nyx_baryon | hostsim | 813.0 | 1502.9 | 1.85 |
| cuszp_1e6 | ocean_temp | devres | 428.4 | 655.9 | 1.53 |
| cuszp_1e6 | ocean_temp | rtrip | 547.0 | 572.9 | 1.05 |
| cuszp_1e6 | ocean_temp | hostsim | 452.1 | 626.9 | 1.39 |
| zfp_gpu_r12 | miranda | devres | 394.5 | 659.4 | 1.67 |
| zfp_gpu_r12 | miranda | rtrip | 605.2 | 1006.6 | 1.66 |
| zfp_gpu_r12 | miranda | hostsim | 407.7 | 925.6 | 2.27 |
| zfp_gpu_r12 | s3d | devres | 972.6 | 1656.1 | 1.70 |
| zfp_gpu_r12 | s3d | rtrip | 1260.2 | 2983.0 | 2.37 |
| zfp_gpu_r12 | s3d | hostsim | 726.7 | 2444.4 | 3.36 |
| zfp_gpu_r12 | scale-T | devres | 1143.2 | 1337.0 | 1.17 |
| zfp_gpu_r12 | scale-T | rtrip | 1092.8 | 2145.6 | 1.96 |
| zfp_gpu_r12 | scale-T | hostsim | 777.4 | 1872.5 | 2.41 |
| zfp_gpu_r12 | nyx_baryon | devres | 693.3 | 1139.7 | 1.64 |
| zfp_gpu_r12 | nyx_baryon | rtrip | 933.9 | 1903.1 | 2.04 |
| zfp_gpu_r12 | nyx_baryon | hostsim | 706.4 | 1841.4 | 2.61 |
| zfp_gpu_r12 | ocean_temp | devres | 351.7 | 464.7 | 1.32 |
| zfp_gpu_r12 | ocean_temp | rtrip | 562.0 | 767.5 | 1.37 |
| zfp_gpu_r12 | ocean_temp | hostsim | 374.4 | 651.3 | 1.74 |
| zfp_gpu_r16 | miranda | devres | 758.6 | 645.4 | 0.85 |
| zfp_gpu_r16 | miranda | rtrip | 696.2 | 1136.0 | 1.63 |
| zfp_gpu_r16 | miranda | hostsim | 462.9 | 1055.3 | 2.28 |
| zfp_gpu_r16 | s3d | devres | 699.4 | 1863.5 | 2.66 |
| zfp_gpu_r16 | s3d | rtrip | 1322.7 | 2958.9 | 2.24 |
| zfp_gpu_r16 | s3d | hostsim | 814.9 | 2727.2 | 3.35 |
| zfp_gpu_r16 | scale-T | devres | 635.2 | 1464.4 | 2.31 |
| zfp_gpu_r16 | scale-T | rtrip | 1016.9 | 2297.5 | 2.26 |
| zfp_gpu_r16 | scale-T | hostsim | 718.0 | 2434.1 | 3.39 |
| zfp_gpu_r16 | nyx_baryon | devres | 600.1 | 1348.0 | 2.25 |
| zfp_gpu_r16 | nyx_baryon | rtrip | 989.6 | 2274.5 | 2.30 |
| zfp_gpu_r16 | nyx_baryon | hostsim | 668.9 | 2009.3 | 3.00 |
| zfp_gpu_r16 | ocean_temp | devres | 408.1 | 505.6 | 1.24 |
| zfp_gpu_r16 | ocean_temp | rtrip | 522.0 | 695.7 | 1.33 |
| zfp_gpu_r16 | ocean_temp | hostsim | 404.4 | 730.8 | 1.81 |
| zfp_gpu_r4 | miranda | devres | 347.2 | 515.0 | 1.48 |
| zfp_gpu_r4 | miranda | rtrip | 572.7 | 935.8 | 1.63 |
| zfp_gpu_r4 | miranda | hostsim | 410.9 | 840.4 | 2.05 |
| zfp_gpu_r4 | s3d | devres | 464.9 | 1122.0 | 2.41 |
| zfp_gpu_r4 | s3d | rtrip | 1431.6 | 2281.4 | 1.59 |
| zfp_gpu_r4 | s3d | hostsim | 540.1 | 1945.6 | 3.60 |
| zfp_gpu_r4 | scale-T | devres | 475.5 | 895.7 | 1.88 |
| zfp_gpu_r4 | scale-T | rtrip | 792.0 | 1818.5 | 2.30 |
| zfp_gpu_r4 | scale-T | hostsim | 471.3 | 1437.9 | 3.05 |
| zfp_gpu_r4 | nyx_baryon | devres | 366.8 | 891.3 | 2.43 |
| zfp_gpu_r4 | nyx_baryon | rtrip | 740.4 | 1525.8 | 2.06 |
| zfp_gpu_r4 | nyx_baryon | hostsim | 430.5 | 1318.0 | 3.06 |
| zfp_gpu_r4 | ocean_temp | devres | 361.6 | 341.5 | 0.94 |
| zfp_gpu_r4 | ocean_temp | rtrip | 428.9 | 502.7 | 1.17 |
| zfp_gpu_r4 | ocean_temp | hostsim | 391.7 | 471.7 | 1.20 |
| zfp_gpu_r8 | miranda | devres | 384.7 | 497.7 | 1.29 |
| zfp_gpu_r8 | miranda | rtrip | 554.9 | 1185.2 | 2.14 |
| zfp_gpu_r8 | miranda | hostsim | 363.7 | 835.4 | 2.30 |
| zfp_gpu_r8 | s3d | devres | 588.0 | 1354.7 | 2.30 |
| zfp_gpu_r8 | s3d | rtrip | 1159.6 | 2535.9 | 2.19 |
| zfp_gpu_r8 | s3d | hostsim | 619.2 | 2127.9 | 3.44 |
| zfp_gpu_r8 | scale-T | devres | 624.1 | 1151.1 | 1.84 |
| zfp_gpu_r8 | scale-T | rtrip | 922.1 | 1850.0 | 2.01 |
| zfp_gpu_r8 | scale-T | hostsim | 645.8 | 1721.8 | 2.67 |
| zfp_gpu_r8 | nyx_baryon | devres | 523.5 | 1066.7 | 2.04 |
| zfp_gpu_r8 | nyx_baryon | rtrip | 875.0 | 1758.1 | 2.01 |
| zfp_gpu_r8 | nyx_baryon | hostsim | 659.4 | 1529.6 | 2.32 |
| zfp_gpu_r8 | ocean_temp | devres | 330.3 | 376.3 | 1.14 |
| zfp_gpu_r8 | ocean_temp | rtrip | 408.0 | 527.8 | 1.29 |
| zfp_gpu_r8 | ocean_temp | hostsim | 343.8 | 583.6 | 1.70 |

## 5. Progressive reads

| dataset | codec | pct | VOL bytes read | filter bytes read | VOL read ms | filter read ms | VOL RMSE | filter RMSE |
|---|---|---|---|---|---|---|---|---|
| s3d | sperr_pwe1e3 | 5 | 10.6% | 5.0% | 2901 | 3713 | 4.157e-04 | 5.104e-04 |
| s3d | sperr_pwe1e3 | 10 | 15.6% | 10.0% | 2924 | 3655 | 2.281e-04 | 2.513e-04 |
| s3d | sperr_pwe1e3 | 25 | 30.6% | 25.0% | 2941 | 3695 | 8.799e-05 | 9.149e-05 |
| s3d | sperr_pwe1e3 | 50 | 55.6% | 50.0% | 2946 | 3731 | 4.638e-05 | 4.638e-05 |
| s3d | sperr_pwe1e3 | 75 | 80.6% | 75.0% | 2966 | 3710 | 3.314e-05 | 3.314e-05 |
| s3d | sperr_pwe1e3 | 100 | 100.0% | 100.0% | 2984 | 3724 | 2.036e-05 | 2.036e-05 |
| s3d | sperr_pwe1e6 | 5 | 5.1% | 5.0% | 3372 | 3815 | 8.305e-06 | 1.154e-05 |
| s3d | sperr_pwe1e6 | 10 | 10.1% | 10.0% | 3397 | 3886 | 3.374e-06 | 3.923e-06 |
| s3d | sperr_pwe1e6 | 25 | 25.1% | 25.0% | 3488 | 3979 | 9.352e-07 | 9.352e-07 |
| s3d | sperr_pwe1e6 | 50 | 50.1% | 50.0% | 3556 | 4113 | 3.243e-07 | 3.243e-07 |
| s3d | sperr_pwe1e6 | 75 | 75.1% | 75.0% | 3612 | 4168 | 2.034e-07 | 2.034e-07 |
| s3d | sperr_pwe1e6 | 100 | 100.0% | 100.0% | 3802 | 4340 | 1.191e-07 | 1.191e-07 |
| scale-T | sperr_pwe1e3 | 5 | 5.3% | 5.1% | 3577 | 4362 | 6.120e-04 | 6.120e-04 |
| scale-T | sperr_pwe1e3 | 10 | 10.3% | 10.2% | 3642 | 4473 | 4.289e-04 | 4.289e-04 |
| scale-T | sperr_pwe1e3 | 25 | 25.3% | 25.5% | 3690 | 4399 | 2.602e-04 | 2.602e-04 |
| scale-T | sperr_pwe1e3 | 50 | 50.3% | 50.0% | 3785 | 4572 | 1.793e-04 | 1.793e-04 |
| scale-T | sperr_pwe1e3 | 75 | 75.3% | 75.5% | 3864 | 4570 | 1.446e-04 | 1.446e-04 |
| scale-T | sperr_pwe1e3 | 100 | 100.0% | 100.0% | 4129 | 4808 | 1.085e-04 | 1.085e-04 |

## Recorded failures (rep=-1)

- chunk: einspline37 bzip2 N=1 hostsim
- chunk: einspline37 cuszp_1e3 N=1 hostsim
- chunk: einspline37 cuszp_1e3 N=2 hostsim
- chunk: einspline37 cuszp_1e6 N=1 hostsim
- chunk: einspline37 cuszp_1e6 N=8 hostsim
- chunk: einspline37 cuszp_1e6 N=16 hostsim
- chunk: einspline37 noop N=1 hostsim
- chunk: einspline37 sz3_1e3 N=1 hostsim
- chunk: einspline37 sz3_1e3 N=2 hostsim
- chunk: einspline37 sz3_1e6 N=1 hostsim
- chunk: einspline37 szx_1e3 N=1 hostsim
- chunk: einspline37 szx_1e3 N=2 hostsim
- chunk: einspline37 szx_1e6 N=1 hostsim
- chunk: einspline37 zfp_1e3 N=1 hostsim
- chunk: einspline37 zfp_1e6 N=1 hostsim
- chunk: einspline37 zfp_cpu_r12 N=1 hostsim
- chunk: einspline37 zfp_cpu_r16 N=1 hostsim
- chunk: einspline37 zfp_cpu_r4 N=1 hostsim
- chunk: einspline37 zfp_cpu_r8 N=1 hostsim
- chunk: einspline37 zfp_cpu_r8 N=2 hostsim
- chunk: einspline37 zfp_gpu_r12 N=1 hostsim
- chunk: einspline37 zfp_gpu_r12 N=8 hostsim
- chunk: einspline37 zfp_gpu_r12 N=16 hostsim
- chunk: einspline37 zfp_gpu_r16 N=1 hostsim
- chunk: einspline37 zfp_gpu_r16 N=8 hostsim
- chunk: einspline37 zfp_gpu_r16 N=16 hostsim
- chunk: einspline37 zfp_gpu_r4 N=1 hostsim
- chunk: einspline37 zfp_gpu_r8 N=1 hostsim
- chunk: einspline37 zfp_gpu_r8 N=8 hostsim
- direct: einspline37 cuszp_1e3 N=1 hostsim
- direct: einspline37 cuszp_1e6 N=1 hostsim
- direct: einspline37 cuszp_1e6 N=8 hostsim
- direct: einspline37 cuszp_1e6 N=16 hostsim
- direct: einspline37 noop N=1 hostsim
- direct: einspline37 sz3_1e3 N=1 hostsim
- direct: einspline37 sz3_1e6 N=1 hostsim
- direct: einspline37 szx_1e3 N=1 hostsim
- direct: einspline37 szx_1e6 N=1 hostsim
- direct: einspline37 zfp_cpu_r12 N=1 hostsim
- direct: einspline37 zfp_cpu_r16 N=1 hostsim
- direct: einspline37 zfp_cpu_r4 N=1 hostsim
- direct: einspline37 zfp_cpu_r8 N=1 hostsim
- direct: einspline37 zfp_gpu_r12 N=1 hostsim
- direct: einspline37 zfp_gpu_r12 N=8 hostsim
- direct: einspline37 zfp_gpu_r12 N=16 hostsim
- direct: einspline37 zfp_gpu_r16 N=1 hostsim
- direct: einspline37 zfp_gpu_r16 N=8 hostsim
- direct: einspline37 zfp_gpu_r16 N=16 hostsim
- direct: einspline37 zfp_gpu_r4 N=1 hostsim
- direct: einspline37 zfp_gpu_r8 N=1 hostsim
- direct: einspline37 zfp_gpu_r8 N=8 hostsim
- group: einspline37 cuszp_1e3 N=4 devres
- group: einspline37 cuszp_1e3 N=4 rtrip
- group: einspline37 cuszp_1e6 N=4 devres
- group: einspline37 cuszp_1e6 N=4 hostsim
- group: einspline37 cuszp_1e6 N=4 rtrip
- group: einspline37 szx_1e3 N=1 hostsim
- group: einspline37 zfp_gpu_r12 N=4 devres
- group: einspline37 zfp_gpu_r12 N=4 hostsim
- group: einspline37 zfp_gpu_r12 N=4 rtrip
- group: einspline37 zfp_gpu_r16 N=4 devres
- group: einspline37 zfp_gpu_r16 N=4 hostsim
- group: einspline37 zfp_gpu_r16 N=4 rtrip
- group: einspline37 zfp_gpu_r4 N=4 devres
- group: einspline37 zfp_gpu_r4 N=4 hostsim
- group: einspline37 zfp_gpu_r4 N=4 rtrip
- group: einspline37 zfp_gpu_r8 N=4 devres
- group: einspline37 zfp_gpu_r8 N=4 hostsim
- group: einspline37 zfp_gpu_r8 N=4 rtrip

