# Black Hole Simulator

An interactive 3D black hole simulation built from scratch in C++ with
raylib, as a learning project. Real SI-unit Newtonian gravity, a velocity
Verlet integrator, and procedural GPU shaders for the starfield and
accretion disk.

## Current status

Stage 5 complete. The simulation renders a black hole with a swirling
procedural accretion disk against a twinkling starfield, with full 3D
camera control and interactive body launching.

| Stage | What                               | Status  |
| ----- | ---------------------------------- | ------- |
| 0     | Toolchain setup                    | ✅ Done |
| 1     | 2D orbital sim                     | ✅ Done |
| 2     | N-body mutual gravity              | ✅ Done |
| 3     | 3D camera pass                     | ✅ Done |
| 4     | Procedural starfield shader        | ✅ Done |
| 5     | Procedural accretion disk          | ✅ Done |
| 6     | Gravitational lensing              | ⏳ Next |
| 7     | Interactive UI (mass/spin sliders) | ⏳      |
| 8     | State-sync multiplayer             | ⏳      |
| 9     | Headless cloud rendering           | ⏳      |

## Controls

| Input                             | Action                                                         |
| --------------------------------- | -------------------------------------------------------------- |
| Left-mouse drag                   | Launch a body (drag from start point, release to set velocity) |
| Space + left-drag                 | Orbit the camera                                               |
| Scroll wheel / `+` / `-` / arrows | Zoom                                                           |
| `1` / `2` / `3`                   | Spawn small / medium / large body in orbit                     |
| `I`                               | Spawn a body on a randomly inclined orbit                      |
| `C`                               | Clear all bodies and reset the swallowed counter               |

## Build

Requires CMake and a C++17 compiler. raylib 5.5 is fetched automatically
via CMake FetchContent.

```bash
cmake -B build
cmake --build build
```

Run from the project root (so the shader files resolve):

```bash
./build/blackholesim
```

The shaders `starfield.fs` and `disk.fs` are loaded at runtime relative to
the working directory, so launch the binary from the directory that
contains them.

## Project structure

```
.
├── main.cpp          # Simulation: physics, camera, interaction, render loop
├── starfield.fs      # Fullscreen procedural starfield (fragment shader)
├── disk.fs           # Procedural accretion disk (fragment shader)
├── CMakeLists.txt    # Build config (raylib via FetchContent)
├── docs/             # Stage-by-stage build guides
└── build/            # Compiler output (git-ignored; regenerated)
```

## How it works

**Physics.** Bodies feel Newtonian gravity from the black hole and from
each other. The integrator is velocity Verlet (kick-drift-kick), which is
symplectic — orbital energy error oscillates rather than drifting, so
orbits stay stable over long runs. The N-body pair loop uses Newton's
third law to halve the work. Two separate softening lengths keep the
black-hole interaction and the body-body interaction well-behaved at their
very different scales.

**Units.** Real SI throughout: G = 6.674e-11, c = 299,792,458 m/s, black
hole mass 8e30 kg. The Schwarzschild radius is computed as R_s = 2GM/c².
A render-scale factor maps simulation metres to on-screen units, and a
time-scale factor compresses a real orbital period into a watchable ~30
seconds.

**Starfield (`starfield.fs`).** A fullscreen fragment shader drawn behind
the 3D scene. Each pixel hashes its grid cell to decide whether it holds a
star, with a soft radial glow and per-star sinusoidal twinkle driven by a
`time` uniform.

**Accretion disk (`disk.fs`).** A fragment shader applied to a flat plane
mesh in the orbital plane. Per pixel it converts to polar coordinates,
carves a ring with `discard`, maps radius to a temperature color ramp
(white-hot inner edge to cool-red outer), and adds multi-octave value
noise for turbulent streaks. Rotation is differential (inner rings spin
faster) and is applied by rotating the noise sampling coordinate by a
wrapped angle, which keeps it seamless and numerically stable over
arbitrarily long runs.

## Known gaps and future work

- **No gravitational lensing yet** (Stage 6). Light travels straight; the
  disk doesn't wrap over the top of the hole.
- **Disk doesn't know the camera**, so there's no Doppler beaming (the
  approaching side should look brighter). Planned alongside Stage 6.
- **Disk inner/outer radius is hardcoded** in the shader and doesn't track
  the event-horizon size. A mass slider (Stage 7) would pass the horizon
  radius as a uniform.
- **Stars are screen-space**, so they don't parallax as the camera orbits.
  Fine as a backdrop; will be revisited when lensing samples the
  background per-ray.
- **Trail trimming is O(n) per frame** (front-erase on a vector).
  Negligible at current body counts; worth revisiting only if bodies
  multiply heavily.

## Learning philosophy

Every stage produces a running artifact. Code stays flat and unabstracted
until abstraction earns its place. The render loop is strictly phased:
update, then physics, then a single draw pass. Small commits, each a
working state.
