#!/usr/bin/env python3
"""Exercise deletion IPC against a real daemon and disposable ROMs only."""

import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


PREPARING, READY, COMMITTING, DONE, ERROR, CANCELLED = range(6)


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(3)
        self.sock.connect(str(path))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.sock.close()

    def receive(self, size):
        result = bytearray()
        while len(result) < size:
            chunk = self.sock.recv(size - len(result))
            if not chunk:
                raise RuntimeError("daemon closed the retained deletion connection")
            result.extend(chunk)
        return bytes(result)

    def request(self, kind, **fields):
        data = json.dumps({"type": kind, **fields}, separators=(",", ":")).encode()
        self.sock.sendall(struct.pack("!I", len(data)) + data)
        size = struct.unpack("!I", self.receive(4))[0]
        assert size <= 16 * 1024 * 1024, size
        return json.loads(self.receive(size))

    def settle(self, reply):
        deadline = time.monotonic() + 15
        while reply.get("phase") in (PREPARING, COMMITTING):
            assert time.monotonic() < deadline, reply
            time.sleep(0.02)
            reply = self.request("rom-delete-status")
        return reply

    def preview(self, path, source="primary", disc=None):
        fields = {"source_id": source, "rom_relpath": path}
        if disc is not None:
            fields["disc"] = disc
        return self.settle(self.request("rom-delete-preview", **fields))

    def commit(self, token):
        return self.settle(self.request("rom-delete-commit", token=token))


def rejected(reply):
    assert reply.get("phase") == ERROR or reply.get("type") == "error", reply


def ready(reply):
    assert reply.get("phase") == READY and reply.get("token"), reply
    return reply["token"]


def write(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents if isinstance(contents, bytes) else contents.encode())


class Fixture:
    def __init__(self, root, daemon):
        self.root, self.daemon = root, daemon
        self.primary, self.secondary = root / "primary", root / "secondary"
        self.runtime, self.state, self.platform = root / "runtime", root / "state", root / "platform"
        self.socket, self.db_path = self.runtime / "d.sock", self.state / "library.db"
        self.log, self.proc, self.log_file = root / "daemon.log", None, None
        self.readonly_marker = root / "readonly-source"
        self.failure_marker = root / "fail-after"
        self.rename_failure_marker = root / "fail-before-rename"
        for directory in (self.runtime, self.state, self.platform / "defaults",
                          self.primary / "Apps/shared", self.secondary / "Roms/GBA"):
            directory.mkdir(parents=True)
        for name in ("Delete", "Keep", "Cancel", "Stale", "ReadOnly", "LostResponse", "Serialize"):
            write(self.primary / f"Roms/GBA/{name}.gba", f"{name} disposable payload\n")
        write(self.secondary / "Roms/GBA/Delete.gba", "Other card must survive\n")
        write(self.secondary / "Roms/GBA/ReadOnly.gba", "Readonly card must survive\n")
        write(self.primary / "Roms/PS/Owner.m3u", "Disc.cue\n")
        write(self.primary / "Roms/PS/Disc.cue", 'FILE "Track.bin" BINARY\n  TRACK 01 MODE2/2352\n')
        write(self.primary / "Roms/PS/Track.bin", b"track payload\n")
        write(self.primary / "Roms/PS/Partial.m3u", "PartialDisc.cue\n")
        write(self.primary / "Roms/PS/PartialDisc.cue", 'FILE "PartialTrack.bin" BINARY\n  TRACK 01 MODE2/2352\n')
        write(self.primary / "Roms/PS/PartialTrack.bin", b"partial payload\n")
        systems = []
        for code, extensions, playlists in (("GBA", ["gba", "zip", "7z"], []),
                                            ("PS", ["cue", "img", "pbp", "chd", "toc"], ["m3u"])):
            systems.append({
                "id": code, "name": code, "patterns": [code], "extensions": extensions,
                "archive_extensions": [], "archive_inner_extensions": extensions,
                "archive_mode": "pass_through", "file_names": [], "ignore_file_names": [],
                "playlist_extensions": playlists, "m3u_generation": "none",
                "default_core": "fixture", "alternate_cores": [],
                "rom_root": f"Roms/{code}", "image_root": f"Images/{code}",
            })
        write(self.platform / "defaults/systems.json", json.dumps({"platform": "mac", "systems": systems}))
        write(self.platform / "defaults/cores.json", json.dumps({"platform": "mac", "cores": [{
            "id": "fixture", "display_name": "Fixture", "type": "retroarch",
            "file_name": "fixture_libretro.dylib", "config_folder": "Fixture", "status": "packaged",
        }]}))

    def start(self):
        self.socket.unlink(missing_ok=True)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("UMRK_", "JAWAKA_", "SDCARD_PATH", "ROMS_PATH",
                                      "USERDATA_PATH", "APPS_PATH", "IMAGES_PATH", "LOGS_PATH"))}
        env.update(PLATFORM="mac", SDCARD_PATH=str(self.primary),
                   SDCARD_PATHS=f"{self.primary}:{self.secondary}",
                   UMRK_RUNTIME_PATH=str(self.runtime), UMRK_DAEMON_SOCKET=str(self.socket),
                   UMRK_INTERNAL_DATA_PATH=str(self.state), UMRK_PLATFORM_PATH=str(self.platform),
                   USERDATA_PATH=str(self.primary / ".userdata/mac"),
                   JAWAKA_OSD="0", JAWAKA_SDCARD_ROOT=str(self.primary),
                   JAWAKA_TEST_DELETE_READONLY_FILE=str(self.readonly_marker),
                   JAWAKA_TEST_DELETE_FAIL_AFTER_FILE=str(self.failure_marker),
                   JAWAKA_TEST_DELETE_FAIL_BEFORE_RENAME_FILE=str(self.rename_failure_marker),
                   JAWAKA_TEST_DELETE_PREPARE_DELAY_MS="500",
                   JAWAKA_TEST_DELETE_COMMIT_DELAY_MS="500")
        self.log_file = self.log.open("ab")
        self.proc = subprocess.Popen([str(self.daemon), "--daemon-only"], env=env,
                                     cwd=Path(__file__).resolve().parents[1], stdout=self.log_file,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            assert self.proc.poll() is None, "daemon exited during startup"
            if self.socket.exists():
                try:
                    with Client(self.socket) as client:
                        status = client.request("library-status")
                    if status.get("generation", 0) > 0 and not status.get("scan_running", False):
                        assert "using compatibility scanner" not in self.log.read_text()
                        return status["generation"]
                except (ConnectionError, FileNotFoundError):
                    pass
            time.sleep(0.02)
        raise RuntimeError("daemon scan did not become idle")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=3)
                raise RuntimeError("daemon did not stop cleanly")
        if self.log_file:
            self.log_file.close()
        self.proc = None


def wait_idle(fixture, minimum_generation):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        with Client(fixture.socket) as client:
            status = client.request("library-status")
        if status.get("generation", 0) >= minimum_generation and not status.get("scan_running", False):
            return status["generation"]
        time.sleep(0.02)
    raise RuntimeError(f"queued scan did not finish: {status}")


def assert_serialized(fixture, owner, phase):
    for kind, fields in (
        ("library-relocate-prepare", {"operation_id": "deletion-busy", "expected_generation": 0,
                                     "items": [{"old": {"source_id": "primary", "rom_relpath": "GBA/Cancel.gba"},
                                                "new": {"source_id": "secondary_sd", "rom_relpath": "GBA/Cancel.gba"}}]}),
        ("storage-action", {"source": "secondary_sd", "action": "safe-unmount"}),
        ("launch-game", {"system": "GBA", "rom_path": "Roms/GBA/Cancel.gba"}),
    ):
        with Client(fixture.socket) as client:
            reply = client.request(kind, **fields)
        assert reply.get("type") == "error" and "ROM deletion" in reply.get("message", ""), reply
    with Client(fixture.socket) as client:
        queued = client.request("scan-library")
    assert queued.get("action") == "scan-library queued", queued
    with Client(fixture.socket) as client:
        status = client.request("library-status")
    assert not status.get("scan_running"), status
    assert owner.request("rom-delete-status").get("phase") == phase
    return status["generation"]


def test_serialization(fixture):
    with Client(fixture.socket) as client:
        preparing = client.request("rom-delete-preview", source_id="primary", rom_relpath="GBA/Cancel.gba")
        assert preparing.get("phase") == PREPARING, preparing
        generation = assert_serialized(fixture, client, PREPARING)
        ready(client.settle(preparing))
        client.request("rom-delete-cancel")
    wait_idle(fixture, generation + 1)
    with Client(fixture.socket) as client:
        token = ready(client.preview("GBA/Serialize.gba"))
        committing = client.request("rom-delete-commit", token=token)
        assert committing.get("phase") == COMMITTING, committing
        generation = assert_serialized(fixture, client, COMMITTING)
        result = client.settle(committing)
        assert result.get("phase") == DONE, result
    wait_idle(fixture, generation + 2)  # One deletion publication, then the queued scan.
    assert not (fixture.primary / "Roms/GBA/Serialize.gba").exists()


def test_partial_failure(fixture):
    primary = fixture.primary
    parent_path = primary / "Roms/PS/Partial.m3u"
    original_playlist = parent_path.read_bytes()
    with sqlite3.connect(fixture.db_path) as db:
        parent = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='PS/Partial.m3u'").fetchone()[0]
        keep = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='GBA/Keep.gba'").fetchone()[0]
        db.execute("INSERT INTO game_settings(game_id,key,value,updated_at) VALUES(?,'display_name','Keep parent metadata',1)", (parent,))
        db.execute("INSERT INTO favorites VALUES('game',?,1)", (parent,))
        db.execute("INSERT INTO recents VALUES('game',?,1,20)", (parent,))
        db.execute("INSERT OR REPLACE INTO settings VALUES('five_game_ids',?)", (f"{parent},{keep}",))
        for path, member in (("PS/PartialTrack.bin", ""), ("PS/PartialTrack.bin", "member"),
                             ("PS/PartialDisc.cue", ""), ("PS/Partial.m3u", "")):
            db.execute("INSERT INTO hidden_roms VALUES('primary',?,?)", (path, member))
    write(fixture.failure_marker, "1")
    with Client(fixture.socket) as client:
        token = ready(client.preview("PS/Partial.m3u"))
        result = client.commit(token)
        rejected(result)
        assert result.get("removed_count") == 1, result
    assert not (primary / "Roms/PS/PartialTrack.bin").exists()
    assert (primary / "Roms/PS/PartialDisc.cue").exists()
    assert parent_path.read_bytes() == original_playlist
    with sqlite3.connect(fixture.db_path) as db:
        assert db.execute("SELECT id FROM games WHERE id=?", (parent,)).fetchone()
        assert db.execute("SELECT value FROM game_settings WHERE game_id=? AND key='display_name'", (parent,)).fetchone()[0] == "Keep parent metadata"
        assert not db.execute("SELECT 1 FROM hidden_roms WHERE rom_relpath='PS/PartialTrack.bin'").fetchone()
        assert db.execute("SELECT 1 FROM hidden_roms WHERE rom_relpath='PS/PartialDisc.cue'").fetchone()
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == f"{parent},{keep}"
    fixture.failure_marker.unlink()
    with Client(fixture.socket) as client:
        preview = client.preview("PS/Partial.m3u")
        token = ready(preview)
        assert any(item.get("missing") and item["rom_relpath"] == "PS/PartialTrack.bin" for item in preview["files"]), preview
        result = client.commit(token)
        assert result.get("phase") == DONE and result.get("removed_count") == 2, result
        assert result.get("absent_count") == 1, result
    assert not parent_path.exists() and not (primary / "Roms/PS/PartialDisc.cue").exists()
    with sqlite3.connect(fixture.db_path) as db:
        assert not db.execute("SELECT id FROM games WHERE id=?", (parent,)).fetchone()
        assert not db.execute("SELECT 1 FROM hidden_roms WHERE rom_relpath LIKE 'PS/Partial%'").fetchone()
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == str(keep)


def disc_key(path, member="", source="primary"):
    return {"source_id": source, "rom_relpath": path, "member": member}


def add_playlist(fixture, filename, contents, payloads):
    relative = f"PS/{filename}"
    write(fixture.primary / "Roms" / relative, contents)
    for name, data in payloads.items():
        write(fixture.primary / "Roms/PS" / name, data)
    # Add only the requested root, without a discovery pass inventing owners.
    with sqlite3.connect(fixture.db_path) as db:
        result = db.execute("INSERT INTO games(system,name,source_id,rom_relpath,rom_path,playtime_s) "
                            "VALUES('PS',?,'primary',?,?,123)", (filename, relative, f"Roms/{relative}"))
        game_id = result.lastrowid
        db.execute("INSERT INTO favorites VALUES('game',?,17)", (game_id,))
        db.execute("INSERT INTO recents VALUES('game',?,19,23)", (game_id,))
        db.execute("INSERT INTO game_settings(game_id,key,value,updated_at) VALUES(?,'display_name',?,29)",
                   (game_id, filename))
    return relative, game_id


def assert_parent_metadata(fixture, game_id):
    with sqlite3.connect(fixture.db_path) as db:
        assert db.execute("SELECT playtime_s FROM games WHERE id=?", (game_id,)).fetchone()[0] == 123
        assert db.execute("SELECT added_at FROM favorites WHERE kind='game' AND target_id=?", (game_id,)).fetchone()[0] == 17
        assert db.execute("SELECT duration_s FROM recents WHERE kind='game' AND target_id=?", (game_id,)).fetchone()[0] == 23
        assert db.execute("SELECT updated_at FROM game_settings WHERE game_id=?", (game_id,)).fetchone()[0] == 29


def library_generation(fixture):
    with Client(fixture.socket) as client:
        return client.request("library-status")["generation"]


def test_disc_edits(fixture):
    original = b'\xef\xbb\xbf#EXTM3U\r\n"Edit One.cue"|First label\r\n#SAVEDISK:Save Disk\r\nEditTwo.chd|Hidden second\r\n# trailing\r\n'
    expected = b'\xef\xbb\xbf#EXTM3U\r\n#SAVEDISK:Save Disk\r\nEditTwo.chd|Hidden second\r\n# trailing\r\n'
    relative, parent = add_playlist(fixture, "DiscEdit.m3u", original, {
        "Edit One.cue": 'FILE "EditTrack.bin" BINARY\r\n', "EditTrack.bin": b"first payload", "EditTwo.chd": b"second payload",
    })
    playlist = fixture.primary / "Roms" / relative
    with sqlite3.connect(fixture.db_path) as db:
        keep = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='GBA/Keep.gba'").fetchone()[0]
        db.execute("INSERT OR REPLACE INTO settings VALUES('five_game_ids',?)", (f"{parent},{keep}",))
        for path in (relative, "PS/Edit One.cue", "PS/EditTrack.bin", "PS/EditTwo.chd"):
            db.execute("INSERT INTO hidden_roms VALUES('primary',?,'')", (path,))
    for malformed in (None, [], {"source_id": "primary", "rom_relpath": "PS/Edit One.cue"},
                      {"source_id": "primary", "rom_relpath": "PS/Edit One.cue", "member": None}):
        with Client(fixture.socket) as client:
            rejected(client.settle(client.request("rom-delete-preview", source_id="primary",
                                                 rom_relpath=relative, disc=malformed)))
        assert playlist.read_bytes() == original
    with Client(fixture.socket) as client:
        rejected(client.preview(relative, disc=disc_key("PS/NotAMember.chd")))
    with Client(fixture.socket) as client:
        preview = client.preview(relative, disc=disc_key("PS/Edit One.cue"))
        token = ready(preview)
        assert preview.get("playlist_edit") and not preview.get("final_disc"), preview
        assert preview.get("remaining_discs") == 1, preview  # Hidden second disc still counts.
        assert preview.get("disc_name") == "First label", preview
        assert playlist.read_bytes() == original
        result = client.commit(token)
        assert result.get("phase") == DONE and result.get("removed_count") == 2, result
    assert playlist.read_bytes() == expected
    assert not (fixture.primary / "Roms/PS/Edit One.cue").exists()
    assert not (fixture.primary / "Roms/PS/EditTrack.bin").exists()
    assert_parent_metadata(fixture, parent)
    with sqlite3.connect(fixture.db_path) as db:
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == f"{parent},{keep}"
        hidden = {row[0] for row in db.execute("SELECT rom_relpath FROM hidden_roms WHERE rom_relpath LIKE 'PS/Edit%' OR rom_relpath=?", (relative,))}
        assert hidden == {relative, "PS/EditTwo.chd"}, hidden
    with Client(fixture.socket) as client:
        preview = client.preview(relative, disc=disc_key("PS/EditTwo.chd"))
        ready(preview)
        assert preview.get("final_disc") and not preview.get("playlist_edit"), preview
        assert preview.get("remaining_discs") == 0 and preview.get("file_count") == 2, preview
        assert playlist.read_bytes() == expected and (fixture.primary / "Roms/PS/EditTwo.chd").exists()
        client.request("rom-delete-cancel")
    assert playlist.exists()
    with Client(fixture.socket) as client:
        token = ready(client.preview(relative, disc=disc_key("PS/EditTwo.chd")))
        result = client.commit(token)
        assert result.get("phase") == DONE and result.get("removed_count") == 2, result
    assert not playlist.exists()
    with sqlite3.connect(fixture.db_path) as db:
        assert not db.execute("SELECT 1 FROM games WHERE id=?", (parent,)).fetchone()
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == str(keep)

    relative, parent = add_playlist(fixture, "DuplicateEdit.m3u", "DupFirst.chd|One\nDupFirst.chd|Again\nDupSecond.chd|Two\n", {
        "DupFirst.chd": b"one", "DupSecond.chd": b"two",
    })
    with Client(fixture.socket) as client:
        preview = client.preview(relative, disc=disc_key("PS/DupFirst.chd"))
        token = ready(preview)
        assert preview.get("remaining_discs") == 1, preview
        result = client.commit(token)
        assert result.get("phase") == DONE and result.get("removed_count") == 1, result
    assert (fixture.primary / "Roms" / relative).read_text() == "DupSecond.chd|Two\n"
    assert_parent_metadata(fixture, parent)

    # Two selectors share one physical archive, so only the playlist changes.
    relative, parent = add_playlist(fixture, "ArchiveEdit.m3u", "EditArchive.zip#Disc A.cue|Disc A\nEditArchive.zip#Disc B.cue|Disc B\n", {
        "EditArchive.zip": b"shared archive payload",
    })
    with sqlite3.connect(fixture.db_path) as db:
        for member in ("Disc A.cue", "Disc B.cue"):
            db.execute("INSERT INTO hidden_roms VALUES('primary','PS/EditArchive.zip',?)", (member,))
    before = library_generation(fixture)
    with Client(fixture.socket) as client:
        preview = client.preview(relative, disc=disc_key("PS/EditArchive.zip", "Disc A.cue"))
        token = ready(preview)
        assert preview.get("playlist_edit") and preview.get("bytes") == 0, preview
        result = client.commit(token)
        assert result.get("phase") == DONE and result.get("removed_count") == 0, result
        assert result.get("playlist_replaced"), result
    assert library_generation(fixture) > before, "playlist-only edit did not publish its generation"
    assert (fixture.primary / "Roms" / relative).read_text() == "EditArchive.zip#Disc B.cue|Disc B\n"
    assert (fixture.primary / "Roms/PS/EditArchive.zip").read_bytes() == b"shared archive payload"
    assert_parent_metadata(fixture, parent)
    with sqlite3.connect(fixture.db_path) as db:
        assert db.execute("SELECT COUNT(*) FROM hidden_roms WHERE rom_relpath='PS/EditArchive.zip'").fetchone()[0] == 2

    relative, _ = add_playlist(fixture, "OwnedEdit.m3u", "OwnedOne.chd\nOwnedTwo.chd\n", {
        "OwnedOne.chd": b"one", "OwnedTwo.chd": b"two",
    })
    add_playlist(fixture, "Playlist owner.m3u", "OwnedEdit.m3u\n", {})
    with Client(fixture.socket) as client:
        result = client.preview(relative, disc=disc_key("PS/OwnedOne.chd"))
        rejected(result)
        assert "Playlist owner" in result.get("error", ""), result
    assert (fixture.primary / "Roms/PS/OwnedOne.chd").exists()

    relative, _ = add_playlist(fixture, "StaleEdit.m3u", "StaleOne.chd\nStaleTwo.chd\n", {
        "StaleOne.chd": b"one", "StaleTwo.chd": b"two",
    })
    with Client(fixture.socket) as client:
        token = ready(client.preview(relative, disc=disc_key("PS/StaleOne.chd")))
        write(fixture.primary / "Roms" / relative, "# changed\nStaleOne.chd\nStaleTwo.chd\n")
        rejected(client.commit(token))
    assert (fixture.primary / "Roms/PS/StaleOne.chd").exists()


def test_disc_parent_reservation(fixture):
    contents = b"ReservedOne.chd\nReservedTwo.chd\n"
    relative, _ = add_playlist(fixture, "ReservedEdit.m3u", contents, {
        "ReservedOne.chd": b"one", "ReservedTwo.chd": b"two",
    })
    with Client(fixture.socket) as client:
        reservation = client.request("library-relocate-prepare", operation_id="reserved-disc-parent",
            expected_generation=library_generation(fixture), items=[{
                "old": {"source_id": "primary", "rom_relpath": relative},
                "new": {"source_id": "secondary_sd", "rom_relpath": relative},
            }])
    assert reservation.get("state") == "prepared", reservation
    try:
        with Client(fixture.socket) as client:
            preview = client.preview(relative, disc=disc_key("PS/ReservedOne.chd"))
            if preview.get("phase") == READY:
                rejected(client.commit(ready(preview)))
            else:
                rejected(preview)
        assert (fixture.primary / "Roms" / relative).read_bytes() == contents
        assert (fixture.primary / "Roms/PS/ReservedOne.chd").exists()
    finally:
        with Client(fixture.socket) as client:
            released = client.request("library-relocate-abort", operation_id="reserved-disc-parent")
        assert released.get("state") == "aborted", released


def test_disc_rename_failure(fixture):
    original = b"RetryDisc.cue|First\r\nRetryKeep.chd|Second\r\n"
    relative, parent = add_playlist(fixture, "RetryEdit.m3u", original, {
        "RetryDisc.cue": 'FILE "RetryTrack.bin" BINARY\n', "RetryTrack.bin": b"remove me", "RetryKeep.chd": b"keep me",
    })
    playlist = fixture.primary / "Roms" / relative
    with sqlite3.connect(fixture.db_path) as db:
        db.execute("INSERT INTO hidden_roms VALUES('primary','PS/RetryDisc.cue','')")
        db.execute("INSERT INTO hidden_roms VALUES('primary','PS/RetryTrack.bin','')")
    write(fixture.rename_failure_marker, "1")
    with Client(fixture.socket) as client:
        token = ready(client.preview(relative, disc=disc_key("PS/RetryDisc.cue")))
        result = client.commit(token)
        rejected(result)
        assert result.get("removed_count") == 2 and not result.get("playlist_replaced"), result
    assert playlist.read_bytes() == original
    assert not (fixture.primary / "Roms/PS/RetryDisc.cue").exists()
    assert not (fixture.primary / "Roms/PS/RetryTrack.bin").exists()
    assert_parent_metadata(fixture, parent)
    with sqlite3.connect(fixture.db_path) as db:
        assert not db.execute("SELECT 1 FROM hidden_roms WHERE rom_relpath IN ('PS/RetryDisc.cue','PS/RetryTrack.bin')").fetchone()
    fixture.rename_failure_marker.unlink()
    fixture.stop()
    fixture.start()
    with Client(fixture.socket) as client:
        preview = client.preview(relative, disc=disc_key("PS/RetryDisc.cue"))
        token = ready(preview)
        assert preview.get("missing_descriptors"), preview
        result = client.commit(token)
        assert result.get("phase") == DONE and result.get("removed_count") == 0, result
        assert result.get("absent_count") == 1, result  # Missing CUE cannot reveal its former track.
    assert playlist.read_bytes() == b"RetryKeep.chd|Second\r\n"
    assert (fixture.primary / "Roms/PS/RetryKeep.chd").read_bytes() == b"keep me"
    assert_parent_metadata(fixture, parent)


def run(fixture):
    initial_generation = fixture.start()
    primary = fixture.primary
    test_serialization(fixture)
    test_partial_failure(fixture)

    with Client(fixture.socket) as client:
        token = ready(client.preview("GBA/Cancel.gba"))
        assert client.request("rom-delete-cancel").get("phase") == CANCELLED
        rejected(client.commit(token))
    assert (primary / "Roms/GBA/Cancel.gba").exists()

    with Client(fixture.socket) as client:
        disconnected_token = ready(client.preview("GBA/Cancel.gba"))
    with Client(fixture.socket) as client:
        rejected(client.commit(disconnected_token))
    assert (primary / "Roms/GBA/Cancel.gba").exists()

    # Switching an owning socket to a legacy one-shot request must not leave
    # its token attached to a connection slot that another socket can reuse.
    with Client(fixture.socket) as client:
        detached_token = ready(client.preview("GBA/Cancel.gba"))
        try:
            client.request("library-status")
        except (RuntimeError, ConnectionError):
            pass  # The daemon deliberately closes an owner using another protocol.
    with Client(fixture.socket) as client:
        rejected(client.commit(detached_token))
    assert (primary / "Roms/GBA/Cancel.gba").exists()

    with Client(fixture.socket) as client:
        token = ready(client.preview("GBA/Stale.gba"))
        write(primary / "Roms/GBA/Stale.gba", "changed after review, must survive\n")
        rejected(client.commit(token))
        rejected(client.commit(token))
    assert (primary / "Roms/GBA/Stale.gba").read_text() == "changed after review, must survive\n"

    # The test daemon consults this marker through its storage guard only.
    for source in ("primary", "secondary_sd"):
        write(fixture.readonly_marker, source)
        with Client(fixture.socket) as client:
            result = client.preview("GBA/ReadOnly.gba", source)
            rejected(result)
            assert result.get("readonly_source") == ("launcher_sd" if source == "primary" else source), result
            assert not result.get("token"), result
        fixture.readonly_marker.unlink()
    with Client(fixture.socket) as client:
        token = ready(client.preview("GBA/ReadOnly.gba"))
        write(fixture.readonly_marker, "primary")
        result = client.commit(token)
        rejected(result)
        assert result.get("readonly_source") == "launcher_sd", result
        fixture.readonly_marker.unlink()
        rejected(client.commit(token))
    assert (primary / "Roms/GBA/ReadOnly.gba").exists()
    assert (fixture.secondary / "Roms/GBA/ReadOnly.gba").exists()

    missing_card = fixture.secondary.with_name("secondary-away")
    fixture.secondary.rename(missing_card)
    try:
        with Client(fixture.socket) as client:
            preview = client.preview("GBA/Cancel.gba")
            ready(preview)
            assert "secondary_sd" in preview.get("missing_sources", ""), preview
            client.request("rom-delete-cancel")
    finally:
        missing_card.rename(fixture.secondary)

    # The descriptor bytes themselves participate in the commit comparison.
    with Client(fixture.socket) as client:
        token = ready(client.preview("PS/Owner.m3u"))
        write(primary / "Roms/PS/Owner.m3u", "# changed label\nDisc.cue\n")
        rejected(client.commit(token))
    assert (primary / "Roms/PS/Track.bin").exists()

    # A separate launch row cannot remove a descriptor needed by a playlist.
    with sqlite3.connect(fixture.db_path) as db:
        db.execute("INSERT OR IGNORE INTO games(system,name,source_id,rom_relpath,rom_path) "
                   "VALUES('PS','Standalone disc','primary','PS/Disc.cue','Roms/PS/Disc.cue')")
    with Client(fixture.socket) as client:
        blocked = client.preview("PS/Disc.cue")
        rejected(blocked)
        assert "Owner" in blocked.get("error", ""), blocked

    protected = {
        primary / "Roms/GBA/Delete.png": b"ROM-directory artwork",
        primary / "Images/GBA/Delete.png": b"image-root artwork",
        primary / "Saves/GBA/Delete.sav": b"save progress",
        primary / "States/GBA/Delete.state": b"save state",
        primary / "States/GBA/Delete.state.png": b"state thumbnail",
        fixture.secondary / "Roms/GBA/Delete.gba": b"Other card must survive\n",
    }
    for path, contents in protected.items():
        write(path, contents)
    with sqlite3.connect(fixture.db_path) as db:
        victim = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='GBA/Delete.gba'").fetchone()[0]
        survivor = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='GBA/Keep.gba'").fetchone()[0]
        db.execute("UPDATE games SET image_root_kind='roms',image_relpath='GBA/Delete.png',"
                   "image_path='Roms/GBA/Delete.png' WHERE id=?", (victim,))
        for game_id in (victim, survivor):
            db.execute("INSERT INTO favorites VALUES('game',?,1)", (game_id,))
            db.execute("INSERT INTO recents VALUES('game',?,1,20)", (game_id,))
            db.execute("INSERT INTO game_settings(game_id,key,value,updated_at) VALUES(?,'core','fixture',1)", (game_id,))
        for key, value in (("five_game_ids", f"{victim},{survivor}"), ("five_game_lock", "pin"),
                           ("five_game_pin_hash", "keep-this-hash")):
            db.execute("INSERT OR REPLACE INTO settings VALUES(?,?)", (key, value))
        for member in ("", "archive member"):
            db.execute("INSERT INTO hidden_roms VALUES('primary','GBA/Delete.gba',?)", (member,))
        db.execute("INSERT INTO hidden_roms VALUES('secondary_sd','GBA/Delete.gba','')")

    with Client(fixture.socket) as client:
        preview = client.preview("GBA/Delete.gba")
        token = ready(preview)
        assert preview["bytes"] == (primary / "Roms/GBA/Delete.gba").stat().st_size, preview
        assert preview["file_count"] == 1, preview
        result = client.commit(token)
        assert result.get("phase") == DONE and result.get("removed_count") == 1, result
        assert not (primary / "Roms/GBA/Delete.gba").exists()
        # An old token never authorizes deleting a replacement at the same path.
        write(primary / "Roms/GBA/Delete.gba", b"replacement must survive")
        rejected(client.commit(token))
        assert (primary / "Roms/GBA/Delete.gba").read_bytes() == b"replacement must survive"
    for path, contents in protected.items():
        assert path.read_bytes() == contents, path
    with sqlite3.connect(fixture.db_path) as db:
        assert db.execute("SELECT id FROM games WHERE id=?", (victim,)).fetchone() is None
        assert db.execute("SELECT id FROM games WHERE id=?", (survivor,)).fetchone()
        for table, key in (("favorites", "target_id"), ("recents", "target_id"), ("game_settings", "game_id")):
            assert not db.execute(f"SELECT 1 FROM {table} WHERE {key}=?", (victim,)).fetchone()
            assert db.execute(f"SELECT 1 FROM {table} WHERE {key}=?", (survivor,)).fetchone()
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == str(survivor)
        assert db.execute("SELECT value FROM settings WHERE key='five_game_pin_hash'").fetchone()[0] == "keep-this-hash"
        assert not db.execute("SELECT 1 FROM hidden_roms WHERE source_id='primary' AND rom_relpath='GBA/Delete.gba'").fetchone()
        assert db.execute("SELECT 1 FROM hidden_roms WHERE source_id='secondary_sd' AND rom_relpath='GBA/Delete.gba'").fetchone()
    with Client(fixture.socket) as client:
        status = client.request("library-status")
        assert status["generation"] > initial_generation, status

    # Lose the terminal result after commit acceptance. A reconnect cannot
    # replay the old authorization, even if a new file takes the same path.
    lost_file = primary / "Roms/GBA/LostResponse.gba"
    with Client(fixture.socket) as client:
        token = ready(client.preview("GBA/LostResponse.gba"))
        accepted = client.request("rom-delete-commit", token=token)
        assert accepted.get("phase") in (COMMITTING, DONE), accepted
    deadline = time.monotonic() + 10
    while lost_file.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert not lost_file.exists(), "accepted deletion never completed after disconnect"
    write(lost_file, b"replacement after lost response")
    with Client(fixture.socket) as client:
        rejected(client.commit(token))
    assert lost_file.read_bytes() == b"replacement after lost response"

    # A restart loses every in-memory review token as well.
    with Client(fixture.socket) as client:
        token = ready(client.preview("GBA/Cancel.gba"))
    fixture.stop()
    fixture.start()
    with Client(fixture.socket) as client:
        rejected(client.commit(token))
    assert (primary / "Roms/GBA/Cancel.gba").exists()

    test_disc_edits(fixture)
    test_disc_parent_reservation(fixture)
    test_disc_rename_failure(fixture)


def main():
    repo = Path(__file__).resolve().parents[1]
    daemon = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo / "build/bin/jawakad-delete-test"
    assert daemon.is_file(), f"build the daemon first: {daemon}"
    # Keep AF_UNIX socket names within the host's small path limit.
    with tempfile.TemporaryDirectory(prefix="jw-del-ipc-", dir="/tmp") as directory:
        fixture = Fixture(Path(directory), daemon)
        try:
            run(fixture)
        except Exception:
            if fixture.log.exists():
                print(fixture.log.read_text(errors="replace"), file=sys.stderr)
            raise
        finally:
            fixture.stop()
    print("PASS rom-delete-ipc-smoke")


if __name__ == "__main__":
    main()
