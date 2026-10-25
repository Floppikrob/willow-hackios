set -e
S=$(dirname "$(readlink -f "$0")")
U=$HOME/hackios/userland
T=$HOME/hackios/xnu-deps/build/mesa
export PATH=$HOME/hackios/xnu-deps/host/bin:$PATH
cd $U
glslangValidator -V --vn vk_test_comp -o obj/vk_test_comp.h src/vk_test.comp
xnu-clang -arch arm64 -isysroot $HOME/theos/sdks/iPhoneOS16.5.sdk -O1 -Iobj -I$T/mesa-turnip/include -o obj/vk_test src/vk_test.c
mkdir -p $HOME/willow/vk-share/vulkan
cp $T/build-turnip/src/freedreno/vulkan/libvulkan_freedreno.dylib $HOME/willow/vk-share/vulkan/
llvm-strip -x $HOME/willow/vk-share/vulkan/libvulkan_freedreno.dylib
ls -la obj/vk_test $HOME/willow/vk-share/vulkan/
