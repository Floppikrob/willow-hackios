set -e
cd $HOME/hackios
FLASHBOOT=1 CRYPTEXES=flash REST=1 RC_JOBS=rc.jobs.vktest.willow EXTRA=$HOME/hackios/userland/obj/vk_test EXTRA_SHARE=$HOME/willow/vk-share/vulkan userland/build.sh > /tmp/ub-vk.log 2>&1 || { tail -30 /tmp/ub-vk.log; exit 1; }
tail -5 /tmp/ub-vk.log
ls -la userland/root/share/vulkan userland/root/bin/vk_test userland/root.apfs
bash "$(dirname "$(readlink -f "$0")")/iosimg.sh" 240 "gpu=1 osm=1 touchscreen=1 fbwidth=1080 fbheight=2340 fb_beat=0 sb_statusbar=1 ufs_write=data var_disk=1" 2>&1 | tail -8
