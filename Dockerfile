FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libboost-all-dev \
    libgtest-dev \
    libsqlite3-dev \
    libsodium-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build \
    -DFETCH_GTEST=OFF \
    -DFETCH_JSON=OFF \
    && cmake --build build

EXPOSE 9001

# The engine opens its SQLite file at data/exchange.db (relative to WORKDIR).
# Mounting a volume here, not at /app itself, persists the database across
# container recreations without shadowing the compiled binary.
VOLUME ["/app/data"]

CMD ["./build/engine"]