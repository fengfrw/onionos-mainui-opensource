# SPDX-License-Identifier: GPL-3.0-only
"""Right in Game Info opens a fullscreen gallery of the game's media: front and
back covers, 3D box, screenshots and video from media/ in its console's ROM
folder, by the ROM file's name; never the Imgs cover Game Info shows. Right
and Left browse, and B, or Left on the first item, returns to Game Info.
Videos play with Onion's FFmpeg, loaded from .tmp_update/lib/parasyte only
then; here a stub stands in for it (tests/ffmpeg_stub.c) under FFmpeg's
sonames, with LD_LIBRARY_PATH as Onion starts MainUI, without that folder."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from PIL import Image
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="media-gallery-", dir=BUILD))
LIBS = {"full": BUILD / "ffstub.so", "noscale": BUILD / "ffstub-noscale.so"}
for name, stub in LIBS.items():
    (OUT / name).mkdir()
    for soname in ("libavutil.so.54", "libswscale.so.3", "libavcodec.so.56", "libavformat.so.56"):
        (OUT / name / soname).symlink_to(stub.resolve())


def environment(path=None):
    """Onion starts MainUI without its FFmpeg folder on LD_LIBRARY_PATH."""
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)
    if path:
        env["LD_LIBRARY_PATH"] = str(OUT / path)
    return env


# The player on its own: timing, repeats, stopping, and every failure.
VIDEOS = OUT / "videos"
VIDEOS.mkdir()
for name, spec in {"ok": "320 240 6 20", "wide": "640 160 3 20", "tall": "120 240 3 20",
                   "nopts": "320 240 6 20 nopts", "noseek": "320 240 3 20 noseek",
                   "slow": "320 240 1000 30 slow", "empty": "320 240 0 20",
                   "nocodec": "320 240 5 20 nocodec", "huge": "4000 3000 5 20",
                   "hugenocodec": "4000 3000 5 20 nocodec", "liar": "4000 3000 5 20 lie",
                   "nodims": "320 240 5 20 nodims", "extra": "320 240 5 20 extra",
                   "grow": "320 240 200 20 grow10",
                   "cover": "320 240 6 20 cover", "waitopen": "320 240 6 20 waitopen",
                   "waitprobe": "320 240 6 20 waitprobe",
                   "waitread": "320 240 6 20 waitread"}.items():
    (VIDEOS / f"{name}.mp4").write_text(f"STUB {spec}\n")
(VIDEOS / "text.mp4").write_text("not a video\n")
# Found in the folder given, as in Onion's parasyte folder, or by name.
(OUT / "none").mkdir()
for mode, folder, path in (("play", "full", None), ("errors", "full", None),
                           ("errors", "-", "full"), ("missing", "noscale", None),
                           ("missing", "none", None), ("missing", "-", None),
                           ("cancel", "full", None)):
    libraries = str(OUT / folder) if folder != "-" else "-"
    result = subprocess.run([str(BUILD / "fixture-video"), str(VIDEOS), libraries, mode],
                            cwd=ROOT, env=environment(path), timeout=60, capture_output=True,
                            text=True)
    assert result.returncode == 0, (mode, folder, path, result.stdout, result.stderr)
    assert "[ffstub]" not in result.stderr, result.stderr

# A console with a game that has every kind of media, one without any, and
# one in a subfolder, its media at the console's ROM folder.
SD = OUT / "sd"
(SD / "Emu/FC").mkdir(parents=True)
(SD / "Emu/FC/config.json").write_text(json.dumps(dict(
    label="NES", rompath="../../Roms/FC", imgpath="../../Roms/FC/Imgs", launch="launch.sh",
    extlist="nes")))
(SD / ".tmp_update/config").mkdir(parents=True)
ROMS = SD / "Roms/FC"
(ROMS / "Sub").mkdir(parents=True)
GAME = "Super Game$ Collection (USA)"
for rom in (GAME + ".nes", "Zero Media.nes", "Sub/Inner Game.nes"):
    (ROMS / rom).write_bytes(b"rom")
# Every kind of media, with screenshots <name>.png, -1 and -2; -4 does not
# show, -3 being missing. The list's cover in Imgs is no part of the gallery.
MEDIA = [("box2dfront", "", (200, 30, 30)), ("box2dback", "", (30, 200, 30)),
         ("box3d", "", (30, 200, 200)), ("screenshot", "", (30, 30, 200)),
         ("screenshot", "-1", (220, 220, 30)), ("screenshot", "-2", (220, 120, 30)),
         ("screenshot", "-4", (220, 30, 220))]
COLOURS = {folder + suffix: colour for folder, suffix, colour in MEDIA}
TALL = {"box2dfront", "box2dback", "box3d"}
for folder, suffix, colour in MEDIA:
    target = ROMS / "media" / folder / (GAME + suffix + ".png")
    target.parent.mkdir(parents=True, exist_ok=True)
    Image.new("RGB", (300, 400) if folder in TALL else (320, 240), colour).save(target)
(ROMS / "media/videos").mkdir()
(ROMS / "media/videos" / (GAME + ".mp4")).write_text("STUB 320 240 6 20\n")
COVER = (120, 60, 160)
(ROMS / "Imgs").mkdir()
Image.new("RGB", (200, 280), COVER).save(ROMS / "Imgs" / (GAME + ".png"))
# Screenshots numbered from -2, as some scrapers name them: all show.
Image.new("RGB", (50, 50), (200, 30, 30)).save(ROMS / "media/box2dfront/Inner Game.png")
Image.new("RGB", (320, 240), (90, 90, 90)).save(ROMS / "media/screenshot/Inner Game-2.png")
Image.new("RGB", (320, 240), (150, 150, 150)).save(ROMS / "media/screenshot/Inner Game-3.png")
FRAME0 = (0, 255, 100)  # the stub's first frame


PARASYTE = SD / ".tmp_update/lib/parasyte"


def shot(actions, libraries="full", system="NES"):
    """The screen after the actions, with Onion's FFmpeg folder holding the
    full stub, one without sws_scale, or nothing."""
    shutil.rmtree(PARASYTE, ignore_errors=True)
    if libraries:
        shutil.copytree(OUT / libraries, PARASYTE, symlinks=True)
    target = OUT / "shot.bmp"
    command = [str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme", str(ONION_THEME),
               "--input", actions, "--snapshot", str(target)]
    if system:
        command += ["--system", system]
    result = subprocess.run(command, cwd=ROOT, env=environment(), check=True,
                            timeout=30, capture_output=True, text=True)
    assert "[ffstub]" not in result.stderr, result.stderr
    (SD / "appconfigs/romwinidx.json").unlink(missing_ok=True)
    return Image.open(target).convert("RGB"), result.stderr


def colours(image):
    return {colour for _, colour in image.getcolors(1 << 24)}


def centre(image):
    return image.getpixel((320, 240))


# The list's rows: Sub, then the game, then Zero Media. Right opens Game Info,
# as before, with the cover from Imgs.
info, _ = shot("DR")
assert COVER in colours(info)
# Right again: the front cover, fitted to the height and centred on black.
box, _ = shot("DRR")
assert centre(box) == COLOURS["box2dfront"], centre(box)
assert box.getpixel((5, 240)) == (0, 0, 0) and box.getpixel((315 - 180, 240)) == (0, 0, 0)
assert box.getpixel((320 - 170, 240)) == COLOURS["box2dfront"]
# Then the back cover, the 3D box and the screenshots, which fill the screen;
# then the video, the last, where Right stays. Never the Imgs cover.
order = ["box2dback", "box3d", "screenshot", "screenshot-1", "screenshot-2"]
for steps, key in enumerate(order, 1):
    image, _ = shot("DRR" + "R" * steps)
    assert centre(image) == COLOURS[key], (key, centre(image))
    assert COVER not in colours(image), key
full, _ = shot("DRRRRR")
assert full.getpixel((2, 2)) == COLOURS["screenshot"]
VIDEO = "DRR" + "R" * 6  # the seventh item
video, log = shot(VIDEO)
assert centre(video) == FRAME0 and video.getpixel((2, 2)) == FRAME0, centre(video)
# The counter shows over it, bottom right.
assert any(min(video.getpixel((x, y))) > 200 for x in range(560, 640) for y in range(430, 480))
assert centre(shot(VIDEO + "R")[0]) == FRAME0  # the last: Right stays
# Left goes back an item, and from the first one back to Game Info.
assert centre(shot("DRRRL")[0]) == COLOURS["box2dfront"]
assert centre(shot(VIDEO + "L")[0]) == COLOURS["screenshot-2"]
assert shot("DRRL")[0] == info and shot("DRRRLL")[0] == info
# B returns to Game Info, from an image or a video, as it was.
for actions in ("DRRB", VIDEO + "B", "DRRRRRRB", VIDEO + "BRB" + "R" * 7 + "B", "DRRLRLRRLLRB"):
    assert shot(actions)[0] == info, actions
# Up and Down, A and Select do nothing in the gallery; B still returns.
assert shot("DRRUDES")[0] == box
assert shot("DRRUDESB")[0] == info
# A game without media: Right does nothing in Game Info.
plain, _ = shot("DDR")
assert shot("DDRR")[0] == plain
# A game in a subfolder (below ".."): its media is at the console's ROM
# folder. Its screenshots start at -2; Right on the last stays there.
for steps, colour in enumerate([COLOURS["box2dfront"], (90, 90, 90), (150, 150, 150),
                                (150, 150, 150)]):
    assert centre(shot("EDRR" + "R" * steps)[0]) == colour, steps

# Without FFmpeg, or with one that lacks a function, the video shows why.
for libraries in (None, "noscale"):
    card, log = shot(VIDEO, libraries)
    assert centre(card) != FRAME0 and "[video]" in log, log
    text = [(x, y) for x in range(100, 540) for y in range(200, 280)
            if min(card.getpixel((x, y))) > 200]
    assert len(text) > 100, len(text)
    # Images still show, and B still returns.
    assert centre(shot("DRR", libraries)[0]) == COLOURS["box2dfront"]
    assert shot(VIDEO + "B", libraries)[0] == info

# From Favorites, the game's console is found by its launcher.
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps({"menu": dict(favorites=True)}))
(SD / "Roms/favourite.json").write_text(json.dumps(dict(
    label=GAME, rompath=f"/mnt/SDCARD/Roms/FC/{GAME}.nes", launch="/mnt/SDCARD/Emu/FC/launch.sh",
    type=5)) + "\n")
assert centre(shot("ERR", system=None)[0]) == COLOURS["box2dfront"]
# X and Y with the Favorite shortcut do nothing in the gallery.
(SD / ".tmp_update/config/keymap.json").write_text(json.dumps(dict(
    mainui_button_x="fav", mainui_button_y="fav")))
before = (SD / "Roms/favourite.json").read_text()
gallery = shot("ERR", system=None)[0]
assert shot("ERRXY", system=None)[0] == gallery
assert (SD / "Roms/favourite.json").read_text() == before
shutil.rmtree(OUT)
print("Game Info's gallery shows box art, screenshots and video; B returns to Game Info")
