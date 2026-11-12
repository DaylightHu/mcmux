# mcmux
mcmux is an experimental redstone simulator written in C, based on Minecraft 1.16.2. It started as a project to learn raylib and later evolved into an effort to combine redstone construction with programming.

The goal is to give users control over circuit inputs, execution backends, and simulation time, enabling faster execution of circuits built entirely from redstone. The computation remains in the circuits themselves, with their logic and in-game timing preserved.


> [!TIP]
> **📖 API Reference**
>
> Explore public functions, grouped by purpose, with concise descriptions.
>
> [**English →**](https://daylighthu.github.io/mcmux/mcmux-api-en.html) · [**简体中文 →**](https://daylighthu.github.io/mcmux/mcmux-api-zh.html)
>

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
| `./nob benchmark` | Generate C queries, compile them, and run the full redstone benchmark suite. |
| `./nob test-c` | Generate, compile and test the C query backend. |
| `./nob export-c INPUT.mcs OUTPUT.c [SYMBOL]` | Export an MCS circuit as a C query model. |
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

When the crosshair targets redstone dust, the top-left HUD shows its current
power level (0–15), including while ticking is frozen. The power line is hidden
when targeting any other block or empty space.

| Command | Purpose |
| --- | --- |
| `//pos1 X Y Z` / `//pos2 X Y Z` | Set the inclusive selection corners. |
| `//copy [X Y Z]` | Copy the selection with an optional anchor. |
| `//paste [-a] X Y Z` | Paste at an anchor; `-a` skips air. |
| `//stack [COUNT] DIRECTION` | Add repeated copies in a direction. |
| `//move [DISTANCE] DIRECTION` | Move the selected blocks. |
| `//undo` / `//redo` | Undo or redo the last selection edit. |
| `//schem save [-f] NAME [X Y Z]` | Save the selection beside the current world's `.mcw`; `-f` replaces an existing file. |
| `//schem load NAME` | Load an `.mcs` beside the current world's `.mcw` into the clipboard, preserving its anchor and container contents. Use `//paste X Y Z` to place it. |
| `/tick freeze` | Toggle automatic ticking. |
| `/tick step NUMBER` | Run a fixed number of ticks while frozen. |
| `/tick noupdate` | Toggle block callbacks and neighbor updates. |
| `/plot info` | Show the current plot bounds. |
| `/plot expand DIRECTION [COUNT]` | Expand the plot. |
| `/plot shrink DIRECTION [COUNT]` | Shrink the plot; confirm if edits would be deleted. |

Structure names accept an optional `.mcs` extension, with the same portable
letters, digits, `-` and `_` as `//schem save`. Loading does not change the world
or the selection; a failed load keeps the previous clipboard. Pasting retains
the normal `-a`, undo and redo behavior.

For example, place `demo-cancel-clear-wiring.mcs` in `saves/examples/`, open the
`examples` world, then run `//schem load demo-cancel-clear-wiring` followed by
`//paste 43 6 23` to place its saved anchor at its original position.

## World directories

Each world is stored as a self-contained directory under `saves/`. The
directory and its `.mcw` file use the same world name. Programs and structures
belonging to that world can be stored beside the world file.

The repository includes an example world at `saves/examples/`.

## References

- https://github.com/raysan5/raylib
- https://github.com/tsoding/nob.h
- https://github.com/jdah/minecraft-weekend
- https://github.com/MCHPR/MCHPRS
- https://github.com/gnembon/fabric-carpet
- https://github.com/maruohon/tweakeroo
- https://github.com/EngineHub/WorldEdit
- https://github.com/tmux/tmux
- https://github.com/neovim/neovim
- https://github.com/cktan/tomlc17
