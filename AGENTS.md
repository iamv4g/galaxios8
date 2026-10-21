# Working on galaxios8 (iOS on the Galaxy S8)

- For Samsung SM-G950F (`dreamlte`, Exynos 8895) work, read `DREAMLTE_PROGRESS.md` first: it is the active handoff. Update it after each milestone and before stopping, and commit completed changes. Record boot numbers, CI run IDs and exact results, and keep observations apart from guesses and from "built but not tested on the phone".
- Keep build success, recovered kernel markers, physical screen reports and iOS runtime behavior as separate claims.
- The phone is written only through TWRP, and only BOOT `sda7` and USERDATA `sda24`. Never touch EFS, recovery, modem, persist or the bootloaders. Confirm each destructive step (flashing, formatting, `dd`) with the user before running it.
- Keep device dumps, serial numbers, private addresses, passwords and downloaded Apple binaries (IPSW, root images) out of git.
- `origin` is the `iamv4g/galaxios8` fork; `upstream` is `Floppikrob/ios-galaxy-s8`. Use `gh` for GitHub Actions kernel builds and logs, and local single-file cross-compiles before spending a CI run.
- Everything in `docs/history/` (Galaxy A5 2016 notes, SWEET donor `NOTES.md`) is history. Do not edit them to imply S8 behavior.

## Galaxy A5 2016 (A5XELTE) reference

- For SM-A510F work, read `docs/history/A5XELTE_PROGRESS.md` and `docs/history/a5xelte-migration-map.md` first and record new device evidence in the former.
- Preserve recovery p11, EFS p3 and the existing Linux BOOT checkpoint. XNU trials may write only the explicitly authorized BOOT p10; the donor's Xiaomi `fastboot` and USERDATA instructions are not A5XELTE instructions.

## Original SWEET contributor instructions (donor reference)

- Read `docs/history/NOTES.md` before starting; it contains the project goal, hardware facts,
  current blocker, local artifact locations and the next concrete action.
- The user switches agents frequently. Update the active handoff in `docs/history/NOTES.md`
  after each milestone and before stopping. Record commits, CI run IDs, exact
  build/boot results and unfinished work. Distinguish observations from guesses.
- Commit completed changes. Use `gh` for GitHub Actions kernel builds and logs.
  Local single-file cross-compiles are useful before spending a CI run.
- Device bring-up uses temporary `fastboot boot` images. Preserve the Android
  installation and partition backups. The user approved (2026-10-15) using
  internal UFS for iOS and said Android may be deleted: the iOS root goes
  into the existing `userdata` partition (no GPT changes). Keep bootloaders,
  recovery, modem, persist and the backups intact, and confirm each
  destructive step with the user before running it.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
