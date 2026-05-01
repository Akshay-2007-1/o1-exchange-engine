FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-all-dev \
    libgtest-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build \
    -DFETCH_GTEST=OFF \
    -DFETCH_JSON=OFF \
    && cmake --build build

EXPOSE 9001

CMD ["./build/engine"]