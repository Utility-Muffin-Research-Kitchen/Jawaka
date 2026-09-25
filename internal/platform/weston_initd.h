#ifndef JW_WESTON_INITD_H
#define JW_WESTON_INITD_H

/* Shell command prefix for every stop, start and restart of the stock
   compositor. S49weston backgrounds Weston, so it reparents to init and outlives
   the Leaf generation that restarted it, together with its clients and the log
   tee. Run it from / and without the loader variables: jawakad's
   LD_LIBRARY_PATH points at the launcher's lib/ on the SD card, so those
   processes would map libz and libatomic from the card for the rest of the boot.
   Once a launcher update replaces the files, the mappings pin deleted inodes and
   the shutdown barrier cannot remount the card read-only. A card cwd does the
   same.

   The rest of the environment is kept on purpose: the session exports the
   WESTON_DRM_* panel-first settings when HDMI is connected at boot, and a
   restart must not fall back to mirroring onto the TV. Extra settings go after
   the prefix as NAME=VALUE words, before the script path. */
#define JW_WESTON_INITD_SCRIPT "/etc/init.d/S49weston"
#define JW_WESTON_ROOTFS_ENV "cd / && env -u LD_LIBRARY_PATH -u LD_PRELOAD"
#define JW_WESTON_INITD(verb) JW_WESTON_ROOTFS_ENV " " JW_WESTON_INITD_SCRIPT " " verb

#endif
