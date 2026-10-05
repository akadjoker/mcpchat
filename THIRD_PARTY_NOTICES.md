# Third-party software

Bundled in `third_party/`:

| Component | Version | License |
|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT |
| [stb_image](https://github.com/nothings/stb) | 2.28 | Public domain / MIT |

Fetched by CMake at configure time (see `cmake/Dependencies.cmake`):

- [zen_platform](https://github.com/akadjoker/zen_plataform) — window and input (MIT)
- [iGUI](https://github.com/akadjoker/iGUI) — immediate-mode UI (MIT), which embeds the Roboto font (Apache License 2.0)
- [containers](https://github.com/akadjoker/containers) — data structures used by iGUI (MIT)
- libcurl (Linux, system library) — curl license
