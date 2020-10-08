# Check http://releases.llvm.org/download.html#10.0.0 for the latest available binaries
FROM ubuntu:18.04

# Make sure the image is updated, install some prerequisites,
# Download the latest version of Clang (official binary) for Ubuntu
# Extract the archive and add Clang to the PATH
RUN apt-get update && apt-get install -y \
   xz-utils \
   build-essential \
   curl \
   llvm-7 llvm-7-dev llvm-7-runtime llvm-7-tools \
   clang-7

RUN apt-get install -y cmake git boost1.62

# Start from a Bash prompt
CMD [ "/bin/bash" ]
