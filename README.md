# demo

Code repository for the EdgeZ Wi-Fi HaLow PoC kit.

## Getting started

Use this order for local development.

1. Start the server:

	```bash
	cd server
	mvn spring-boot:run
	```

2. Find your local IP address (macOS example):

	```bash
	ipconfig getifaddr en0
	```

	If you are on Ethernet, use `en1` (or run `ifconfig` to confirm your active interface).

3. Build the client with that server IP:

	- Update `device/sdkconfig.defaults.esp32s3` and set:

	  `CONFIG_LWM2M_SERVER_URI="coap://<YOUR_LOCAL_IP>:5683"`

	  Example:

	  `CONFIG_LWM2M_SERVER_URI="coap://192.168.1.100:5683"`

	- Then build from the device folder:

	  ```bash
	  cd device
	  idf.py build
	  ```

4. Optional no-build path (flash prebuilt firmware):

	 - Open https://www.edgez.ai/flasher
	 - In the Server dropdown, choose `custom`
	 - Enter your local server IP in the server input field
	 - Flash the firmware from the web flasher

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
