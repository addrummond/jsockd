# OpenBSD build instructions

## Install necessary packages

```sh
pkg_add install -y git gmake node npm clang
```

## Install CMake 4

```sh
wget https://github.com/Kitware/CMake/releases/download/v4.2.0/cmake-4.2.0.tar.gz
tar -xzf cmake-4.2.0.tar.gz
cd cmake-4.2.0
./bootstrap && make && sudo make install
```

## Clone and build JSockD

```sh
git clone https://github.com/addrummond/jsockd.git
cd jsockd
./build_quickjs.sh
( cd jsockd_server && ./mk.sh Release )
```

Release binary is now at `jsockd_server/build_Release/jsockd`.
