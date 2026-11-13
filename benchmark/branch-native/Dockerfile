FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make git ca-certificates llvm-7-dev libboost1.71-all-dev libreadline-dev zlib1g-dev libtinfo-dev \
 && rm -rf /var/lib/apt/lists/*
