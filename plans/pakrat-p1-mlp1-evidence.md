# P1 Pak Rat recovery — MLP1 qualification

Date: 2026-07-29

Status: code and required physical qualification complete; review and merge of
draft PR #34 remain.

## Qualified build and hardware

- Jawaka branch: `agent/pakrat-commit-token`, based on PR #34 commit
  `c9bae61cb08ddb59a4d2e996cc9618b1f2b69cf6`.
- MLP1 ADB serial: `b1622a9e81b735ad`; Buildroot, Linux 5.10.209.
- Expendable 128 GB FAT card CID:
  `0353445344313238852b8a0b0c019b13` (`/dev/mmcblk1p1`).
- Matrix mount: `/media/sdcard1`; the removal fixture was prepared while the
  same card was `/mnt/sdcard` and recovered after reboot at
  `/media/sdcard1`.
- `jawakad` SHA-256:
  `f4c33b00ab9e75c5c1f7ac8af4f4f634661a57587ecc08efed6c8d10b8a00769`.
- `jawaka-pakrat-smoke` SHA-256:
  `db7ef56ebb28328e0919ac2f85d14be0a774dbdb20b69db90c5d03eace25024b`.
- Both binaries are AArch64 ELF executables targeting GNU/Linux 5.10.

## Automated coverage

The native recovery smoke passes 28 scenarios. The added scenario stops the
installer at the exact `after-promote` boundary and proves that the new tree is
live while the durable record still identifies the old tree before recovery.

The MLP1 runner bind-mounts its dedicated fixture directory from the selected
real vfat card and rejects any non-FAT or unsupported target. Its 24 scenarios
pass on device:

- every injected transition boundary from `before-stage` through
  `before-cleanup`, including both sides of `syncfs` and record publication;
- in-process `syncfs` and record failures;
- daemon-start recovery before the first scan;
- deterministic stopped-process recovery;
- truncated manifest and missing entry point;
- same-version repair before and after the record commit;
- first install, adoption, unidentifiable rollback, and orphan stage handling;
- actual fixture unmount with zero-mutation deferral until the source returns;
- update while a file in the old package remains open; and
- normal update and cleanup.

The host/device runner uses `env -i` with an explicit P1 environment. This is
required because an ADB shell launched by the stock daemon can inherit stale
Leaf storage lists that refer to the other card. The first manual recovery with
that inherited environment correctly deferred without mutation; the isolated
rerun recovered the intended mounted source.

## Physical unsafe-removal proof

Both old `0.1.0` and new `0.2.0` packages contained a 128 MiB random
`bulk.bin`. The old install was committed and flushed. The update then stopped
PID 31576 at `after-promote`, before `syncfs` or install-record publication.
Read-only inspection proved:

- live target version `0.2.0`;
- rollback version `0.1.0`;
- durable database record version `0.1.0`;
- old record token `3fa7ef5a3e1f4c4271e175a2e2bdd9d9`;
- new tree token `b24f9db38261a7cab1281e205f95e061`; and
- both live and rollback `bulk.bin` files were 134,217,728 bytes.

The card was physically removed while the installer remained stopped. The CID
vanished and the kernel reported FAT directory read failures. The stopped
installer was killed and the stale bind detached without a sync. After card
reinsertion and reboot, `fsck.vfat -a -v` cleared the dirty bit and reclaimed
unconnected clusters (`Filesystem was changed`, exit 1: repairs made).

Before recovery, the surviving filesystem still had the uncommitted `0.2.0`
target, the `0.1.0` rollback, both 128 MiB payloads, and the `0.1.0` database
record. The first isolated recovery restored the old tree. Final proof:

- target and database version `0.1.0`;
- payload `old-payload` and `bulk.bin` 134,217,728 bytes;
- tree marker and database token both
  `3fa7ef5a3e1f4c4271e175a2e2bdd9d9`; and
- zero `.pakrat-*` transition siblings.

This is the plan's power-loss/card-removal analogue: the exact pre-commit state
survived an unsafe physical removal and recovery chose the already-running tree
using the durable record token rather than package version or filesystem shape.

## Reproduction

The automated matrix is destructive only inside `.p1-pakrat-fixture` on the
explicitly selected expendable card:

```sh
CONFIRM_P1_FAT_SMOKE=1 \
ADB_SERIAL=b1622a9e81b735ad \
P1_SDCARD_PATH=/media/sdcard1 \
make mlp1-adb-pakrat-recovery-smoke
```

The physical-removal flow uses the same runner with
`P1_MODE=removal-prepare P1_LARGE_BYTES=134217728`, followed after reinsertion
and FAT repair by `P1_MODE=removal-recover`. The prepare mode deliberately
retains the fixture and stopped process so the operator—not the script—controls
the pull.

Build evidence under `build/p1-mlp1-evidence-*` is intentionally ignored and
not committed; this document records the durable qualification result.
