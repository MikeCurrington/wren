# vscode-wren-debug

A minimal VS Code debug adapter *bridge* for Wren. The DAP server itself runs
inside the Wren VM (or the app embedding it), listening on a local TCP port;
this extension just points VS Code at that port, so there is no separate
adapter process and nothing to build (`extension.js` is plain JavaScript).

## Install (developer)

Symlink the folder into your VS Code extensions directory and restart VS Code:

```sh
ln -s "$PWD/extensions/vscode-wren-debug" \
      ~/.vscode/extensions/vscode-wren-debug
```

Pair it with a Wren syntax highlighting extension from the marketplace for
readable sources while debugging.

## Usage

1. Start the host application with its debugger enabled. For the standalone
   example host built by the CMake build:

   ```sh
   ./projects/cmake/build/wren_debug_example --port 4711 --wait-ms 60000 myscript.wren
   ```

   (`--wait-ms` holds the script until a client attaches, so you debug from
   the first line.)

2. In VS Code, open the folder containing your `.wren` scripts, add a
   launch configuration:

   ```json
   {
     "version": "0.2.0",
     "configurations": [
       { "type": "wren", "request": "attach", "name": "Attach to Wren VM", "port": 4711 }
     ]
   }
   ```

3. Set breakpoints in `.wren` files and start debugging. Breakpoints,
   stepping (over/in/out), the call stack, and variables all work. Local
   variables appear by slot position (`slot0`, `slot1`, ...) because the
   compiler does not retain local names; module-level variables and
   `System.print` output (in the debug console) appear by name.
