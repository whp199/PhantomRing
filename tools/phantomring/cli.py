"""`pr`: the PhantomRing Linux-side command line. See docs/TOOLS.md."""

from __future__ import annotations

import argparse
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from . import launch as L
from . import proto
from .proto import GuestFlags, HostFlags, PadFlags, Status

EXIT_OK, EXIT_ERROR, EXIT_USAGE, EXIT_ABSENT, EXIT_TIMEOUT = 0, 1, 2, 3, 4


def open_link(path: str | None) -> proto.Link | None:
	p = path or proto.default_path()
	try:
		return proto.Link(p)
	except proto.MappingMissing as e:
		print(f"no PhantomRing mapping: {e}\n  nothing has run since boot, or the mapping was removed; `pr launch` creates it "
			"(or run `pr fake-er --create` for a stand-in host)", file=sys.stderr)
		return None


def fmt_age(age) -> str:
	if age is None:
		return "n/a"
	return f"{age * 1000:.0f} ms ago" if age < 10 else f"{age:.1f} s ago"


def side_line(name: str, s: dict) -> str:
	pid = f"pid {s['pid']}{'' if s['pidAlive'] or not s['pid'] else ' (gone)'}" if s["pid"] else "no pid"
	beat = "never" if not s["heartbeat"] else fmt_age(s["heartbeatAge"])
	state = "ALIVE" if s["alive"] else "absent"
	return f"{name:<17} {state:<6} {s['statusName']:<8} {pid:<18} heartbeat {beat:<12} {s['text']!r}"


def v3(v, nd=2) -> str:
	return "(" + ", ".join(f"{x:.{nd}f}" for x in v) + ")"


def status_text(link: proto.Link, rates: dict | None = None) -> str:
	out = []
	h = link.snapshot(proto.Header, proto.OFF_HEADER)
	out.append(f"mapping   {link.path} ({proto.MAPPING_BYTES >> 20} MB)")
	if h.magic != proto.MAGIC:
		out.append(f"header    not stamped yet (magic {h.magic:#x}): the Elden Ring host has not attached since the mapping was created")
	else:
		ver = "" if h.version == proto.VERSION else f"  VERSION MISMATCH: mapping v{h.version}, tools v{proto.VERSION}"
		if proto.version_mismatch(h):
			ver += f"\n          {proto.version_mismatch(h)}"
		att = "guest attached" if h.guestNonce == h.sessionNonce and h.sessionNonce else ("guest not attached" if not h.guestNonce else "guest attached to an OLD session")
		out.append(f"header    v{h.version} session {h.sessionNonce:#018x} ({att}) qpcFreq {h.qpcFrequency}{ver}")
	host, guest = link.side("host"), link.side("guest")
	out.append(side_line("host (Elden Ring)", host))
	out.append(side_line("guest (MGSV)", guest))

	hs = link.host_state()
	if hs:
		out.append(f"host      frame {hs.frame} flags {proto.flag_names(hs.flags, HostFlags)} tod {hs.timeOfDayHours:.2f}h "
			f"hp {hs.erHpFrac:.2f} world {hs.erWorldId:#010x} camMax {hs.cameraMaxDistance:.1f} m groundY "
			f"{'n/a' if math.isnan(hs.groundY) else f'{hs.groundY:.2f}'} fall {hs.fallSpeed:.1f}")
	else:
		out.append("host      no HostState published")
	gs = link.guest_state()
	if gs:
		buddy = proto.enum_name(gs.buddyType, proto.BuddyType)
		vehicle = "none" if gs.vehicleId in (0xFFFF, 0xFFFFFFFF) else f"{gs.vehicleId:#x}"
		out.append(f"guest     frame {gs.frame} flags {proto.flag_names(gs.flags, GuestFlags)} mission {gs.missionCode} location {gs.locationCode}")
		out.append(f"          odo {v3(gs.odo)} epoch {gs.odoEpoch}  root {v3(gs.rootPos)} yaw {math.degrees(gs.rootYaw):.1f} deg  vel {v3(gs.velocity)}")
		out.append(f"          cam {v3(gs.camPos)} quat {v3(gs.camQuat, 3)} fov {math.degrees(gs.camFovY):.1f} deg near {gs.camNear:.2f}")
		out.append(f"          life {gs.life}/{gs.lifeMax} stamina {gs.stamina} stance {gs.stance} weapon {gs.weaponId:#x} ammo {gs.ammoInClip}/{gs.ammoStock} "
			f"phase {gs.phase}")
		out.append(f"          buddy {buddy} ({gs.buddyType}) flags {proto.flag_names(gs.buddyFlags, proto.GuestBuddyFlags)} "
			f"object {'none' if gs.buddyObjectId == 0xFFFF else f'{gs.buddyObjectId:#x}'} ammo {gs.buddyAmmo} pos {v3(gs.buddyPos)} "
			f"yaw {math.degrees(gs.buddyYaw):.1f} deg  vehicle {vehicle}{'  RIDING' if gs.flags & GuestFlags.OnVehicle else ''}")
	else:
		out.append("guest     no GuestState published")
	pad = link.pad_state()
	if pad:
		out.append(f"pad       packet {pad.packet} flags {proto.flag_names(pad.flags, PadFlags)} buttons {proto.flag_names(pad.buttons, proto.XButtons)} "
			f"LT {pad.leftTrigger} RT {pad.rightTrigger} L ({pad.thumbLX},{pad.thumbLY}) R ({pad.thumbRX},{pad.thumbRY})")
	pose = link.pose()
	actors = link.actors()
	out.append(f"pose      {f'{pose.boneCount} bones, frame {pose.frame}' if pose else 'none'}    actors {actors.count if actors else 'none'}")
	ge, he, rr = link.guest_events, link.host_events, link.render_ring
	out.append(f"events    guest->host head {ge.head} pending {ge.pending()}   host->guest head {he.head} pending {he.pending()}")
	gc, hc = link.guest_console, link.host_console
	out.append(f"consoles  lua #{gc.reqSeq}/{gc.respSeq}{' PENDING' if gc.reqSeq != gc.respSeq else ''}   er #{hc.reqSeq}/{hc.respSeq}{' PENDING' if hc.reqSeq != hc.respSeq else ''}")
	dc, hud = link.draw_ctl, link.hud_ctl
	line = (f"render    draw lists {dc.framesPublished} (state {dc.state:#x})  hud {hud.framesPublished}  render ring "
		f"{rr.used() / 1048576:.2f}/{rr.cap / 1048576:.0f} MB pending (head {rr.head})")
	if rates:
		line += f"  [{rates.get('draw', 0):.0f} lists/s, guest {rates.get('guest', 0):.0f} fps, host {rates.get('host', 0):.0f} fps]"
	out.append(line)
	return "\n".join(out)


def cmd_status(args) -> int:
	link = open_link(args.path)
	if not link:
		return EXIT_USAGE
	print(status_text(link))
	host, guest = link.side("host"), link.side("guest")
	return EXIT_OK if (host["alive"] or guest["alive"]) else EXIT_ABSENT


def cmd_watch(args) -> int:
	link = open_link(args.path)
	if not link:
		return EXIT_USAGE
	prev = None
	try:
		while True:
			gs, hs = link.guest_state(), link.host_state()
			now = time.monotonic()
			cur = (now, link.draw_ctl.framesPublished, gs.frame if gs else 0, hs.frame if hs else 0)
			rates = None
			if prev and cur[0] > prev[0]:
				dt = cur[0] - prev[0]
				rates = {"draw": (cur[1] - prev[1]) / dt, "guest": (cur[2] - prev[2]) / dt, "host": (cur[3] - prev[3]) / dt}
			prev = cur
			text = status_text(link, rates)
			sys.stdout.write("\x1b[H\x1b[2J" + time.strftime("%H:%M:%S") + "  (Ctrl-C to quit)\n" + text + "\n")
			sys.stdout.flush()
			time.sleep(args.interval)
	except KeyboardInterrupt:
		return EXIT_OK


def console_cmd(args, which: str) -> int:
	link = open_link(args.path)
	if not link:
		return EXIT_USAGE
	if which == "guest":
		if args.file:
			text = Path(args.file).read_text()
		else:
			text = " ".join(args.code)
		if args.ret and not text.lstrip().startswith("return"):
			text = "return " + text
	else:
		text = " ".join(args.command)
	if not text.strip():
		print("nothing to send", file=sys.stderr)
		return EXIT_USAGE
	side = link.side(which)
	game = "MGSV guest (Lua console)" if which == "guest" else "Elden Ring host (command console)"
	if not side["alive"] and not args.force:
		why = f"status {side['statusName']}"
		if side["heartbeat"]:
			why += f", last heartbeat {fmt_age(side['heartbeatAge'])}"
		else:
			why += ", never heartbeat"
		if side["pid"] and not side["pidAlive"]:
			why += f", pid {side['pid']} gone"
		print(f"{game} is not running ({why}); nothing would answer. --force sends anyway.", file=sys.stderr)
		return EXIT_ABSENT
	try:
		status, response = proto.console_request(link, which, text, timeout=args.timeout, force=args.force)
	except proto.ConsoleError as e:
		print(f"{game}: {e}", file=sys.stderr)
		return EXIT_TIMEOUT
	if status == 0:
		if response:
			print(response)
		return EXIT_OK
	print(f"error: {response}", file=sys.stderr)
	return EXIT_ERROR


def cmd_launch(args) -> int:
	opts = L.LaunchOptions(mgsv=not args.er_only, er=not args.mgsv_only, dry_run=args.dry_run, width=args.width,
		height=args.height, wait=args.wait, seed_save=args.seed_save, keep_backups=args.keep_backups)
	if args.er_headless:
		w, _, h = args.er_headless.partition("x")
		opts.er_headless = (int(w), int(h))
	if args.proton_debug is not None:
		opts.proton_debug = args.proton_debug
	if args.shm:
		os.environ["PHANTOMRING_SHM"] = args.shm  # proto.default_path() and the games' PHANTOMRING_MAPPING follow it
	try:
		return L.launch(opts)
	except L.LaunchError as e:
		print(f"launch aborted: {e}", file=sys.stderr)
		return EXIT_ERROR


def cmd_stop(args) -> int:
	only = "er" if args.er else "mgsv" if args.mgsv else None
	return L.stop(timeout=args.timeout, kill=args.kill, only=only)


def tail(path: Path, n: int) -> list[str]:
	try:
		with open(path, "rb") as f:
			f.seek(0, os.SEEK_END)
			size = f.tell()
			f.seek(max(0, size - 256 * 1024))
			return f.read().decode("utf-8", "replace").splitlines()[-n:]
	except OSError as e:
		return [f"<{e}>"]


def log_files() -> list[Path]:
	files: list[Path] = []
	current = L.LOGS / "current"
	if current.exists():
		files += sorted(p for p in current.resolve().glob("*.log"))
	for cand in L.plugin_log_candidates():
		if cand.is_dir():
			newest = sorted(cand.glob("*.log"), key=lambda p: p.stat().st_mtime)
			files += newest[-1:]
		elif cand.exists():
			files.append(cand)
	return files


def cmd_logs(args) -> int:
	files = log_files()
	if not files:
		print("no logs yet (logs/current is created by pr launch; plugin logs by the plugins)")
		return EXIT_ABSENT
	for f in files:
		print(f"==> {f} <==")
		for line in tail(f, args.lines):
			print(line)
		print()
	if args.follow:
		cmd = ["tail", "-n", "0", "-F", *map(str, files)]
		try:
			return subprocess.call(cmd)
		except KeyboardInterrupt:
			return EXIT_OK
	return EXIT_OK


def cmd_restore(args) -> int:
	er = not args.mgsv_only
	mgsv = not args.er_only
	src = L.BACKUPS / args.timestamp
	if not (src / "manifest.json").exists():
		avail = sorted(p.name for p in L.BACKUPS.glob("*") if (p / "manifest.json").exists()) if L.BACKUPS.exists() else []
		print(f"no backup {src}; available: {', '.join(avail) or 'none'}", file=sys.stderr)
		return EXIT_USAGE
	if not args.yes:
		manifest = L.verify_backup(src)
		print(f"would restore from {src} (verified):")
		for name, info in manifest["sets"].items():
			if (er and name == "er") or (mgsv and name.startswith("mgsv/")):
				files = L.restore_files(info, name, args.all_er_files)
				print(f"  {name}: {len(files)} files -> {info['source']}")
				for e in files:
					print(f"    overwrites {e['path']}  ({e['bytes']} bytes)")
				for p in L.created_after(info, name):
					print(f"    removes {p.name}  (not in the backup: created after it; kept in the safety backup)")
				left = len(info["files"]) - len(files)
				if left:
					print(f"    ({left} other files of that folder are left as they are, e.g. your vanilla ER0000.sl2: --all-er-files restores them too)")
		print("current saves are backed up first. Re-run with --yes to do it.")
		return EXIT_ERROR
	try:
		L.restore_saves(args.timestamp, er, mgsv, all_er=args.all_er_files)
	except L.LaunchError as e:
		print(f"restore aborted: {e}", file=sys.stderr)
		return EXIT_ERROR
	return EXIT_OK


def cmd_backup(args) -> int:
	running = L.game_processes()
	if running["eldenring"] or running["mgsvtpp"]:
		print(f"close the games first (running: {running})", file=sys.stderr)
		return EXIT_ERROR
	dest = L.backup_saves(L.Machine(), L.timestamp(), not args.mgsv_only, not args.er_only)
	L.verify_backup(dest)
	print(f"backup verified: {dest}")
	return EXIT_OK


def cmd_uninstall_guest(args) -> int:
	d = L.Machine().app_dir(L.MGSV_APPID)
	if not d:
		print("MGSV install not found", file=sys.stderr)
		return EXIT_ERROR
	try:
		L.uninstall_guest(d)
	except L.LaunchError as e:
		print(e, file=sys.stderr)
		return EXIT_ERROR
	return EXIT_OK


def cmd_screenshot(args) -> int:
	spectacle = shutil.which("spectacle")
	if not spectacle:
		print("spectacle not found", file=sys.stderr)
		return EXIT_ERROR
	out = Path(args.output) if args.output else L.LOGS / "screenshots" / f"{L.timestamp()}.png"
	out.parent.mkdir(parents=True, exist_ok=True)
	mode = {"full": "-f", "active": "-a", "monitor": "-m"}[args.mode]
	r = subprocess.run([spectacle, "-b", "-n", mode, "-o", str(out)], capture_output=True, text=True, timeout=30)
	for _ in range(20):
		if out.exists() and out.stat().st_size > 0:
			break
		time.sleep(0.25)
	if not out.exists():
		print(f"spectacle did not write {out} (exit {r.returncode}): {r.stderr.strip()}", file=sys.stderr)
		return EXIT_ERROR
	print(out)
	return EXIT_OK


def cmd_install_me3(args) -> int:
	try:
		me3 = L.install_me3(archive=args.archive)
	except L.LaunchError as e:
		print(e, file=sys.stderr)
		return EXIT_ERROR
	print(me3)
	return EXIT_OK


class VersionAction(argparse.Action):
	"""`pr --version`: computed only when asked (it asks the VCS and reads both DLLs)."""

	def __init__(self, option_strings, dest, **kwargs):
		super().__init__(option_strings, dest, nargs=0, help="print the version of these tools and of the built DLLs, then exit")

	def __call__(self, parser, namespace, values, option_string=None):
		from . import version
		print(version.report(L.BUILD))
		parser.exit()


def main(argv=None) -> int:
	ap = argparse.ArgumentParser(prog="pr", description="PhantomRing Linux tools (docs/TOOLS.md)")
	ap.add_argument("--version", action=VersionAction)
	ap.add_argument("--path", help="mapping file (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1)")
	sub = ap.add_subparsers(dest="cmd", required=True)

	sub.add_parser("status", help="header, both sides, states, rings").set_defaults(fn=cmd_status)
	p = sub.add_parser("watch", help="live status")
	p.add_argument("-n", "--interval", type=float, default=0.5)
	p.set_defaults(fn=cmd_watch)

	p = sub.add_parser("lua", help="run Lua inside MGSV (guest console)")
	p.add_argument("code", nargs="*", help="Lua chunk, e.g. 'return vars.playerPosX'")
	p.add_argument("-f", "--file", help="read the chunk from a file")
	p.add_argument("-r", "--ret", action="store_true", help="prepend 'return '")
	p.add_argument("-t", "--timeout", type=float, default=5.0)
	p.add_argument("--force", action="store_true", help="send even if the guest looks absent or busy")
	p.set_defaults(fn=lambda a: console_cmd(a, "guest"))

	p = sub.add_parser("er", help="run a command inside Elden Ring (host console)")
	p.add_argument("command", nargs="+")
	p.add_argument("-t", "--timeout", type=float, default=5.0)
	p.add_argument("--force", action="store_true", help="send even if the host looks absent or busy")
	p.set_defaults(fn=lambda a: console_cmd(a, "host"))

	p = sub.add_parser("er-continue", help="press A through Elden Ring's title screen until the character loads")
	p.add_argument("--timeout", type=float, default=180.0)
	p.set_defaults(fn=cmd_er_continue)

	p = sub.add_parser("launch", help="back up saves, install the guest, start MGSV (hidden) and Elden Ring")
	g = p.add_mutually_exclusive_group()
	g.add_argument("--mgsv-only", action="store_true")
	g.add_argument("--er-only", action="store_true")
	p.add_argument("--dry-run", action="store_true", help="print the checks, plan and commands; start and write nothing")
	p.add_argument("-W", "--width", type=int, default=1280, help="MGSV (gamescope headless) width")
	p.add_argument("-H", "--height", type=int, default=720, help="MGSV (gamescope headless) height")
	p.add_argument("--wait", action="store_true", help="stay in the foreground until both exit")
	p.add_argument("--proton-debug", help="WINEDEBUG for both games (default: reduced +seh/+debugstr/+loaddll set)")
	p.add_argument("--er-headless", metavar="WxH", help="test automation: Elden Ring in its own headless gamescope too (e.g. 3840x2160), "
		"for a machine whose displays are off")
	p.add_argument("--shm", help="mapping file for this launch (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1); "
		"lets separate test setups run side by side")
	p.add_argument("--seed-save", metavar="NAME", help="the Elden Ring save PhantomRing0000.sl2 is first copied from (a file name "
		"in the save dir or a path; default $PHANTOMRING_SEED_SAVE, else ER0000.sl2). Only used while PhantomRing0000.sl2 doesn't exist")
	p.add_argument("--keep-backups", type=int, metavar="N", help=f"save backups to keep under backups/ (default "
		f"$PHANTOMRING_KEEP_BACKUPS or {L.KEEP_BACKUPS}; the oldest and *-pre-restore are always kept; 0 keeps all)")
	p.set_defaults(fn=cmd_launch)

	p = sub.add_parser("stop", help="SIGTERM the process trees pr launch started")
	p.add_argument("--er", action="store_true", help="stop only Elden Ring (MGSV keeps running)")
	p.add_argument("--mgsv", action="store_true", help="stop only MGSV (Elden Ring keeps running)")
	p.add_argument("--timeout", type=float, default=20.0)
	p.add_argument("--kill", action="store_true", help="SIGKILL what is left after the timeout")
	p.set_defaults(fn=cmd_stop)

	p = sub.add_parser("logs", help="tail the newest launch + plugin logs")
	p.add_argument("-n", "--lines", type=int, default=30)
	p.add_argument("-f", "--follow", action="store_true")
	p.set_defaults(fn=cmd_logs)

	p = sub.add_parser("backup-saves", help="verified backup of the ER and MGSV saves")
	g = p.add_mutually_exclusive_group()
	g.add_argument("--mgsv-only", action="store_true")
	g.add_argument("--er-only", action="store_true")
	p.set_defaults(fn=cmd_backup)

	p = sub.add_parser("restore-saves", help="copy a backup back (backs up the current saves first)")
	p.add_argument("timestamp", help="backups/<timestamp>")
	g = p.add_mutually_exclusive_group()
	g.add_argument("--mgsv-only", action="store_true")
	g.add_argument("--er-only", action="store_true")
	p.add_argument("--yes", action="store_true", help="really do it")
	p.add_argument("--all-er-files", action="store_true",
		help="restore every file of the Elden Ring save folder (default: only PhantomRing0000.sl2 and its .bak)")
	p.set_defaults(fn=cmd_restore)

	sub.add_parser("uninstall-guest", help="remove our dinput8.dll (+ini) from the MGSV dir").set_defaults(fn=cmd_uninstall_guest)

	sub.add_parser("doctor", help="check this machine for PhantomRing: Steam, game versions, me3, gamescope, Proton, the build, "
		"saves (read-only)").set_defaults(fn=lambda a: __import__("phantomring.doctor", fromlist=["main"]).main(a))
	p = sub.add_parser("collect-logs", help="zip the logs, inis, versions and pr doctor for a bug report (personal paths redacted)")
	p.add_argument("-o", "--output", help="zip to write (default logs/phantomring-logs-<time>.zip)")
	p.set_defaults(fn=lambda a: __import__("phantomring.collect", fromlist=["main"]).main(a))

	p = sub.add_parser("screenshot", help="KDE screenshot via spectacle (for live tests)")
	p.add_argument("-o", "--output")
	p.add_argument("--mode", choices=["full", "active", "monitor"], default="full")
	p.set_defaults(fn=cmd_screenshot)

	p = sub.add_parser("install-me3", help="extract the verified me3 0.13.0 into third_party/me3")
	p.add_argument("--archive", metavar="PATH", help=f"the downloaded {L.ME3_ASSET} (default $PHANTOMRING_ME3_ARCHIVE, "
		"third_party/downloads/ or ~/Downloads/)")
	p.set_defaults(fn=cmd_install_me3)

	for name, module in (("fake-er", "fake_er"), ("fake-mgsv", "fake_mgsv"), ("record", "recorder"), ("replay", "replay")):
		p = sub.add_parser(name, help=f"run {module}.py (args passed through)", add_help=False)
		p.add_argument("rest", nargs=argparse.REMAINDER)
		p.set_defaults(fn=lambda a, m=module: __import__(f"phantomring.{m}", fromlist=["main"]).main(
			a.rest + (["--path", a.path] if a.path else [])), passthrough=True)

	args, extra = ap.parse_known_args(argv)
	if extra:
		if not getattr(args, "passthrough", False):
			ap.error(f"unrecognized arguments: {' '.join(extra)}")
		args.rest = extra + args.rest
	try:
		return args.fn(args)
	except BrokenPipeError:
		return EXIT_OK


if __name__ == "__main__":
	sys.exit(main())

def cmd_er_continue(args) -> int:
	"""Elden Ring's title needs "press any button", then CONTINUE (focused by default); its menu ignores
	presses for a few seconds while it fades in. Press A (and Enter, for a machine with no controller) every 4 s
	while the live host session says 'title', stop as soon as it loads or is in game."""
	deadline = time.monotonic() + args.timeout
	pressed = 0
	while time.monotonic() < deadline:
		try:
			link = proto.Link(proto.default_path())
		except proto.MappingMissing:
			time.sleep(2)
			continue
		try:
			h = link.header
			alive = h.magic == proto.MAGIC and h.qpcFrequency and h.hostHeartbeatQpc and \
				(proto.qpc_now() - h.hostHeartbeatQpc) * 1000 // h.qpcFrequency < proto.HEARTBEAT_TIMEOUT_MS
			status = Status(h.hostStatus) if alive else None
			if status in (Status.Loading, Status.Ready) and pressed:
				print(f"Elden Ring: {status.name} after {pressed} press(es)")
				return 0
			if status == Status.Title:
				try:
					proto.console_request(link, "host", "press a 250", timeout=5.0)
					# With no controller connected Elden Ring doesn't poll XInput at all and the pad press reaches nothing:
					# Enter on the keyboard too (Elden Ring reads it once a frame, so held a moment).
					proto.console_request(link, "host", "input keydown 0x0D", timeout=5.0)
					time.sleep(0.2)
					proto.console_request(link, "host", "input keyup 0x0D", timeout=5.0)
					pressed += 1
				except Exception as e:  # console busy or briefly absent: try again
					print(f"press failed: {e}", file=sys.stderr)
		finally:
			link.close()
		time.sleep(4.0)
	print("Elden Ring did not leave its title screen in time", file=sys.stderr)
	return 1
