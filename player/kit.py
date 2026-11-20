#!/usr/bin/env python3
"""The player kit of a game that uses SwARc: everything needed to build the
game on a player's Mac except the game itself.

  kit.py build [--root GAME_REPO]        dist/<id>-kit-macos-arm64.tar.gz
  kit.py sync [--root GAME_REPO] [--check]
      write the game's install.sh and .github/workflows/kit.yml from
      player/templates (--check: fail if they are out of date instead)

The game is described by recomplauncher.json at the root of its repository
(player/README.md, "recomplauncher.json"). `build` configures the game's
CMake project with a stub lifted file (out/kit), builds <binary>_kit_parts
and assembles dist/<id>-kit: the libraries the game links (SwARc's and, from
Homebrew, all they load in turn) with their install names rewritten to
@rpath, the lifter, the game's own non-lifted objects, the SwARc headers
lifted code includes, the compile and link flags CMake uses (read from the
stub), pe_symbols.py, the manifests, game.env and player/build.sh. Nothing
from the game, nor derived from it.

Needs the game's developer setup (Homebrew packages, external/SwARc). The kit
runs on the macOS it was built on and newer (Homebrew's libraries target
their build OS), so release kits come from CI on the oldest supported macOS.
"""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
SWARC = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import bundle  # noqa: E402


# --- recomplauncher.json ---------------------------------------------------

def load_config(root):
    path = os.path.join(root, "recomplauncher.json")
    with open(path) as f:
        doc = json.load(f)
    x = doc.get("x-swarc")
    if not x:
        sys.exit("kit: %s has no x-swarc block (player/README.md)" % path)
    for key in ("id", "app", "binary", "repo", "steam", "files", "hashes", "lift"):
        if key not in x:
            sys.exit("kit: %s: x-swarc.%s is missing" % (path, key))
    for key in ("appid", "depot", "manifest", "title", "folder", "size"):
        if key not in x["steam"]:
            sys.exit("kit: %s: x-swarc.steam.%s is missing" % (path, key))
    if not x["lift"].get("images"):
        sys.exit("kit: %s: x-swarc.lift.images is empty" % path)
    c = dict(x)
    c["name"] = doc["name"]
    c["find"] = doc["game"]["check"]["file"]
    c["env"] = x["id"].upper().replace("-", "_")
    c["kit_name"] = "%s-kit" % x["id"]
    c["asset"] = "%s-kit-macos-arm64.tar.gz" % x["id"]
    c.setdefault("kit", {})
    return c


def game_env(c):
    """game.env: what build.sh needs to know, as bash 3.2 variables."""
    q = shlex.quote
    steam = c["steam"]
    lines = ["# Written by SwARc's player/kit.py from recomplauncher.json; build.sh sources it."]

    def var(name, value):
        lines.append("%s=%s" % (name, q(str(value))))

    def arr(name, values):
        lines.append("%s=(%s)" % (name, " ".join(q(str(v)) for v in values)))

    var("ID", c["id"])
    var("ENV", c["env"])
    var("NAME", c["name"])
    var("APP", c["app"])
    var("BINARY", c["binary"])
    var("REPO", c["repo"])
    var("FIND", c["find"])
    var("STEAM_APP", steam["appid"])
    var("STEAM_DEPOT", steam["depot"])
    var("STEAM_MANIFEST", steam["manifest"])
    var("STEAM_BUILD", steam.get("build", ""))
    var("STEAM_TITLE", steam["title"])
    var("STEAM_FOLDER", steam["folder"])
    var("STEAM_SIZE", steam["size"])
    arr("FILES", c["files"])
    hashes = sorted(c["hashes"].items())
    var("HASH_N", len(hashes))
    arr("HASH_FILES", [h[0] for h in hashes])
    arr("HASH_SHAS", [h[1] for h in hashes])
    var("OTHER_VERSIONS", c.get("other_versions", "Other versions don't work."))
    supplied = c.get("supplied", [])
    var("SUPPLIED_N", len(supplied))
    arr("SUPPLIED_FILES", [s["file"] for s in supplied])
    arr("SUPPLIED_SHAS", [s["sha256"] for s in supplied])
    arr("SUPPLIED_ABOUT", [s["about"] for s in supplied])
    images = c["lift"]["images"]
    arr("LIFT_ARGS", c["lift"].get("args", []))
    var("LIFT_N", len(images))
    arr("LIFT_FILES", [i["file"] for i in images])
    arr("LIFT_MANIFESTS", [i.get("manifest", "empty.manifest") for i in images])
    arr("LIFT_BASES", [i.get("base", "") for i in images])
    var("DISK_GB", c.get("disk_gb", 5))
    var("BUILD_TIME", c.get("build_time", "a few minutes"))
    return "\n".join(lines) + "\n"


# --- build -----------------------------------------------------------------

def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def without_paths(tokens):
    """Compiler or linker arguments minus outputs, inputs and anything naming a path."""
    out, skip = [], False
    for t in tokens:
        if skip:
            skip = False
            continue
        if t in ("-o", "-c", "-isysroot", "-isystem", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if t in ("-nostdinc++", ":", "&&") or t.startswith(("-I", "-Wl,-rpath,", "-Wl,-cache_path_lto,", "-fprofile-")):
            continue
        if t.endswith((".o", ".dylib", ".cpp", ".a")) or t.startswith("/"):
            continue
        out.append(t)
    return out


def configure_and_build(root, c, build):
    stub = os.path.join(build, "stub")
    os.makedirs(stub, exist_ok=True)
    # A stub lifted file, so CMake defines the game's targets and their flags.
    with open(os.path.join(stub, "gen_000.cpp"), "w") as f:
        f.write("// stub: SwARc's player/kit.py reads the flags of lifted code from it\n")
    cxxflags = ""
    if os.path.isdir("/Library/Developer/CommandLineTools/usr/include/c++/v1"):
        sdk = run("xcrun", "--show-sdk-path").strip()
        cxxflags = "-nostdinc++ -isystem %s/usr/include/c++/v1" % sdk
    llvm = run("brew", "--prefix", "llvm@22").strip()
    print("configuring %s..." % os.path.relpath(build, root))
    with open(os.path.join(build, "configure.log"), "w") as log:
        subprocess.run(["cmake", "-S", root, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                        "-DRECOMP_LLVM_ROOT=" + llvm, "-DCMAKE_CXX_FLAGS=" + cxxflags,
                        "-D%s_GENERATED_DIR=%s" % (c["env"], stub)] + c["kit"].get("cmake_args", []),
                       check=True, stdout=log, stderr=subprocess.STDOUT)
    print("building...")
    with open(os.path.join(build, "build.log"), "w") as log:
        r = subprocess.run(["cmake", "--build", build, "--target", c["binary"] + "_kit_parts"],
                           stdout=log, stderr=subprocess.STDOUT)
    if r.returncode:
        with open(os.path.join(build, "build.log")) as f:
            sys.stdout.write("".join(f.readlines()[-30:]))
        sys.exit("kit: the build failed (%s)" % os.path.join(build, "build.log"))


def assemble(root, c, build, kit):
    if os.path.exists(kit):
        shutil.rmtree(kit)
    for d in ("bin", "lib", "obj", "include", "src", "data", "app", "flags", "licenses"):
        os.makedirs(os.path.join(kit, d))

    # Flags: CMake's own, from the stub lifted file and the game's link line.
    cc = json.load(open(os.path.join(build, "compile_commands.json")))
    stub = next(e for e in cc if e["file"].endswith("/stub/gen_000.cpp"))
    lifted_flags = without_paths(shlex.split(stub["command"])[1:])
    link_line = run("ninja", "-C", build, "-t", "commands", c["binary"]).strip().splitlines()[-1]
    link_line = link_line.split(" && cd ")[0]  # the bundling and signing steps after it
    link_tokens = shlex.split(link_line)
    first = next(i for i, t in enumerate(link_tokens) if t.endswith(("c++", "clang++")))
    link_flags = without_paths(link_tokens[first + 1:])
    linked = [t for t in link_tokens if t.endswith(".dylib")]
    linked = [t if os.path.isabs(t) else os.path.join(build, t) for t in linked]
    for lib in c["kit"].get("require_libs", []):
        if not any(lib in t for t in linked):
            sys.exit("kit: the game's link line has no %s" % lib)

    # The game's own objects (its <binary>_host target).
    host = [e for e in cc if "/%s_host.dir/" % c["binary"] in e["command"]]
    if not host:
        sys.exit("kit: no %s_host objects in %s" % (c["binary"], build))
    for e in host:
        toks = shlex.split(e["command"])
        obj = toks[toks.index("-o") + 1]
        obj = obj if os.path.isabs(obj) else os.path.join(e["directory"], obj)
        shutil.copy2(obj, os.path.join(kit, "obj", os.path.basename(e["file"]) + ".o"))

    # Libraries: everything the game and the lifter load, outside the system.
    lift = os.path.join(build, "swarc", "tools", "lifter_x86", "recomp_lift")
    extra_rpaths = [os.path.join(build, "swarc"), os.path.join(build, "swarc", "platforms", "win32")]
    names = bundle.copy_closure(linked, lift, extra_rpaths, kit)

    # Headers for lifted code, and the lifted function table's source.
    src = os.path.join(SWARC, "src")
    for d, _, files in os.walk(src):
        for f in files:
            if f.endswith((".h", ".hpp", ".inc", ".def")):
                rel = os.path.relpath(os.path.join(d, f), src)
                os.makedirs(os.path.join(kit, "include", os.path.dirname(rel)), exist_ok=True)
                shutil.copy2(os.path.join(d, f), os.path.join(kit, "include", rel))
    shutil.copy2(os.path.join(src, "x86", "lifted_table.cpp"), os.path.join(kit, "src"))

    with open(os.path.join(kit, "flags", "lifted.txt"), "w") as f:
        f.write("\n".join(lifted_flags) + "\n")
    with open(os.path.join(kit, "flags", "link.txt"), "w") as f:
        f.write("\n".join(link_flags) + "\n")
    with open(os.path.join(kit, "flags", "libs.txt"), "w") as f:
        f.write("\n".join(names[os.path.realpath(p)] for p in linked) + "\n")

    # The lifter's symbols are made by build.sh from the player's own files.
    shutil.copy2(os.path.join(SWARC, "tools", "pe_symbols.py"), os.path.join(kit, "bin"))
    open(os.path.join(kit, "data", "empty.manifest"), "w").close()
    for img in c["lift"]["images"]:
        if "manifest" in img:
            shutil.copy2(os.path.join(root, img["manifest"]), os.path.join(kit, "data"))
    shutil.copy2(os.path.join(root, "cmake", "Info.plist.in"), os.path.join(kit, "app", "Info.plist"))
    with open(os.path.join(kit, "game.env"), "w") as f:
        f.write(game_env(c))
    shutil.copy2(os.path.join(HERE, "build.sh"), os.path.join(kit, "build.sh"))
    os.chmod(os.path.join(kit, "build.sh"), 0o755)

    # Licences: SwARc's, and where each bundled Homebrew library came from.
    shutil.copy2(os.path.join(SWARC, "LICENSE"), os.path.join(kit, "licenses", "SwARc-LICENSE"))
    with open(os.path.join(kit, "licenses", "THIRD-PARTY.txt"), "w") as f:
        f.write("Libraries bundled in lib/, built by Homebrew (https://brew.sh); each keeps its own licence.\n\n")
        for real, name in sorted(names.items(), key=lambda x: x[1]):
            if "/Cellar/" in real:
                pkg, ver = real.split("/Cellar/")[1].split("/")[:2]
                f.write("%-40s %s %s\n" % (name, pkg, ver))

    # The oldest macOS every bundled file supports.
    minos = "0"
    for p in [os.path.join(kit, "lib", n) for n in names.values()] + [os.path.join(kit, "bin", "recomp_lift")]:
        for l in run("otool", "-l", p).splitlines():
            if l.strip().startswith("minos "):
                v = l.split()[1]
                if tuple(map(int, v.split("."))) > tuple(map(int, minos.split("."))):
                    minos = v
    with open(os.path.join(kit, "MIN_MACOS"), "w") as f:
        f.write(minos + "\n")

    # The game's commit, then SwARc's (an update is a newer game commit).
    game_rev = run("git", "-C", root, "describe", "--always", "--dirty").strip()
    swarc_rev = run("git", "-C", SWARC, "rev-parse", "--short", "HEAD").strip()
    with open(os.path.join(kit, "VERSION"), "w") as f:
        f.write("%s\n%s\n" % (game_rev, swarc_rev))
    print("kit: %d libraries, needs macOS %s" % (len(names), minos))


def cmd_build(root, c):
    build = os.path.join(root, "out", "kit")
    dist = os.path.join(root, "dist")
    kit = os.path.join(dist, c["kit_name"])
    os.makedirs(dist, exist_ok=True)
    configure_and_build(root, c, build)
    print("assembling %s..." % os.path.relpath(kit, root))
    assemble(root, c, build, kit)
    tarball = os.path.join(dist, c["asset"])
    run("tar", "-C", dist, "-czf", tarball, c["kit_name"])
    print("wrote %s (%s)" % (os.path.relpath(tarball, root), run("du", "-h", tarball).split()[0]))


# --- sync ------------------------------------------------------------------

GENERATED = (("install.sh", "install.sh", 0o755), ("kit.yml", os.path.join(".github", "workflows", "kit.yml"), 0o644))


def render(template, c):
    with open(os.path.join(HERE, "templates", template)) as f:
        text = f.read()
    values = {
        "ID": c["id"], "ENV": c["env"], "NAME": c["name"], "APP": c["app"], "REPO": c["repo"],
        "KIT": c["kit_name"], "ASSET": c["asset"], "BREW": " ".join(c["kit"].get("brew", [])),
        "RUNNER": c["kit"].get("runner", "macos-15"),
    }
    for k, v in values.items():
        text = text.replace("@%s@" % k, v)
    return text


def cmd_sync(root, c, check):
    stale = []
    for template, rel, mode in GENERATED:
        path = os.path.join(root, rel)
        want = render(template, c)
        have = open(path).read() if os.path.exists(path) else None
        if have == want:
            continue
        if check:
            stale.append(rel)
            continue
        os.makedirs(os.path.dirname(path) or root, exist_ok=True)
        with open(path, "w") as f:
            f.write(want)
        os.chmod(path, mode)
        print("wrote %s" % rel)
    if stale:
        sys.exit("kit: out of date with SwARc's player/templates: %s (run external/SwARc/player/kit.py sync)"
                 % ", ".join(stale))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=("build", "sync"))
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(SWARC)),
                    help="the game's repository (default: the one SwARc is checked out in)")
    ap.add_argument("--check", action="store_true", help="sync: fail if the files are out of date")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    c = load_config(root)
    if a.command == "build":
        cmd_build(root, c)
    else:
        cmd_sync(root, c, a.check)


if __name__ == "__main__":
    main()
