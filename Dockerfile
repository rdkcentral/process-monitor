# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -- -j"$(nproc)"

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/ProcessMonitor /usr/local/bin/ProcessMonitor
COPY --from=build /src/build/libexithandler.so* /usr/local/lib/

ENTRYPOINT ["/usr/local/bin/ProcessMonitor"]
CMD ["--help"]
