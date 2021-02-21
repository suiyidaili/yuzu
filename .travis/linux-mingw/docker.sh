#!/bin/bash -ex

cd /yuzu
# override Travis CI unreasonable ccache size
echo 'max_size = 3.0G' > "$HOME/.ccache/ccache.conf"

mkdir build && cd build
cmake .. -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(pwd)/../CMakeModules/MinGWCross.cmake" -DUSE_CCACHE=ON -DENABLE_COMPATIBILITY_LIST_DOWNLOAD=ON -DCMAKE_BUILD_TYPE=Release
ninja

# Clean up the dirty hacks
rm /bin/uname && mv /bin/uname1 /bin/uname
rm /bin/cmd

ccache -s

echo "Tests skipped"
#ctest -VV -C Release

echo 'Prepare binaries...'
cd ..
mkdir package

QT_PLATFORM_DLL_PATH='/usr/x86_64-w64-mingw32/lib/qt5/plugins/platforms/'
find build/ -name "yuzu*.exe" -exec cp {} 'package' \;

# copy Qt plugins
mkdir package/platforms
cp "${QT_PLATFORM_DLL_PATH}/qwindows.dll" package/platforms/
cp -rv "${QT_PLATFORM_DLL_PATH}/../mediaservice/" package/
cp -rv "${QT_PLATFORM_DLL_PATH}/../imageformats/" package/
rm -f package/mediaservice/*d.dll

for i in package/*.exe; do
  # we need to process pdb here, however, cv2pdb
  # does not work here, so we just simply strip all the debug symbols
  x86_64-w64-mingw32-strip "${i}"
done

pip3 install pefile
python3 .travis/linux-mingw/scan_dll.py package/*.exe "package/"
python3 .travis/linux-mingw/scan_dll.py package/imageformats/*.dll "package/"
