/* The MLP1 platform backend's xylem side (OS=xylem, -DJW_OS_XYLEM).
 *
 * xylem is a mainline-kernel Buildroot distro. None of stock LoongOS's vendor
 * services exist there: no loong_power, loong_light or loong_transition, no
 * libloong_sdk or /oem/loong/loong.db, no "Loong Gamepad" module, no
 * /sys/kernel/powerCtrl/spk_ctl and none of the rk817 vendor codec numids.
 * device_mlp1.c keeps what is the same board on both kernels (backlight,
 * performance, the PMIC hold time, storage) and calls in here, under
 * JW_OS_XYLEM, for the parts the vendor services did on stock.
 *
 * What mainline offers instead, as measured on the Green Genes image:
 *   - rk817-battery and rk817-charger (type USB), found by type;
 *   - card 0 "rk817_ext": "Master Playback Volume", "Playback Mux" (HP/SPK),
 *     "Internal Speakers Switch", "Headphones Jack"; the one speaker hangs off
 *     the codec's headphone outputs, so the mux stays HP for both outputs and
 *     the speaker switch follows the jack ("rk817_ext Headphones" evdev);
 *   - PulseAudio through the default client config, so pactl works as on stock;
 *   - deep suspend with wake by the power key, which leaves the panel lit
 *     unless the backlight is blanked first.
 *
 * Only built for PLATFORM=mlp1 OS=xylem. */
#ifndef JW_PLATFORM_DEVICE_MLP1_XYLEM_H
#define JW_PLATFORM_DEVICE_MLP1_XYLEM_H

#include <stdbool.h>
#include <stddef.h>

#include "internal/platform/device.h"

/* Backend init and shutdown: the codec routing, and the jack device. */
void jw_mlp1_xylem_init(void);
void jw_mlp1_xylem_shutdown(void);

/* Battery percent and charger state, from the supplies found by type. Leaves
   a field alone when it cannot be read. */
void jw_mlp1_xylem_get_power_status(jw_platform_status *out);
/* 1 when a charger input is online, 0 when none is, -1 when there is none. */
int  jw_mlp1_xylem_charger_online(void);

/* Actions stock's vendor services carried and xylem has nothing behind yet;
   the backend answers them UNSUPPORTED. */
bool jw_mlp1_xylem_action_unsupported(jw_platform_action action);

/* Audio: speaker and wired headphones on the rk817, nothing else yet. */
jw_platform_audio_output jw_mlp1_xylem_audio_output(void);
unsigned jw_mlp1_xylem_audio_outputs(void);
/* Route to the speaker or the headphones. 0 on success, -1 otherwise
   (including any other output). */
int  jw_mlp1_xylem_route_audio(jw_platform_audio_output output);
/* The headphone jack's input device, for the daemon's poll set; -1 if none. */
int  jw_mlp1_xylem_jack_fd(void);
/* Follow a jack edge; returns JW_PLATFORM_AUDIO_EVENT_* bits. */
unsigned jw_mlp1_xylem_audio_tick(void);
/* Re-read the jack and put the route back where it belongs. */
void jw_mlp1_xylem_audio_reconcile(const char *reason);
/* True when the default PulseAudio sink has its device open (RUNNING or
   IDLE), so suspending it around a system sleep closes the device. */
bool jw_mlp1_xylem_sink_open(void);

/* Cards. The launcher root (SDCARD_PATH) may be a directory on the root
   filesystem rather than a card of its own. The second slot's card has one
   mount point, $UMRK_SECONDARY_SDCARD_PATH, else /roms (where Green Genes'
   image mounts it); stock's /mnt/sdcard and /media/sdcard1 swap does not
   exist here. */
const char *jw_mlp1_xylem_secondary_mount(void);
/* The first partition of the card in the second slot, by its SD controller
   (stock's mmcblk numbers do not hold on mainline), as /dev/mmcblkNp1; false
   when the slot is empty. */
bool jw_mlp1_xylem_secondary_block(char *out, size_t out_size);
/* The device of the mount holding `path` (the longest mount point that is
   `path` or one of its parents); false when /proc/mounts can't be read. */
bool jw_mlp1_xylem_containing_mount(const char *path, char *device, size_t device_size);

/* Suspend to RAM: blank the backlight, write `mem`, unblank and put
   `restore_brightness_raw` back (when > 0). Blocks until resume. 0 when the
   kernel suspended and resumed. The caller suspends and resumes the
   PulseAudio sink around it. */
int  jw_mlp1_xylem_suspend(int restore_brightness_raw);

#endif /* JW_PLATFORM_DEVICE_MLP1_XYLEM_H */
