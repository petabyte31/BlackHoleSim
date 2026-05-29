# Black Hole Simulator

An interactive 3D black hole simulation built from scratch in C++, with real
physics, real graphics programming, and a slow, deliberate path from
"empty window" to "Einstein ring and glowing accretion disk."

This is a personal project — equal parts physics, graphics, and a vehicle
for learning C++ properly.

> **Status:** Active development. Currently at **Stage 1 of 9** —
> writing the 2D orbital simulation from scratch as the foundation.

---

## The vision

A black hole sits at the center of an interactive 3D world. I can drop
spaceships, moons, and planets near it and watch them orbit, spiral, or
get torn apart and swallowed. I can fly the camera around. I can adjust
the black hole's **mass** with a slider and watch the event horizon grow,
the accretion disk brighten, and the relativistic light-bending warp
intensify. Friends can connect over a network and watch the same live
simulation from their own machines.

When I want a high-quality clip to share, I rent a GPU for a few minutes,
render a 4K cinematic of a scenario, and ship the video.

The simulation is grounded in real physics — Newtonian gravity for orbits,
the Schwarzschild metric for the visual horizon, ray-marched geodesics for
the gravitational lensing, procedural plasma noise for the accretion disk.
Not a movie of a black hole. A black hole simulator that produces movies.

---

## Why I'm building this

Three reasons, in order of how much they matter to me:

1. **Black holes are the most spectacular physics in the universe.** They
   bend light, freeze time, and contain regions where the laws of physics
   as we know them break down. Building a simulator means understanding
   them on a level that reading about them can't reach.

2. **I'm learning C++, and I want to learn it by building something real.**
   Tutorials get you to "hello world." Projects get you to a working
   craft. This project is structured so that every stage exercises a new
   set of C++ ideas — `struct`s and `std::vector` early, references and
   `const`-correctness next, templates and shaders and networking later.

3. **It's a long-term project I won't get bored of.** The endpoint is far
   enough that there's always a next thing worth working on.

---

## Current capabilities

This list grows as the project does. Right now:

- [x] Build system set up (CMake fetches raylib automatically)
- [x] Compiles and runs on macOS (M1, Apple Silicon)
- [ ] 2D orbital simulation with click-and-drag to launch bodies _(Stage 1 — in progress)_
- [ ] N-body mutual gravity _(Stage 2)_
- [ ] 3D scene with orbiting camera _(Stage 3)_
- [ ] First fragment shader — procedural starfield _(Stage 4)_
- [ ] Procedural accretion disk _(Stage 5)_
- [ ] Gravitational lensing via ray-marched geodesics _(Stage 6)_
- [ ] Interactive UI: adjustable mass, spawn objects, save scenarios _(Stage 7)_
- [ ] Networked shared viewing for friends _(Stage 8)_
- [ ] Headless cloud rendering for cinematic clips _(Stage 9)_

---

## The physics

Stage 1 is **Newtonian gravity around a point mass**, with the event
horizon as a hard boundary that consumes anything that crosses it. The
acceleration on a body at position **r** from a black hole of mass _M_ at
the origin is

    a = -G·M · r / (|r|² + ε²)^(3/2)

The softening term `ε²` keeps the simulation stable when a body gets
close to the singularity. Integration uses **velocity Verlet**
(kick–drift–kick), which conserves energy over long timescales — circles
stay circular, ellipses stay ellipses. Naive Euler integration looks
right for a few frames and then spirals everything apart; using Verlet
from the start is what separates a toy from a simulation.

Later stages bring in real general-relativistic effects:

- **Schwarzschild horizon radius:** `R_s = 2GM/c²`, which is what makes
  the horizon grow when you turn up the mass.
- **Geodesics of light in curved spacetime**, ray-marched per pixel in a
  fragment shader, producing gravitational lensing and the photon sphere.
- **Doppler beaming** on the rotating accretion disk — the side moving
  toward the camera is brighter and bluer.
- **Gravitational redshift** near the horizon — light coming from close to
  the event horizon shifts toward red.

What this project **doesn't** simulate, and won't, because they're either
not visual or out of scope:

- Hawking radiation (timescales beyond the age of the universe)
- The information paradox (a debate in equations, not a renderable thing)
- Kerr (rotating) black holes with full frame-dragging — possibly in a
  far-future stage, but a serious research-level undertaking

---

## Tech stack

| Layer           | Choice                                                |
| --------------- | ----------------------------------------------------- |
| Language        | C++17                                                 |
| Graphics        | [raylib](https://www.raylib.com/) (with GLSL shaders) |
| Build           | CMake (raylib auto-fetched via `FetchContent`)        |
| Editor          | VS Code with CMake Tools and C/C++ extensions         |
| Version control | Git                                                   |
| Platform        | macOS (M1, Apple Silicon)                             |

Future stages will add:

- A small networking library (`enet` or WebSockets) for shared viewing
- `ffmpeg` for stitching frames into video clips
- A cheap VPS for the sync server
- A per-second-billed cloud GPU (Vast.ai / RunPod) for offline renders

All chosen to be open source and frugal — pay only when there's a real
reason to.

---

## Project structure

```
bh_sim/
├── README.md             ← you are here
├── docs/
│   ├── roadmap.md        ← the 9-stage plan
│   ├── stage0_setup.md   ← toolchain install (one-time)
│   └── stage1_guide.md   ← current stage detailed plan
├── .gitignore
├── CMakeLists.txt
├── main.cpp              ← the simulation (will split into src/ later)
└── build/                ← generated; not in git
```

I'm starting with a flat layout. When the source grows past a few hundred
lines (probably Stage 2 or 3), `main.cpp` becomes:

```
src/
├── main.cpp
├── body.cpp / body.hpp
├── physics.cpp / physics.hpp
└── render.cpp / render.hpp
shaders/
├── starfield.glsl
└── disk.glsl
```

Not before. Premature folder structure is a tax on attention.

---

## Building

You need:

- Xcode Command Line Tools (`xcode-select --install`)
- Homebrew, then `brew install cmake`
- VS Code with the **C/C++** and **CMake Tools** extensions

Then:

```bash
git clone <repo-url>
cd bh_sim
```

Open the folder in VS Code. CMake Tools will detect `CMakeLists.txt` and
offer to configure — accept, pick the **Clang** kit. The first build
takes 5–10 minutes because raylib is downloading and compiling. Every
build after that is seconds.

Hit `F7` to build, `Shift+F5` (or the play button) to run.

On an 8 GB Mac, set `cmake.parallelJobs` to **2** in VS Code settings
before the first build, and close heavy apps. Detail in
[`docs/stage0_setup.md`](docs/stage0_setup.md).

---

## Controls

Will grow as the project does. As of Stage 1:

| Key / action    | Effect                                                                |
| --------------- | --------------------------------------------------------------------- |
| `1`             | Spawn a spaceship in orbit                                            |
| `2`             | Spawn a moon in orbit                                                 |
| `3`             | Spawn an Earth in orbit                                               |
| `P`             | Plunge: spawn something on a low-energy trajectory toward the horizon |
| `C`             | Clear the scene                                                       |
| Left mouse drag | Aim and launch a custom body (Stage 1, step 6)                        |

---

## The roadmap

A nine-stage plan, each producing a runnable artifact. The full version is
in [`docs/roadmap.md`](docs/roadmap.md). Summary:

| Stage | What                                             | Status         |
| ----- | ------------------------------------------------ | -------------- |
| 0     | Toolchain setup                                  | ✅ done        |
| 1     | 2D orbital sim from scratch                      | 🚧 in progress |
| 2     | N-body mutual gravity                            | ⏳ next        |
| 3     | Promote to 3D                                    | ⏳             |
| 4     | First shader: procedural starfield               | ⏳             |
| 5     | Procedural accretion disk                        | ⏳             |
| 6     | Gravitational lensing                            | ⏳             |
| 7     | Interactive UI: mass slider, spawn UI, save/load | ⏳             |
| 8     | Multiplayer shared viewing                       | ⏳             |
| 9     | Headless cloud rendering                         | ⏳             |

Realistic total to a polished Stage 5: **4–6 months** of evenings and
weekends. The shader stages are harder than the C++ stages, and that's
where most of the time goes.

---

## Learning goals

This project is a vehicle for getting genuinely good at:

- **C++17**: structs, vectors, references, `const`-correctness, RAII,
  later templates and smart pointers
- **CMake** at a real-world level (not toy `Makefile` level)
- **Linear algebra** in 2D and 3D: vectors, dot products, transforms
- **Numerical integration** of ODEs — why Verlet beats Euler, what
  symplectic means
- **Graphics programming**: the GPU pipeline, vertex and fragment shaders,
  uniforms, ray-marching
- **General relativity**, at the level needed to render it — geodesics in
  Schwarzschild geometry, gravitational lensing, time dilation
- **Networking**: state synchronization, authoritative servers, UDP vs TCP
- **Linux server work**: deploying a service, ssh, systemd

Each stage uses what came before and adds one new thing. By the end I'll
have written 2000+ lines of C++ I understand top to bottom, plus shaders,
plus a server.

---

## Working philosophy

A few rules I try to keep:

- **Every stage produces a running thing.** If a stage doesn't end with
  something I can launch and play with, the stage isn't finished.
- **Small commits.** One sensible change per commit, working state every
  time. Future-me debugging at 2 AM will thank present-me.
- **Read errors before asking for help.** Compiler errors are verbose
  but the first three lines almost always say what's wrong.
- **Write it yourself.** Copying code I haven't understood means I have a
  black hole simulator with no new C++ in my head. Defeats the point.
- **Premature optimization is the enemy.** Profile first, then optimize
  the actual hot spot.
- **Premature architecture is the same enemy.** No `src/` folder until
  the second source file. No "engine" abstraction until I've built three
  things that would actually benefit from one.

---

## Acknowledgements

- The **Event Horizon Telescope** collaboration, whose images of M87* and
  Sagittarius A* set the visual bar.
- **NASA's black hole anatomy** writeup — the clearest one-page summary
  of what's actually around a black hole.
- The **raylib** team for a graphics library that's small, fast, and
  doesn't make you write 200 lines of boilerplate to open a window.
- _Interstellar_'s VFX team for proving cinematic-quality lensing was
  possible in real-time-adjacent rendering, and publishing the math.

---

## License

TBD. Probably MIT once the project is public.
