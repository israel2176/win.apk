#!/usr/bin/env python3
"""untar.py <archive.tar.gz> <dest>: unpack a gitiles archive the same on Linux and on Windows.

AOSP's trees hold symlinks (frameworks/native/include/ui/Rect.h -> ../../libs/ui/include/ui/Rect.h).
MSYS2's tar cannot make them on Windows, and fails on the whole archive. Here regular files are
written first, then each symlink becomes a copy of what it names when that is inside the archive
(several passes, since a link may name another link); a link to something the archive lacks
(build/soong's clang-format files) is left out, as nothing builds from it.
"""
import os
import shutil
import sys
import tarfile


def main(archive: str, dest: str) -> int:
    dest = os.path.abspath(dest)
    links = []
    with tarfile.open(archive, 'r:gz') as tar:
        for m in tar:
            path = os.path.abspath(os.path.join(dest, m.name))
            if not (path == dest or path.startswith(dest + os.sep)):
                raise SystemExit(f'untar: {m.name} is outside the destination')
            if m.issym() or m.islnk():
                links.append((path, m.linkname, m.issym()))
            elif m.isdir():
                os.makedirs(path, exist_ok=True)
            elif m.isfile():
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with tar.extractfile(m) as src, open(path, 'wb') as out:
                    shutil.copyfileobj(src, out)
    pending = links
    while pending:
        left = []
        for path, target, sym in pending:
            src = os.path.normpath(os.path.join(os.path.dirname(path), target) if sym else os.path.join(dest, target))
            if not src.startswith(dest + os.sep) or not os.path.exists(src):
                left.append((path, target, sym))
                continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if os.path.isdir(src):
                shutil.copytree(src, path, dirs_exist_ok=True)
            else:
                shutil.copyfile(src, path)
        if len(left) == len(pending):
            break  # what is left names nothing in the archive
        pending = left
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
