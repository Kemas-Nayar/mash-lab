# Lunyx C2 Lab (localhost-only)

A deliberately constrained HTTPS beacon/task-queue laboratory. The agent has no shell execution, file transfer, persistence, injection, or arbitrary command capability. It accepts only `status`, `interval`, and `shutdown` tasks.

## Requirements

- Linux
- C++20 compiler
- Boost headers (Beast and PropertyTree)
- OpenSSL development headers/libraries
- CMake 3.20+

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Create a localhost certificate

The certificate includes `localhost` and `127.0.0.1` SANs. The agent validates the certificate against this certificate file and checks the `localhost` identity.

```sh
mkdir -p certs
openssl req -x509 -newkey rsa:3072 -sha256 -days 30 -nodes \
  -keyout certs/server.key -out certs/server.crt \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
chmod 600 certs/server.key
```

## Run

Terminal 1:

```sh
./build/controller
```

Terminal 2:

```sh
./build/agent
```

Controller console commands:

```text
queue status
queue interval 10
kill
quit
```

`kill` is a one-way killswitch for the current controller process: every connected agent exits on its next beacon. Restart the controller to reset it. The controller binds explicitly to `127.0.0.1:8443`; the agent destination is compiled as `127.0.0.1:8443` and is not configurable.

## Protocol

`POST /beacon` exchanges a small JSON beacon and one queued task over TLS. The queue is in-memory and shared by agents. Sleep jitter is uniformly sampled from 80% to 120% of the configured interval.

## Scope

This is a local protocol/process-control lab, not a general-purpose remote administration tool. Do not expose the listener beyond loopback.
