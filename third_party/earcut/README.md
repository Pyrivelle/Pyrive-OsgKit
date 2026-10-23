# mapbox/earcut.hpp

- Upstream: https://github.com/mapbox/earcut.hpp
- Pinned commit: `177bd66b5338ab66f4e9ce8f3cca418b9463d809`
- Retrieved: 2026-10-21
- Header: `include/mapbox/earcut.hpp`, copied without modifications.
- Header SHA-256: `29091c0d7af22a2f985f5ebd9aedb1f1b056fe099d1d31395c393c5f1ecb8c52`
- License: ISC; original notice is in `LICENSE` and packaged as `dist/licenses/earcut.txt`.

Private build dependency of PereViaOsg; consumers do not need this header.
Only `mapbox::earcut<uint32_t>` is used. The optional `mapbox::refine` pass is not enabled.
IslandBuilder accepts one simple outer ring and up to 64 non-touching, non-nested holes.
Self-intersections, overlapping rings and holes outside the outer ring are rejected.
Input validation, output count/index/winding/area checks, terrain subdivision, heights,
normals, colors and the OSG mesh remain in PereVia code.

## Local comparison

VS 2026 / MSVC v145, `/O2 /MD`, synthetic alternating-radius concave stars,
100 triangulations per input size, average milliseconds per call (2026-10-21):

| Points | Previous ear clipping | Earcut adapter including output checks |
| ---: | ---: | ---: |
| 10 | 0.001369 | 0.002384 |
| 128 | 0.046678 | 0.026465 |
| 512 | 0.733351 | 0.329693 |
| 2048 | 10.535139 | 2.932524 |

This compares initial triangulation only, excluding input validation, subdivision,
height generation, normals, OSG allocation and rendering. It is a small local
experiment, not a general performance guarantee. Small contours can be slower
because of adapter/allocation/check overhead.

The current 10-point demo at subdivision 5 still has 49,792 triangles,
26,178 stored vertices and 1,958,760 bytes of mesh array payload. Its updated
surface area is 9,403.942 square scene units and enclosed volume is 106,382.399
cubic scene units. The changed connectivity alters terrain samples slightly.
