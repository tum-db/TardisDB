# TardisDB with its PostgreSQL-compatible server: Ubuntu 24.04, GCC 13, LLVM 18 (as "Building with LLVM 18"
# in README.md). The container of the paper's numbers, with LLVM 7, is benchmark/branch-native/Dockerfile.
#
#   docker build -t tardisdb .
#   docker run --rm -p 127.0.0.1:5432:5432 tardisdb
#   psql -h 127.0.0.1 -p 5432 -U postgres -d tardisdb
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make git ca-certificates llvm-18-dev libboost-dev libreadline-dev zlib1g-dev libtinfo-dev \
 && rm -rf /var/lib/apt/lists/*
# how many cores to use for compilation (default: all)
ARG BUILD_CORES
# -DINTERVAL_VISIBILITY=ON etc. select a variant of the paper
ARG CMAKE_OPTIONS="-DINTERVAL_VISIBILITY=ON"
RUN useradd -ms /bin/bash tardisdb
USER tardisdb
WORKDIR /home/tardisdb
COPY --chown=tardisdb . ./src/
RUN mkdir build && cd build \
 && cmake -DCMAKE_BUILD_TYPE=Release -DTARDISDB_LLVM_VERSION=18 ${CMAKE_OPTIONS} ../src \
 && make -j${BUILD_CORES:-$(nproc)} sql pgserver
EXPOSE 5432
# listens on all interfaces of the container: no authentication, so publish the port on localhost only
# (docker run -p 127.0.0.1:5432:5432) unless the network is trusted
CMD ["/home/tardisdb/build/pgserver", "-h", "0.0.0.0", "-p", "5432"]
