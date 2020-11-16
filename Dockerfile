FROM ubuntu:20.04
MAINTAINER Maximilian E. Schüle <m.schuele@tum.de>
ENV DEBIAN_FRONTEND=noninteractive
# how many cores to use for compilation
ARG BUILD_CORES
# update
RUN apt-get update
# Install some tools
RUN apt-get install -y cmake g++ git llvm-7 libboost1.71-all-dev
## Install pistache
RUN cd /tmp/ && git clone https://github.com/oktal/pistache.git && cd pistache && git submodule update --init && \
   mkdir build && cd build && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release .. && make -j${BUILD_CORES} && make install && \
   LD_LIBRARY_PATH=/usr/local/lib && export LD_LIBRARY_PATH && ldconfig -v
# Run the rest as non root user
RUN useradd -ms /bin/bash dockeruser
USER dockeruser
WORKDIR /home/dockeruser
# copy work dir
RUN mkdir ./tardisdb
COPY --chown=dockeruser . ./tardisdb/
# Install tardisdb
RUN cd ./tardisdb && rm -rf build && mkdir build && cd ./build && cmake .. && make -j${BUILD_CORES}
# Run
EXPOSE 5000
CMD ["/home/dockeruser/tardisdb/build/server"]
