# Black Hole Simulator

A real-time, physically-grounded black hole engine built from scratch in C++
with raylib, as a learning project. It renders gravitational lensing by marching
light backwards along Schwarzschild null geodesics, and it simulates the
accretion flow as a real smoothed-particle fluid — then feeds that fluid _back_
through the same geodesics so the simulated matter is genuinely lensed and
Doppler-beamed. The result is the _Interstellar_-style image (not really,but still pretty good) : a dark shadow
ringed by a photon ring, with the far side of the disk lensed up and over the
top — reachable either as a polished cinematic render or as a live physics
simulation you can feed and steer.

## Current status

**All three modes complete.** The project is a mode-select homepage leading to
two GPU cinema renderers and one CPU physics simulation, all sharing one
Schwarzschild geodesic integrator.

| Mode            | What it is                                                                 | Status      |
| --------------- | -------------------------------------------------------------------------- | ----------- |
| **Black Hole**  | Cinematic lensed render — photon ring, over-the-top disk halo, beaming     | ✅ Complete |
| **Quasar**      | Same engine scaled up — volumetric slim disk, twin jets, feeding torus     | ✅ Complete |
| **Physics Sim** | Live SPH fluid accretion under relativistic gravity, lensed volumetrically | ✅ Complete |

What remains is tuning and scale — dialing the fluid constants, refining the
emitter feel, and (on faster hardware) parallelizing the particle splat to push
toward tens of thousands of particles at higher grid resolution. The
architecture is done; every mode exists and runs.One thing to keep in mind,you can try adjusting all the scales and constants in the physics sim if you please,as long as you have the computational capabilities for this,you can elevate N quickly in voxel_field.h to elevate the quality.

## Controls

**Global**

| Input               | Action                       |
| ------------------- | ---------------------------- |
| Space + left-drag   | Orbit the camera             |
| Scroll / Up / Down  | Zoom in / out                |
| Left / Right arrows | Rotate the camera            |
| `H`                 | Return to the homepage       |
| `Esc`               | (does not quit — use the OS) |

**Cinema (Black Hole / Quasar)**

| Input | Action                     |
| ----- | -------------------------- |
| `Q`   | Toggle Black Hole ↔ Quasar |

**Physics Sim**

| Input      | Action                                            |
| ---------- | ------------------------------------------------- |
| `D` + drag | Move the emitter nozzle across the view plane     |
| `T` + drag | Grab the aim arrow's tip and swing its direction  |
| `Z` / `X`  | Push the nozzle along the view axis (depth)       |
| `C`        | Toggle steady stream ↔ discrete infalling clumps  |
| `V`        | Toggle point rendering ↔ lensed volumetric render |

## Build

Requires CMake, a C++17 compiler, and **raylib 5.5** on the system (found via
`find_package`; on macOS `brew install raylib`). The physics sim's volumetric
renderer uses raw OpenGL 3D-texture calls, so it also needs the system GL
headers (on macOS, `<OpenGL/gl3.h>`, which exposes the full 4.1 API).

```bash
cmake -B build
cmake --build build
```

Run from the project root so the shaders and font resolve (they load at runtime
relative to the working directory):

```bash
./build/bh_sim
```

## Project structure

```
.
├── main.cpp        # Everything: menu, camera, both renderers, the SPH engine
├── voxel_field.h   # CPU→GPU bridge: splats particles into 3D textures each frame
│
│   # Cinema shaders (GPU)
├── lens.fs         # The scene: geodesic ray march, volumetric disk, jets, beaming
├── glass.fs        # Homepage "Liquid Glass" buttons that refract the backdrop
├── bright.fs       # Bloom pass — luminance threshold
├── blur.fs         # Bloom pass — separable Gaussian
├── composite.fs    # Bloom pass — combine + ACES tone map
│
│   # Physics-sim shaders (GPU)
├── volume.fs       # Lensed + beamed ray march through the SPH particle field
├── sky.fs          # Cheap starfield/nebula backdrop for physics mode
│
├── assets/         # Orbitron-Bold.ttf (homepage font)
├── reference/      # disk.fs, starfield.fs — dead Stage 4/5 checkpoints, kept for reference
├── CMakeLists.txt
└── build/          # Compiler output (git-ignored)
```

## How it works

The project contains **two fundamentally different renderers** behind one menu,
and they optimize for opposite things.

- **Cinema mode** asks, per pixel: _"a light ray leaves the camera through here —
  where does gravity bend it?"_ It ray-marches the Schwarzschild metric on the
  GPU, 360k pixels in parallel. It renders **light**.
- **Physics Sim** asks: _"I have 3000 pieces of fluid — where does each one go
  this frame?"_ It runs a smoothed-particle-hydrodynamics solver on the CPU. It
  renders **matter** — which is then marched by the _same_ geodesic so it lenses.

### The cinema render pipeline (main.cpp)

Every frame runs four passes through off-screen buffers:

1. **Scene** — `lens.fs` renders the full HDR image (full resolution).
2. **Bright extract** — `bright.fs` keeps pixels above a luminance threshold
   (half resolution).
3. **Blur ×2** — `blur.fs` applies a separable Gaussian, ping-ponging horizontal
   then vertical, twice, for a soft wide glow.
4. **Composite** — `composite.fs` adds the glow back and applies an **ACES filmic
   tone map**, bringing over-bright HDR values into range while keeping colour.

The homepage renders a live, slowly-orbiting black hole behind the UI, with
`glass.fs` refracting that backdrop through the menu buttons and a white-star
cursor trail.

### The lensing (lens.fs)

For each pixel a ray is fired from the camera and **marched backwards through
curved spacetime**. Each step bends the ray's velocity toward the hole via the
Schwarzschild null-geodesic term (`accel = -1.5 · rs · h² · rel / r⁵`, with the
angular momentum `h²` conserved along the ray). The march resolves:

- **Capture** — a ray reaching the photon sphere heading inward is swallowed;
  the pixel is black. This draws the shadow at its true apparent size (~2.6 Rs),
  with no fudge factor.
- **Disk** — the disk is sampled per step. In Black Hole mode it's an exact thin
  plane crossing; in Quasar mode it's a **volumetric slim disk** with Beer–Lambert
  emission/absorption, so the near side occludes the far side and it reads as a
  solid body. Because the ray is _bent_, rays aimed above the hole curve back
  down onto the far side of the disk — the halo that wraps over the top.
- **Escape** — surviving rays sample the procedural starfield/nebula in their
  final bent direction, so the sky is lensed too.

### The Quasar (lens.fs, mode-split)

The two cinema modes share one engine but branch on `quasarMode`:

- **Volumetric slim disk** — a puffy inner throat tapering to a thin, feathered
  rim (a lens profile, not a trumpet), sampled as a 3D volume the ray marches
  through. Density is a low-frequency clumping noise whose fragmentation ramps
  with radius — smooth ionized plasma inside, torn dusty gas at the rim.
- **Shakura–Sunyaev temperature** — `T ∝ r^(-3/4)·(1−√(r_isco/r))^(1/4)`, so the
  "Big Blue Bump" UV core stays compact near the ISCO and the sprawl is orange,
  with a dusty infrared torus (mostly obscuring, barely emitting) feeding it.
- **Blandford–Payne jets** — launched from a wide annulus at the inner disk,
  collimated to a waist, then diverging conically, with helical field twist and
  outward-traveling knots.

### Relativistic Doppler beaming

Disk material orbits at a large fraction of light speed. The side rotating
**toward** the camera is beamed brighter and bluer; the receding side dims and
reddens (flux ∝ Doppler factor³·⁵). This is the strong left–right asymmetry in
real black-hole imagery — and in the physics sim it's computed from each
particle's _actual simulated velocity_, not an assumed orbit.

## The physics engine (Physics Sim mode)

The sim models the accretion flow as a genuine fluid, not orbiting dots.

### Units

Simulating in SI would overflow `float` precision (positions ~10¹¹ m resolve to
kilometres). Instead everything runs in **render units**, with gravity derived
from a chosen render-unit speed of light via `GM = rs·c²/2` — so the physics is
self-consistent with the on-screen scale, and `c` doubles as the master
time-scale dial.

### Gravity — Paczyński–Wiita

Newtonian `GM/r²` has no innermost stable orbit, so nothing ever falls in.
Swapping it for `GM/(r−rs)²` reproduces the **ISCO at 3·rs** and the relativistic
plunge almost exactly, while converging to Newton far out — so it's used
everywhere, with no discontinuous mode-switch.

### The fluid — Smoothed Particle Hydrodynamics

Rather than a fixed grid, SPH discretizes the _fluid itself_ into particles.
Every field quantity at a point is a kernel-weighted sum over nearby particles —
each particle is a soft blob of radius `2h`, and overlapping blobs reconstruct a
continuous field. Per frame:

1. **Neighbour grid** — a uniform grid (cell size = `2h`) built by counting sort,
   so each particle checks only a 3×3×3 block (~300 candidates) instead of all 3000. Turns O(N²) into O(N·k).
2. **Density + pressure** — sum the cubic-spline kernel over neighbours for ρ,
   then the ideal-gas law `P = (γ−1)ρu` (γ = 5/3, monatomic hydrogen).
3. **Forces** — the momentum-conserving symmetric pressure gradient, plus
   **Monaghan artificial viscosity** (fires only between approaching pairs) to
   resolve shocks by turning compression into heat.
4. **Integrate** — gravity is sub-stepped 6× per frame for stability near the
   ISCO while the slower SPH force is held fixed (operator splitting); then
   α-viscosity, viscous heating, and β-cooling.

### Emergent luminosity

The disk's glow is not scripted. An α-viscosity term (standing in for the
unresolvable MRI turbulence that drives real accretion) bleeds orbital energy
into heat; because dissipation scales as v², the fast inner disk heats most.
That heat, balanced against radiative cooling, sets each particle's temperature,
which drives its colour. **Orange rim, white-hot ISCO — with no `heat = f(radius)`
anywhere.** The temperature gradient falls out of the energy balance.

### Volumetric lensing — the capstone

3000 discrete particles can't be lensed by a shader directly. So each frame they
are **splatted into 3D textures** (density, momentum-weighted velocity,
temperature), which `volume.fs` marches through along the _same_ Schwarzschild
geodesic as the cinema renderer. Because sampling rides the bent ray, the far
side of the simulated disk lenses over the shadow for free; because the velocity
field is carried, Doppler beaming uses the real local flow. A coarse **occupancy
texture** (rebuilt each frame from the neighbour grid, dilated to stay
conservative) lets the ray fast-forward through the ~95%-empty volume, skipping
_sampling_ but never _bending_ — so the optimization costs no physics.

## Learning philosophy

Built stage by stage, each producing a running artifact. Naive first, optimized
only when it hurt. Real physics wherever feasible, so the iconic look emerges
from the simulation rather than being painted on. Small commits, each a working
state.

_A note on scope: forward photon tracing (emitting light particles that scatter
and lens) is ~6 orders of magnitude too costly for real time — which is exactly
why renderers trace backward from the camera. The volumetric density-texture
bridge is the tractable path to lensing simulated matter, and it's the one used
here._
