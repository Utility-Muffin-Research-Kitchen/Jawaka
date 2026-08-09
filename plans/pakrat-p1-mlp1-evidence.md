# P1 Pak Rat recovery — MLP1 qualification

Date: 2026-07-29

Status: code and required physical qualification complete; review and merge of
draft PR #34 remain.

## Qualified build and hardware

- Jawaka branch: `agent/pakrat-commit-token`. The original physical-pull run
  used the tracked working-tree content subsequently committed as `8155ac7`;
  the full automated device matrix was rerun from exact review-fix commit
  `c3f7644`.
- MLP1 ADB serial: `b1622a9e81b735ad`; Buildroot, Linux 5.10.209.
- Expendable 128 GB FAT card CID:
  `0353445344313238852b8a0b0c019b13` (`/dev/mmcblk1p1`).
- Matrix mount: `/media/sdcard1`; the removal fixture was prepared while the
  same card was `/mnt/sdcard` and recovered after reboot at
  `/media/sdcard1`.
- `jawakad` SHA-256, used for the daemon-start recovery scenario (it does not
  link `pakrat.c` or the injection hook):
  `f4c33b00ab9e75c5c1f7ac8af4f4f634661a57587ecc08efed6c8d10b8a00769`.
- `jawaka-pakrat-smoke` SHA-256, the load-bearing injection-capable artifact:
  `db7ef56ebb28328e0919ac2f85d14be0a774dbdb20b69db90c5d03eace25024b`.
- Both binaries are AArch64 ELF executables targeting GNU/Linux 5.10.
- Production `jawaka-launcher` SHA-256:
  `56a88bedef74cdb7901afde1b4edcc2718cb2b2657767158fe547a21cb1f6b96`.
- Production `jawaka-menu` SHA-256:
  `22ab63ae57a82db4a030139e1fbbfba811e7c5cde7c34fd58ce4541087fae4f1`.

## Automated coverage

The native recovery smoke passes 28 scenarios. The added scenario stops the
installer at the exact `after-promote` boundary and proves that the new tree is
live while the durable record still identifies the old tree before recovery.

Review follow-up commit `c3f7644` makes fault injection a build-time-only smoke
feature. Exact native and MLP1 production builds contain neither
`JW_PAKRAT_FAULT_AT` nor `JW_PAKRAT_PAUSE_AT` in `jawaka-launcher` or
`jawaka-menu`; the dedicated smoke binary contains both and reruns all 28
native and 24 real-vfat device scenarios. A stray production environment
variable can therefore neither crash nor stop a shipped UI process.

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

## Final main-integration qualification — 2026-08-09

P1 was merged with current Jawaka `main` in code merge commit
`c966f94276b6c412b817b8545ed10f9d91bbc3d9`. The merge retained the current
Release A service-supervisor targets and P1's smoke-only fault-injection target,
and removed the duplicate `.PHONY` declaration that existed on `main`.

Two final review findings were addressed before qualification:

- MLP1/Linux still requires a successful `syncfs` barrier. On non-Linux native
  hosts only, an unsupported directory `fsync` returning `EINVAL` is treated as
  best effort so a host install does not fail solely because that platform
  rejects directory syncing.
- `removal-recover` now chains its retained-underlay precondition with `&&`.
  A negative device run with no retained fixture returned nonzero before setup
  and left no bind mount, runner directory, evidence directory, or card fixture.

The exact merged tree passed:

- all 28 native recovery scenarios, both normally and under Clang
  ASan/UBSan (`detect_leaks=0`, because macOS AddressSanitizer does not support
  leak detection);
- `schema-v6-test`, `pakrat-state-logic-test`, and `storage-sources-test`;
- shell syntax and ShellCheck for all three P1 runners;
- the full native build and full MLP1 release cross-build;
- production-binary inspection proving `jawaka-launcher` and `jawaka-menu`
  contain neither `JW_PAKRAT_FAULT_AT` nor `JW_PAKRAT_PAUSE_AT`, while the
  dedicated smoke binary contains both; and
- all 24 real-vfat scenarios on MLP1 `f40098e329c73533`, using the expendable
  64 GB card at `/mnt/sdcard`, CID
  `0034323030303030000000008a0186c5`. Cleanup left no P1 fixture or `/tmp`
  residue on the device.

The corresponding MLP1 binary SHA-256 values are:

- `jawakad`: `1a4b0676a2aed2156e999d621d32170a54f3563d10f3a181737a11eeb5d2110b`;
- `jawaka-launcher`: `a3729713715ae23690362c344efca3e52c42ecdbf481e22ca84ed89b332ab81c`;
- `jawaka-menu`: `8a158e6401f54359aef287957b5cb2e3b4e6d9fa57b1b08b72c160549d1b589d`;
  and
- `jawaka-pakrat-smoke`: `568fb01de599fb9c8c37c1ad7ca5b10c56cd3bdf254f9bcd7ec3b2b7f10e7eba`.

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
