![Background](./res/gui/title_background.png)
# mcmux
mcmux is a C redstone sandbox based on Minecraft 1.16.2. The GUI combines a
world editor, redstone simulation, saved worlds, structure export, and an
embedded mc-nvim workspace.

## Build

Use a C compiler to compile `nob.c` into the `nob` build runner. Then run the
`./nob` commands from the project root. On Windows, use the equivalent
`nob.exe` form.

| Command | Purpose |
| --- | --- |
| `./nob` or `./nob build` | Build the project. |
| `./nob gui` | Build the GUI. |
| `./nob run` | Build and launch the GUI. |
| `./nob reader` | Build the standalone `.mcs` structure reader. |
| `./nob benchmark` | Run the deterministic redstone scaling benchmark. |
| `./nob vendor-list` | List the vendored raylib files required by the build. |
| `./nob precommit` | Stage the required raylib files and verify an index-only build. |
| `./nob clean` | Remove build outputs. |
| `./nob help` | Show the available build commands. |

## Use mcmux
| Command | Purpose |
| --- | --- |
| `Ctrl + b` & `%` | Open mc-nvim |
| `Ctrl + b` & `g` | Open mc-gui |
| `Ctrl + b` & `c` | Open mc-out |
| `Ctrl + b` & `?` | Help |


## mc-nvim

mc-nvim is the text workspace inside mcmux. The available editor actions are:

| Mode or command | Action |
| --- | --- |
| Normal mode: `i` | Insert before the cursor. |
| Normal mode: `a` | Insert after the cursor. |
| Normal mode: `x` | Delete the character under the cursor. |
| Normal mode: `h` `j` `k` `l` or arrow keys | Move the cursor. |
| `Esc` | Return to Normal mode. |
| `:Ex` | Browse the current directory. |
| `:e PATH` | Open a file, or prepare a new file when it does not exist. |
| `:w` | Save the current file. |
| `:make` | Save and build the current C file, updating the core library as needed. |
| `:run` | Save, build, and run the current C program from the project root. |
| `:q` | Close the current pane when its buffers are saved. |
| `:q!` | Close the current pane and discard its unsaved buffers. |
| `:wq` | Save and close the current pane. |
| `:! COMMAND` | Run an external build or program command from the pane directory. |

## mc-gui

World and simulation commands are entered in the GUI with `/` or `//`.
mc-nvim's `:` commands operate on editor files and programs.

| Command | Purpose |
| --- | --- |
| `//pos1 X Y Z` / `//pos2 X Y Z` | Set the inclusive selection corners. |
| `//copy [X Y Z]` | Copy the selection with an optional anchor. |
| `//paste [-a] X Y Z` | Paste at an anchor; `-a` skips air. |
| `//stack [COUNT] DIRECTION` | Add repeated copies in a direction. |
| `//move [DISTANCE] DIRECTION` | Move the selected blocks. |
| `//undo` / `//redo` | Undo or redo the last selection edit. |
| `//schem save [-f] NAME [X Y Z]` | Save the selection beside the current world's `.mcw`; `-f` replaces an existing file. |
| `/tick freeze` | Toggle automatic ticking. |
| `/tick step NUMBER` | Run a fixed number of ticks while frozen. |
| `/tick noupdate` | Toggle block callbacks and neighbor updates. |
| `/plot info` | Show the current plot bounds. |
| `/plot expand DIRECTION [COUNT]` | Expand the plot. |
| `/plot shrink DIRECTION [COUNT]` | Shrink the plot; confirm if edits would be deleted. |

## World directories

Each world is stored as a self-contained directory under `saves/`. The
directory and its `.mcw` file use the same world name. Programs and structures
belonging to that world can be stored beside the world file.

The repository includes an example world at `saves/examples/`.

## Optional CPU queries

After loading or building a circuit, call
`sim_use_cpu(world, min, max)` with inclusive region corners. This prepares a
plan from the current blocks and enables cached input/target queries for dust,
torches, repeaters and ordinary comparators. Actual notifications, delayed
transitions and game-tick phases retain the existing simulation order.

`sim_cpu_active(world)` reports whether the plan remains valid. Layout edits
invalidate it; call `sim_use_cpu()` again to rebuild, or
`sim_use_reference(world)` to release the plan. Unsupported queries, including
container analog overrides and region boundaries, use reference queries.
Other untracked revisions such as inventory or plot changes also invalidate
the plan conservatively. Backend selection is per world and is not saved.

`sim_stats(world)` includes `cpu_evaluations`, `cpu_cache_hits` and
`cpu_fallbacks`. Only the selected query path executes; runtime comparison
against the reference backend is not required. Preparation time and runtime
overhead depend on the circuit, so enable this explicitly when measuring it.

## Redstone scaling benchmark

`./nob benchmark` builds a deterministic vanilla-redstone workload and runs
the same fixed number of game ticks with the reference and CPU query backends.
The default repeater matrix uses width as independent parallel lanes and depth
as physically connected serial stages. A wide, shallow matrix stresses
throughput; a narrow, deep matrix stresses propagation latency. The saved CSA
cell is also available as an independent parallel bank with `--cell csa`.

For example:

```sh
./nob benchmark --width 4096 --depth 1 --ticks 4096 --rounds 7
./nob benchmark --width 1 --depth 4096 --ticks 4096 --rounds 7
./nob benchmark --width 64 --depth 64 --ticks 4096 --rounds 7
./nob benchmark --cell csa --width 256 --depth 1 --ticks 4096 --rounds 7
```

Inputs use a fixed seed and one of `dense`, `sparse` or `random` patterns.
Unless `--no-verify` is supplied, an untimed pass compares every output and
the ordered state-write trace after every game tick. Plan preparation is
reported separately from measured execution. CPU results are rejected if the
plan becomes inactive or any query falls back to the reference path. Record
the commit, command, compiler and machine together with each `RESULT` line;
generated worlds and timing files are build artifacts and are not committed.

## References

- https://github.com/raysan5/raylib
- https://github.com/tsoding/nob.h
- https://github.com/jdah/minecraft-weekend
- https://github.com/MCHPR/MCHPRS
- https://github.com/gnembon/fabric-carpet
- https://github.com/maruohon/tweakeroo
- https://github.com/EngineHub/WorldEdit
- https://github.com/tmux/tmux
