"""Copies a set of Mach-O libraries and everything they load in turn (outside
the system: /usr/lib, /System) into a kit, with install names rewritten to
@rpath/<name> so the kit loads nothing from the build tree or Homebrew.

Libraries go to KIT/lib and look beside themselves (@loader_path); the
lifter goes to KIT/bin and looks in ../lib. Each file is signed again (ad hoc).
"""
import os
import shutil
import subprocess
import sys


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def deps_of(path):
    return [l.strip().split(" (")[0] for l in run("otool", "-L", path).splitlines()[1:]]


def rpaths_of(path):
    out, lines = [], run("otool", "-l", path).splitlines()
    for i, l in enumerate(lines):
        if l.strip() == "cmd LC_RPATH":
            out.append(lines[i + 2].strip().split(" ")[1])
    return out


def install_id(path):
    lines = run("otool", "-D", path).splitlines()
    return lines[1].strip() if len(lines) > 1 else os.path.basename(path)


def resolve(ref, origin, rpaths, exe_dir):
    """The file a load command names, as dyld would find it from ORIGIN."""
    here = os.path.dirname(origin)
    if ref.startswith("@loader_path/"):
        c = os.path.join(here, ref.split("/", 1)[1])
        return os.path.realpath(c) if os.path.exists(c) else None
    if ref.startswith("@executable_path/"):
        c = os.path.join(exe_dir, ref.split("/", 1)[1])
        return os.path.realpath(c) if os.path.exists(c) else None
    if ref.startswith("@rpath/"):
        for r in rpaths:
            r = r.replace("@loader_path", here).replace("@executable_path", exe_dir)
            c = os.path.join(r, ref[len("@rpath/"):])
            if os.path.exists(c):
                return os.path.realpath(c)
        return None
    return os.path.realpath(ref) if os.path.exists(ref) else None


def system(ref):
    return ref.startswith(("/usr/lib/", "/System/"))


def copy_closure(linked, lift, extra_rpaths, kit):
    """Copies LINKED (the game's libraries), the lifter LIFT and all they load
    into KIT. Returns {real path: name in KIT/lib}."""
    exe_dir = os.path.dirname(lift)
    base_rpaths = rpaths_of(lift) + extra_rpaths
    todo = [os.path.realpath(p) for p in linked]
    for ref in deps_of(lift):
        if not system(ref):
            r = resolve(ref, lift, base_rpaths, exe_dir)
            if r is None:
                sys.exit("kit: can't find %s (from recomp_lift)" % ref)
            todo.append(r)
    names = {}  # real path -> name in lib/
    refs = {}  # (real path, reference) -> real path it names
    while todo:
        real = todo.pop()
        if real in names:
            continue
        names[real] = os.path.basename(install_id(real))
        rp = rpaths_of(real) + base_rpaths
        for ref in deps_of(real)[1:]:  # [0] is the library's own id
            if system(ref):
                continue
            r = resolve(ref, real, rp, exe_dir)
            if r is None:
                sys.exit("kit: can't find %s (from %s)" % (ref, real))
            refs[(real, ref)] = r
            todo.append(r)
    if len(set(names.values())) != len(names):
        sys.exit("kit: two libraries share a name: %s" % sorted(names.values()))

    def relink(src, dst, id_name):
        shutil.copy2(src, dst)
        os.chmod(dst, 0o755)
        real_src = os.path.realpath(src)
        args = []
        if id_name:
            args += ["-id", "@rpath/" + id_name]
        for ref in deps_of(src)[1 if id_name else 0:]:
            if system(ref):
                continue
            real = refs.get((real_src, ref)) or resolve(ref, src, rpaths_of(src) + base_rpaths, exe_dir)
            args += ["-change", ref, "@rpath/" + names[real]]
        for r in rpaths_of(src):
            args += ["-delete_rpath", r]
        args += ["-add_rpath", "@loader_path" if id_name else "@executable_path/../lib"]
        subprocess.run(["install_name_tool", *args, dst], check=True, capture_output=True)
        subprocess.run(["codesign", "--force", "--sign", "-", dst], check=True, capture_output=True)

    for real, name in names.items():
        relink(real, os.path.join(kit, "lib", name), name)
    relink(lift, os.path.join(kit, "bin", "recomp_lift"), None)
    return names
