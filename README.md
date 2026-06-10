# demo

Code repository for the EdgeZ Wi-Fi HaLow PoC kit.

## Repository summary

This repository is organized into two main parts:

- `device/`: ESP-IDF firmware and embedded components for the EdgeZ device.
- `server/`: Leshan-based LwM2M edge server implementation, including backend and frontend assets.

Key folders:

- `device/`: firmware sources, board integration, protocol objects, and embedded third-party components.
- `server/src/`: Java server code.
- `server/shared/`: shared frontend resources used by the server UI.
- `server/webapp/`: Vue 3 web application used by the Leshan demo server.

## License

The root project is distributed under Apache License 2.0. See `LICENSE`.

Important attribution for Leshan-derived code:

- `server/` is based on Eclipse Leshan and includes Apache License 2.0 terms in `server/LICENSE`.
- `server/shared/` and `server/webapp/` are copied/adapted from the Leshan project and follow Leshan's original dual-license terms:
	- BSD-3-Clause (`LICENSE.BSD-3-Clause`)
	- EPL-2.0 (`LICENSE.EPL-2`)

When redistributing or modifying these folders, keep the original license and copyright notices and comply with the applicable Leshan license terms.
