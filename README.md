## Code reordering extension (CS2323 course project)
This fork extends Kites with a tool that reorders instructions inside basic blocks to cut
data-hazard stalls on the 5-stage pipelines, checks every change on the simulator, and can
unroll simple loops. On 54 benchmark programs it cuts total cycles by 2.8% (with forwarding)
and 15.0% (without), or 7.3% and 17.7% with loop unrolling, with no program slower or wrong.

- Team: Peddinti Sai Rukmini (reordering library: decoder, dependency graph, cost model,
  schedulers, safety check) and Shrestha Saanvi (evaluation, fuzz testing, loop unrolling, GUI)
- Code: src/reorder/ (library), cli/reorder_command.cpp (command line),
  src/ui/dialogs/ (GUI dialog), test/reorder_*_test.cpp (tests)
- Kites itself was built by our seniors; its original README follows.

# KITES : RISC - V SIMULATOR

Kites a is RISC V simulator and assembly code editor built for RISC V ISA.
Kites support I,M,D and F extenstion in single cyle mod and I,M extension in 5 Stage pipelining mode.(D,F will be added in future releases).

## Downloading and Installation

Prebuilt libraries are available for Linux,Windows on [Release Page](https://github.com/hghit-harshit/Kites/releases)

### Ubuntu(for only ubuntu-22.04 and above)
- Run chmod +x for the Kites AppImage.
- Run the file.

### Windows
- All the required files are inside the zip file, so you can just run the file.
- In case you get popup regarding some missing dll please contact us.

## MacOS
- Put Kites in the application 

RUN

```bash
sudo xattr -rd com.apple.quarantine sudo xattr -rd com.apple.quarantine /path/to/application.app

```

## Building

To build Kites you will first neet to install Qt(version above 6.9)
Then simply build using

```bash
cmake -DCMAKE_PREFIX_PATH="path/to/Qt/6.x.x/<compiler>" ..
```

## Testing

See [docs/TESTING.md](TESTING) for how to build and run tests, including coverage reports.

## CLI

See [docs/CLI.md](docs/CLI.md) for how to build and use the headless command-line executable.

