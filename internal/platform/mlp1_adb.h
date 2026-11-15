#ifndef JW_MLP1_ADB_H
#define JW_MLP1_ADB_H

/* USB ADB on the MLP1 stock rootfs. Stock picks the USB functions from
   /etc/.usb_config at gadget start, and S40network deletes that file on every
   boot unless it is immutable, so "pinned" means usb_adb_en plus chattr +i.
   The enable command creates the file when it is missing (a freshly flashed
   rootfs has none).

   The gadget is driven through /usr/bin/usb-gadget itself. LoongOS 1.4.0.52
   removed /etc/init.d/S50usb-gadget.sh and starts the gadget from S50loong,
   which Leaf mode never runs. The init script only ever passed start, restart
   or stop through to /usr/bin/usb-gadget, and that binary is present on
   1.2.0.98 through 1.4.0.52 (byte-identical on 1.3.0.32, 1.4.0.36 and
   1.4.0.52), so there is no firmware where the script would work and the
   direct call would not.
   usb-gadget restart also starts its service when nothing is running yet;
   start stays as the fallback. */

#include <stdbool.h>
#include <unistd.h>

#define JW_MLP1_ADBD "/usr/bin/adbd"
#define JW_MLP1_USB_CONFIG "/etc/.usb_config"
#define JW_MLP1_USB_GADGET "/usr/bin/usb-gadget"

#define JW_MLP1_ROOTFS_RW_CMD \
    "(mount -o remount,rw / >/dev/null 2>&1 || " \
    "mount -o remount,rw /dev/root / >/dev/null 2>&1)"
#define JW_MLP1_ADB_ENABLE_CMD \
    JW_MLP1_ROOTFS_RW_CMD " || exit 1; " \
    "chattr -i " JW_MLP1_USB_CONFIG " >/dev/null 2>&1 || true; " \
    "printf 'usb_adb_en\\n' >" JW_MLP1_USB_CONFIG " && " \
    "chattr +i " JW_MLP1_USB_CONFIG " && sync"
#define JW_MLP1_ADB_DISABLE_CMD \
    JW_MLP1_ROOTFS_RW_CMD " || exit 1; " \
    "chattr -i " JW_MLP1_USB_CONFIG " >/dev/null 2>&1 || true; " \
    "printf 'usb_mtp_en\\n' >" JW_MLP1_USB_CONFIG " && sync"
#define JW_MLP1_USB_RESTART_CMD \
    JW_MLP1_USB_GADGET " restart >/dev/null 2>&1 || " \
    JW_MLP1_USB_GADGET " start >/dev/null 2>&1"

/* The Network settings ADB toggle is offered when both stock tools are there.
   Takes the paths so the test can point it at a fake rootfs. */
static inline bool jw_mlp1_adb_tools_present(const char *adbd, const char *gadget) {
    return access(adbd, X_OK) == 0 && access(gadget, X_OK) == 0;
}

#endif
