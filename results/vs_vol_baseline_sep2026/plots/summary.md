# VOL vs H5Zcomp filter

Inputs:

- chunk: results/final (H5Z-Libpressio, Sep 2026 per-chunk sweep)
- vol: vol-external-passthrough/tests/results/20260811_140354_52888, vol-external-passthrough/tests/results/device_resident_tests
- VOL progressive: vol-external-passthrough/tests/results/prog_20260818_140442_53500
- VOL progressive: vol-external-passthrough/tests/results/prog_20260818_134437_53492
- VOL progressive: vol-external-passthrough/tests/results/prog_20260818_131707_53488

Times are wall clock; write = create+write+flush+sync+close+csync, read = open+read (cold cache). Reps averaged.

## 1. Ratio parity (N=1)

78 pairs; median |filter/VOL - 1| = 0.00%, worst 0.03%.

| path | dataset | codec | VOL ratio | filter ratio |
|---|---|---|---|---|
| chunk | s3d | sz3_1e3 | 19195.33 | 19201.23 |
| chunk | ocean_temp | zfp_cpu_r4 | 7.56 | 7.56 |
| chunk | ocean_temp | zfp_gpu_r4 | 7.56 | 7.56 |
| chunk | miranda | sz3_1e3 | 164.57 | 164.57 |
| chunk | scale-T | sz3_1e3 | 254.02 | 254.03 |

## 2. Filter / VOL time (geometric mean over datasets)

| N | codec | write chunk | write direct | read chunk | read direct |
|---|---|---|---|---|---|
| 1 | cuszp_1e3 | 2.79 | - | 7.51 | - |
| 1 | cuszp_1e6 | 2.27 | - | 5.04 | - |
| 1 | noop | 2.55 | - | 2.05 | - |
| 1 | sz3_1e3 | 1.95 | - | 1.47 | - |
| 1 | sz3_1e6 | 1.68 | - | 1.21 | - |
| 1 | szx_1e3 | 1.64 | - | 1.22 | - |
| 1 | szx_1e6 | 1.64 | - | 1.27 | - |
| 1 | zfp_cpu_r12 | 1.46 | - | 1.31 | - |
| 1 | zfp_cpu_r16 | 1.46 | - | 1.26 | - |
| 1 | zfp_cpu_r4 | 1.49 | - | 1.37 | - |
| 1 | zfp_cpu_r8 | 1.47 | - | 1.32 | - |
| 1 | zfp_gpu_r12 | 2.64 | - | 2.93 | - |
| 1 | zfp_gpu_r16 | 2.92 | - | 2.48 | - |
| 1 | zfp_gpu_r4 | 2.76 | - | 4.36 | - |
| 1 | zfp_gpu_r8 | 2.70 | - | 3.35 | - |

## 3. Decoupling: ratio vs number of storage chunks

**sz3_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 164.57 | 164.57 | - |
| miranda | 8 | 159.59 | - | - |
| miranda | 16 | 147.11 | - | - |
| s3d | 1 | 19195.33 | 19201.23 | - |
| s3d | 8 | 13524.66 | - | - |
| s3d | 16 | 5274.76 | - | - |
| scale-T | 1 | 254.02 | 254.03 | - |
| scale-T | 8 | 216.88 | - | - |
| scale-T | 16 | 178.46 | - | - |
| nyx_baryon | 1 | 7.85 | 7.85 | - |
| nyx_baryon | 8 | 7.87 | - | - |
| nyx_baryon | 16 | 7.91 | - | - |
| ocean_temp | 1 | 6.49 | 6.49 | - |
| ocean_temp | 8 | 5.85 | - | - |
| ocean_temp | 16 | 5.75 | - | - |
| einspline37 | 1 | 3.38 | 3.38 | - |
| einspline37 | 8 | 3.37 | - | - |
| einspline37 | 16 | 3.38 | - | - |

**szx_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 21.51 | 21.51 | - |
| miranda | 8 | 21.51 | - | - |
| miranda | 16 | 21.51 | - | - |
| s3d | 1 | 26.12 | 26.12 | - |
| s3d | 8 | 26.12 | - | - |
| s3d | 16 | 26.12 | - | - |
| scale-T | 1 | 5.33 | 5.33 | - |
| scale-T | 8 | 5.33 | - | - |
| scale-T | 16 | 5.33 | - | - |
| nyx_baryon | 1 | 2.33 | 2.33 | - |
| nyx_baryon | 8 | 2.33 | - | - |
| nyx_baryon | 16 | 2.33 | - | - |
| ocean_temp | 1 | 2.35 | 2.35 | - |
| ocean_temp | 8 | 2.35 | - | - |
| ocean_temp | 16 | 2.35 | - | - |
| einspline37 | 1 | 1.60 | - | - |
| einspline37 | 8 | 1.60 | - | - |
| einspline37 | 16 | 1.60 | - | - |

**zfp_cpu_r8**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 8.00 | 8.00 | - |
| miranda | 8 | 8.00 | - | - |
| miranda | 16 | 8.00 | - | - |
| s3d | 1 | 8.00 | 8.00 | - |
| s3d | 8 | 7.87 | - | - |
| s3d | 16 | 8.00 | - | - |
| scale-T | 1 | 3.92 | 3.92 | - |
| scale-T | 8 | 3.27 | - | - |
| scale-T | 16 | 3.50 | - | - |
| nyx_baryon | 1 | 4.00 | 4.00 | - |
| nyx_baryon | 8 | 4.00 | - | - |
| nyx_baryon | 16 | 4.00 | - | - |
| ocean_temp | 1 | 3.78 | 3.78 | - |
| ocean_temp | 8 | 2.83 | - | - |
| ocean_temp | 16 | 1.89 | - | - |
| einspline37 | 1 | 4.00 | 4.00 | - |
| einspline37 | 8 | 4.00 | - | - |
| einspline37 | 16 | 4.00 | - | - |

**cuszp_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 21.06 | 21.06 | - |
| miranda | 8 | 21.06 | - | - |
| miranda | 16 | 21.06 | - | - |
| s3d | 1 | 46.22 | 46.22 | - |
| s3d | 8 | 46.21 | - | - |
| s3d | 16 | 46.19 | - | - |
| scale-T | 1 | 12.45 | 12.45 | - |
| scale-T | 8 | 12.45 | - | - |
| scale-T | 16 | 12.45 | - | - |
| nyx_baryon | 1 | 4.29 | 4.29 | - |
| nyx_baryon | 8 | 4.29 | - | - |
| nyx_baryon | 16 | 4.29 | - | - |
| ocean_temp | 1 | 3.71 | 3.71 | - |
| ocean_temp | 8 | 3.71 | - | - |
| ocean_temp | 16 | 3.71 | - | - |
| einspline37 | 1 | 5.26 | - | - |
| einspline37 | 8 | 3.37 | - | - |
| einspline37 | 16 | 3.37 | - | - |

## 4. Residency arms: end-to-end write time (ms)

| codec | dataset | arm | VOL | filter | filter / VOL |
|---|---|---|---|---|---|
| cuszp_1e3 | miranda | devres | 231.0 | - | - |
| cuszp_1e3 | miranda | rtrip | 505.4 | - | - |
| cuszp_1e3 | miranda | hostsim | 314.6 | 736.5 | 2.34 |
| cuszp_1e3 | s3d | devres | 358.2 | - | - |
| cuszp_1e3 | s3d | rtrip | 1024.3 | - | - |
| cuszp_1e3 | s3d | hostsim | 485.4 | 1939.1 | 3.99 |
| cuszp_1e3 | scale-T | devres | 374.7 | - | - |
| cuszp_1e3 | scale-T | rtrip | 744.6 | - | - |
| cuszp_1e3 | scale-T | hostsim | 550.9 | 1570.8 | 2.85 |
| cuszp_1e3 | nyx_baryon | devres | 511.0 | - | - |
| cuszp_1e3 | nyx_baryon | rtrip | 950.1 | - | - |
| cuszp_1e3 | nyx_baryon | hostsim | 569.6 | 1600.5 | 2.81 |
| cuszp_1e3 | ocean_temp | devres | 372.3 | - | - |
| cuszp_1e3 | ocean_temp | rtrip | 472.9 | - | - |
| cuszp_1e3 | ocean_temp | hostsim | 356.9 | 800.6 | 2.24 |
| cuszp_1e3 | einspline37 | hostsim | 4703.8 | - | - |
| cuszp_1e6 | miranda | devres | 401.9 | - | - |
| cuszp_1e6 | miranda | rtrip | 629.6 | - | - |
| cuszp_1e6 | miranda | hostsim | 463.3 | 933.5 | 2.01 |
| cuszp_1e6 | s3d | devres | 441.0 | - | - |
| cuszp_1e6 | s3d | rtrip | 1167.9 | - | - |
| cuszp_1e6 | s3d | hostsim | 559.6 | 1916.1 | 3.42 |
| cuszp_1e6 | scale-T | devres | 513.8 | - | - |
| cuszp_1e6 | scale-T | rtrip | 953.3 | - | - |
| cuszp_1e6 | scale-T | hostsim | 553.3 | 1424.8 | 2.58 |
| cuszp_1e6 | nyx_baryon | devres | 710.5 | - | - |
| cuszp_1e6 | nyx_baryon | rtrip | 1074.6 | - | - |
| cuszp_1e6 | nyx_baryon | hostsim | 813.0 | 1573.3 | 1.94 |
| cuszp_1e6 | ocean_temp | devres | 428.4 | - | - |
| cuszp_1e6 | ocean_temp | rtrip | 547.0 | - | - |
| cuszp_1e6 | ocean_temp | hostsim | 452.1 | 794.8 | 1.76 |
| zfp_gpu_r12 | miranda | devres | 394.5 | - | - |
| zfp_gpu_r12 | miranda | rtrip | 605.2 | - | - |
| zfp_gpu_r12 | miranda | hostsim | 407.7 | 977.0 | 2.40 |
| zfp_gpu_r12 | s3d | devres | 972.6 | - | - |
| zfp_gpu_r12 | s3d | rtrip | 1260.2 | - | - |
| zfp_gpu_r12 | s3d | hostsim | 726.7 | 2507.1 | 3.45 |
| zfp_gpu_r12 | scale-T | devres | 1143.2 | - | - |
| zfp_gpu_r12 | scale-T | rtrip | 1092.8 | - | - |
| zfp_gpu_r12 | scale-T | hostsim | 777.4 | 1956.2 | 2.52 |
| zfp_gpu_r12 | nyx_baryon | devres | 693.3 | - | - |
| zfp_gpu_r12 | nyx_baryon | rtrip | 933.9 | - | - |
| zfp_gpu_r12 | nyx_baryon | hostsim | 706.4 | 1910.8 | 2.70 |
| zfp_gpu_r12 | ocean_temp | devres | 351.7 | - | - |
| zfp_gpu_r12 | ocean_temp | rtrip | 562.0 | - | - |
| zfp_gpu_r12 | ocean_temp | hostsim | 374.4 | 859.1 | 2.29 |
| zfp_gpu_r16 | miranda | devres | 758.6 | - | - |
| zfp_gpu_r16 | miranda | rtrip | 696.2 | - | - |
| zfp_gpu_r16 | miranda | hostsim | 462.9 | 1261.8 | 2.73 |
| zfp_gpu_r16 | s3d | devres | 699.4 | - | - |
| zfp_gpu_r16 | s3d | rtrip | 1322.7 | - | - |
| zfp_gpu_r16 | s3d | hostsim | 814.9 | 2828.5 | 3.47 |
| zfp_gpu_r16 | scale-T | devres | 635.2 | - | - |
| zfp_gpu_r16 | scale-T | rtrip | 1016.9 | - | - |
| zfp_gpu_r16 | scale-T | hostsim | 718.0 | 2343.7 | 3.26 |
| zfp_gpu_r16 | nyx_baryon | devres | 600.1 | - | - |
| zfp_gpu_r16 | nyx_baryon | rtrip | 989.6 | - | - |
| zfp_gpu_r16 | nyx_baryon | hostsim | 668.9 | 2131.0 | 3.19 |
| zfp_gpu_r16 | ocean_temp | devres | 408.1 | - | - |
| zfp_gpu_r16 | ocean_temp | rtrip | 522.0 | - | - |
| zfp_gpu_r16 | ocean_temp | hostsim | 404.4 | 869.1 | 2.15 |
| zfp_gpu_r4 | miranda | devres | 347.2 | - | - |
| zfp_gpu_r4 | miranda | rtrip | 572.7 | - | - |
| zfp_gpu_r4 | miranda | hostsim | 410.9 | 976.9 | 2.38 |
| zfp_gpu_r4 | s3d | devres | 464.9 | - | - |
| zfp_gpu_r4 | s3d | rtrip | 1431.6 | - | - |
| zfp_gpu_r4 | s3d | hostsim | 540.1 | 2053.9 | 3.80 |
| zfp_gpu_r4 | scale-T | devres | 475.5 | - | - |
| zfp_gpu_r4 | scale-T | rtrip | 792.0 | - | - |
| zfp_gpu_r4 | scale-T | hostsim | 471.3 | 1576.7 | 3.35 |
| zfp_gpu_r4 | nyx_baryon | devres | 366.8 | - | - |
| zfp_gpu_r4 | nyx_baryon | rtrip | 740.4 | - | - |
| zfp_gpu_r4 | nyx_baryon | hostsim | 430.5 | 1409.5 | 3.27 |
| zfp_gpu_r4 | ocean_temp | devres | 361.6 | - | - |
| zfp_gpu_r4 | ocean_temp | rtrip | 428.9 | - | - |
| zfp_gpu_r4 | ocean_temp | hostsim | 391.7 | 630.3 | 1.61 |
| zfp_gpu_r8 | miranda | devres | 384.7 | - | - |
| zfp_gpu_r8 | miranda | rtrip | 554.9 | - | - |
| zfp_gpu_r8 | miranda | hostsim | 363.7 | 1024.4 | 2.82 |
| zfp_gpu_r8 | s3d | devres | 588.0 | - | - |
| zfp_gpu_r8 | s3d | rtrip | 1159.6 | - | - |
| zfp_gpu_r8 | s3d | hostsim | 619.2 | 2190.3 | 3.54 |
| zfp_gpu_r8 | scale-T | devres | 624.1 | - | - |
| zfp_gpu_r8 | scale-T | rtrip | 922.1 | - | - |
| zfp_gpu_r8 | scale-T | hostsim | 645.8 | 1874.2 | 2.90 |
| zfp_gpu_r8 | nyx_baryon | devres | 523.5 | - | - |
| zfp_gpu_r8 | nyx_baryon | rtrip | 875.0 | - | - |
| zfp_gpu_r8 | nyx_baryon | hostsim | 659.4 | 1608.9 | 2.44 |
| zfp_gpu_r8 | ocean_temp | devres | 330.3 | - | - |
| zfp_gpu_r8 | ocean_temp | rtrip | 408.0 | - | - |
| zfp_gpu_r8 | ocean_temp | hostsim | 343.8 | 702.1 | 2.04 |

## 5. Progressive reads

| dataset | codec | pct | VOL bytes read | filter bytes read | VOL read ms | filter read ms | VOL RMSE | filter RMSE |
|---|---|---|---|---|---|---|---|---|
| s3d | sperr_pwe1e3 | 5 | 10.6% | - | 2901 | - | 4.157e-04 | - |
| s3d | sperr_pwe1e3 | 10 | 15.6% | - | 2924 | - | 2.281e-04 | - |
| s3d | sperr_pwe1e3 | 25 | 30.6% | - | 2941 | - | 8.799e-05 | - |
| s3d | sperr_pwe1e3 | 50 | 55.6% | - | 2946 | - | 4.638e-05 | - |
| s3d | sperr_pwe1e3 | 75 | 80.6% | - | 2966 | - | 3.314e-05 | - |
| s3d | sperr_pwe1e3 | 100 | 100.0% | - | 2984 | - | 2.036e-05 | - |
| s3d | sperr_pwe1e6 | 5 | 5.1% | - | 3372 | - | 8.305e-06 | - |
| s3d | sperr_pwe1e6 | 10 | 10.1% | - | 3397 | - | 3.374e-06 | - |
| s3d | sperr_pwe1e6 | 25 | 25.1% | - | 3488 | - | 9.352e-07 | - |
| s3d | sperr_pwe1e6 | 50 | 50.1% | - | 3556 | - | 3.243e-07 | - |
| s3d | sperr_pwe1e6 | 75 | 75.1% | - | 3612 | - | 2.034e-07 | - |
| s3d | sperr_pwe1e6 | 100 | 100.0% | - | 3802 | - | 1.191e-07 | - |
| scale-T | sperr_pwe1e3 | 5 | 5.3% | - | 3577 | - | 6.120e-04 | - |
| scale-T | sperr_pwe1e3 | 10 | 10.3% | - | 3642 | - | 4.289e-04 | - |
| scale-T | sperr_pwe1e3 | 25 | 25.3% | - | 3690 | - | 2.602e-04 | - |
| scale-T | sperr_pwe1e3 | 50 | 50.3% | - | 3785 | - | 1.793e-04 | - |
| scale-T | sperr_pwe1e3 | 75 | 75.3% | - | 3864 | - | 1.446e-04 | - |
| scale-T | sperr_pwe1e3 | 100 | 100.0% | - | 4129 | - | 1.085e-04 | - |

## Recorded failures (rep=-1)

- chunk: einspline37 bzip2 N=1 hostsim
- chunk: einspline37 cuszp_1e3 N=1 hostsim
- chunk: einspline37 cuszp_1e6 N=1 hostsim
- chunk: einspline37 noop N=1 hostsim
- chunk: einspline37 sz3_1e6 N=1 hostsim
- chunk: einspline37 szx_1e3 N=1 hostsim
- chunk: einspline37 szx_1e6 N=1 hostsim
- chunk: einspline37 zfp_1e3 N=1 hostsim
- chunk: einspline37 zfp_1e6 N=1 hostsim
- chunk: einspline37 zfp_cpu_r12 N=1 hostsim
- chunk: einspline37 zfp_cpu_r16 N=1 hostsim
- chunk: einspline37 zfp_gpu_r12 N=1 hostsim
- chunk: einspline37 zfp_gpu_r16 N=1 hostsim
- chunk: einspline37 zfp_gpu_r4 N=1 hostsim
- chunk: einspline37 zfp_gpu_r8 N=1 hostsim

