# HertzyEVS on a Jetson Orin Nano (JetPack 6, Ubuntu 22.04, aarch64)


## 1. Arena SDK for ARM64
Download **ArenaSDK_Linux_ARM64** (LUCID support portal), then:

    tar xzf ArenaSDK_Linux_ARM64.tar.gz -C ~
    cd ~/ArenaSDK_Linux_ARM64 && sudo sh Arena_SDK_ARM64.conf    # sets up ld paths
    sudo sysctl -w net.core.rmem_max=33554432                    # bigger socket buffers (GigE/PoE)

Triton2 EVS is GigE/PoE: put the Jetson NIC and camera in the same subnet (or use the Arena IpConfigUtility),
set MTU 9000 on that NIC if the switch allows it (`sudo ip link set eth0 mtu 9000`).

## 2. Build tools and Qt 6.8
JetPack 6 ships Qt 6.2, which is too old (project needs 6.8). Get 6.8 for arm64:

    sudo apt install build-essential cmake ninja-build git python3-pip libgl1-mesa-dev libxkbcommon-dev \
                     libfontconfig1-dev libdbus-1-dev libxcb-cursor0 fonts-noto-core
    pip install aqtinstall
    python3 -m aqt install-qt linux_arm64 desktop 6.8.3 linux_gcc_arm64 -O ~/Qt     # if aqt has no arm64 build, build Qt from source (qtbase, qtdeclarative, qtshadertools)

## 3. Build

    cd HertzyEVS
    cmake -S . -B build-jetson -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_arm64 \
          -DARENA_SDK_DIR=$HOME/ArenaSDK_Linux_ARM64 \
          -DHZ_EDITION=dev
    cmake --build build-jetson --target hertzyevs hz_tests hz_cli -j4

The configure output must say `HertzyEVS: Arena SDK found` and list the Arena libs; otherwise it is simulator-only.

## 4. Run
    ./build-jetson/hz_tests                             # should end with "N/N checks passed"
    ./build-jetson/hertzyevs --sim                       # test without camera
    ./build-jetson/hertzyevs --camera --out ~/evs        # real camera; ~/evs is the library (one folder per project)
    ./build-jetson/hz_cli measure ~/evs/P/clip.raw --lines        # analysis over ssh, no screen needed
    ./build-jetson/hz_cli stats ~/evs/P/*.raw                     # thumbnails + stats for copied-in recordings
Results are plain files next to each recording (`<clip>.measure/<name>/`, `automeasure_<date>.csv`, `batch_*.csv`):
`scp -r jetson:~/evs/P/clip.measure .` to take them to the PC.
    ./build-jetson/hertzyevs --camera --dump-nodes nodes.txt --wait 5     # dump node map for the guessed LUCID node names
    # no monitor attached:  QT_QPA_PLATFORM=offscreen ./build-jetson/hertzyevs --camera --record 10 --shot a.png

Max performance: `sudo nvpmodel -m 0 && sudo jetson_clocks`.

## Known gaps
- Icons: on Linux Theme.qml uses Unicode symbols from DejaVu Sans (`sudo apt install fonts-dejavu-core fonts-noto-color-emoji`), not Segoe Fluent Icons.
- Not compiled on aarch64 yet; the Arena library file names in CMakeLists are globbed and may need adjusting to the SDK version.
