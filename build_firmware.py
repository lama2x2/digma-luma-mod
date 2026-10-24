#!/usr/bin/env python3
"""Собирает update.img с Luma-модом из официального update.img Digma Luma.

  python3 build_firmware.py ../update.img out/update.img
  python3 build_firmware.py --adb-only ../update.img out/update-adb.img   # без мода, только ADB
  --keep-oem  не прошивать oem (настройки программы, заставка сна) — только если на книге
              уже эта же сборка; при переходе с другой сборки oem нужен из пакета

Пакет повторяет официальный (uboot, boot, recovery, … из того же файла), отличается
только rootfs, а раздел userdata (книги) из пакета убран (с --keep-oem — и oem). Ядро и система
гарантированно из одной сборки. Откат — официальный update.img тем же способом.

Что меняется в rootfs:
  /opt/luma/libluma.so, luma.ini (шаблон), fonts/*.ttf с лицензиями, icons/*.bin
  /opt/ylc-ebook/bin/ylc-daemon.sh — штатный + вставки из luma/daemon-mod.sh
                                     (запуск с модулем и защитой от падений)
  /opt/ylc-ebook/bin/ylc-ebook     — буфер CSS 512 -> 4096 байт (4 инструкции)
  /etc/luma-release
Нужны: unsquashfs/mksquashfs (brew install squashfs).
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LUMA = os.path.join(HERE, "luma")
ROOTFS_PART_BYTES = 0x46000 * 512  # из parameter.txt
# всё как в официальном пакете, кроме userdata (книги; штатно он и так не прошивается)
DROP = ["userdata"]

# ylc-ebook сборки 2026-06-26: malloc(512)/snprintf(...,512,...) для CSS -> 4096
BIN_SHA256 = None  # проверяем по ожидаемым байтам ниже, а не по хешу файла
BIN_PATCHES = [  # (VA, было, стало)  mov r0/r1, #512 -> #4096
    (0xEECB4, bytes.fromhex("020ca0e3"), bytes.fromhex("010aa0e3")),
    (0xEECCC, bytes.fromhex("021ca0e3"), bytes.fromhex("011aa0e3")),
    (0xEED1C, bytes.fromhex("020ca0e3"), bytes.fromhex("010aa0e3")),
    (0xEED2C, bytes.fromhex("021ca0e3"), bytes.fromhex("011aa0e3")),
]
# владельцы и права, которые теряются при распаковке не от root
PSEUDO = [
    "usr/bin/busybox m 4755 0 0",
    "usr/libexec/dbus-daemon-launch-helper m 4750 0 101",
    "var/www m 755 33 33",
]


def rkcrc(buf):
    tbl = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C10DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        tbl.append(c)
    crc = 0
    for b in buf:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ tbl[((crc >> 24) ^ b) & 0xFF]
    return crc


def parse(fw):
    if fw[:4] != b"RKFW":
        sys.exit("это не RKFW update.img")
    hlen = struct.unpack("<H", fw[4:6])[0]
    lo, ls, io, isz = struct.unpack("<4I", fw[25:41])
    af = fw[io:io + isz]
    if af[:4] != b"RKAF":
        sys.exit("нет RKAF")
    alen = struct.unpack("<I", af[4:8])[0]
    if struct.unpack("<I", af[alen:alen + 4])[0] != rkcrc(af[:alen]):
        sys.exit("CRC исходного RKAF не сходится")
    if hashlib.md5(fw[:-32]).hexdigest().encode() != fw[-32:]:
        sys.exit("MD5 исходного update.img не сходится — файл повреждён")
    n = struct.unpack("<I", af[0x88:0x8C])[0]
    parts = []
    for i in range(n):
        e = af[0x8C + i * 112:0x8C + (i + 1) * 112]
        name = e[:32].split(b"\0")[0].decode()
        nand_size, pos, nand_addr, padded, size = struct.unpack("<5I", e[92:112])
        parts.append(dict(raw=e, name=name, pos=pos, size=size, data=af[pos:pos + size]))
    return hlen, (lo, ls, io), af, parts


def patch_binary(path):
    data = bytearray(open(path, "rb").read())
    # PIE: первый PT_LOAD с p_offset=0, p_vaddr=0 -> VA == смещение в файле; проверим
    e_phoff, = struct.unpack("<I", data[0x1C:0x20])
    phnum, = struct.unpack("<H", data[0x2C:0x2E])
    for i in range(phnum):
        p_type, p_off, p_va = struct.unpack("<3I", data[e_phoff + i * 32:e_phoff + i * 32 + 12])
        if p_type == 1:
            if p_off != 0 or p_va != 0:
                sys.exit("неожиданная раскладка ylc-ebook")
            break
    for va, old, new in BIN_PATCHES:
        cur = bytes(data[va:va + 4])
        if cur == new:
            continue
        if cur != old:
            sys.exit(f"ylc-ebook другой версии: по адресу {va:#x} {cur.hex()} вместо {old.hex()}")
        data[va:va + 4] = new
    open(path, "wb").write(data)


def patch_daemon(path):
    # штатный запускатель (файл Digma) + куски из luma/daemon-mod.sh
    text = open(path).read()
    if "Luma mod" in text:
        sys.exit("запускатель уже с модом — нужен штатный (на книге: перезагрузить её)")
    mod = open(os.path.join(LUMA, "daemon-mod.sh")).read()
    setup = mod.split("# @setup\n", 1)[1].split("# @run\n", 1)[0]
    runblk = mod.split("# @run\n", 1)[1]
    loop = "\nwhile true; do\n"
    start = "\n    /opt/ylc-ebook/bin/ylc-ebook\n"
    if text.count(loop) != 1 or text.count(start) != 1:
        sys.exit("штатный ylc-daemon.sh другой версии: не нашёл цикл или строку запуска")
    text = text.replace(loop, "\n" + setup + loop.lstrip("\n"))
    text = text.replace(start, "\n" + runblk)
    open(path, "w").write(text)


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def enable_adb(root):
    # штатно только usb_ums_en: книга видна флешкой, ADB выключен
    cfg = os.path.join(root, "etc", "init.d", ".usb_config")
    text = open(cfg).read()
    if "usb_adb_en" not in text.split():
        open(cfg, "w").write("usb_adb_en\n" + text)


def squash(root, out_img):
    cmd = ["mksquashfs", root, out_img, "-noappend", "-comp", "zstd", "-b", "131072",
           "-all-root", "-pseudo-override", "-no-xattrs", "-quiet"]
    for p in PSEUDO:
        cmd += ["-p", p]
    run(*cmd)
    size = os.path.getsize(out_img)
    if size > ROOTFS_PART_BYTES:
        sys.exit(f"rootfs {size} байт не влезает в раздел {ROOTFS_PART_BYTES}")
    return size


def build_rootfs_adb_only(rootfs_img, out_img, work):
    root = os.path.join(work, "root")
    run("unsquashfs", "-no-xattrs", "-d", root, rootfs_img)
    enable_adb(root)
    return squash(root, out_img)


def build_rootfs(rootfs_img, out_img, work):
    root = os.path.join(work, "root")
    run("unsquashfs", "-no-xattrs", "-d", root, rootfs_img)
    enable_adb(root)
    so = os.path.join(LUMA, "build", "libluma.so")
    if not os.path.exists(so):
        sys.exit("нет luma/build/libluma.so — сначала ./luma/build.sh")

    opt = os.path.join(root, "opt", "luma")
    os.makedirs(os.path.join(opt, "fonts"), exist_ok=True)
    shutil.copy(so, opt)
    shutil.copy(os.path.join(LUMA, "luma.ini"), opt)
    for f in sorted(os.listdir(os.path.join(HERE, "fonts"))):
        if f.lower().endswith((".ttf", ".otf", ".txt")):
            shutil.copy(os.path.join(HERE, "fonts", f), os.path.join(opt, "fonts"))
    os.makedirs(os.path.join(opt, "icons"), exist_ok=True)
    for f in sorted(os.listdir(os.path.join(LUMA, "icons"))):
        if f.endswith(".bin"):
            shutil.copy(os.path.join(LUMA, "icons", f), os.path.join(opt, "icons"))
    daemon = os.path.join(root, "opt", "ylc-ebook", "bin", "ylc-daemon.sh")
    patch_daemon(daemon)
    os.chmod(daemon, 0o755)
    patch_binary(os.path.join(root, "opt", "ylc-ebook", "bin", "ylc-ebook"))
    with open(os.path.join(root, "etc", "luma-release"), "w") as f:
        f.write("LUMA_MOD=1\nBASE=" + open(os.path.join(root, "etc", "os-release")).read().split("RK_BUILD_INFO=")[-1])
    return squash(root, out_img)


def main():
    # отдельный режим для живого теста: пропатчить копию ylc-ebook, снятую с устройства
    if len(sys.argv) == 4 and sys.argv[1] == "--patch-binary":
        shutil.copy(sys.argv[2], sys.argv[3])
        patch_binary(sys.argv[3])
        print(f"пропатчено: {sys.argv[3]}")
        return
    if len(sys.argv) == 4 and sys.argv[1] == "--patch-daemon":
        shutil.copy(sys.argv[2], sys.argv[3])
        patch_daemon(sys.argv[3])
        return
    # --adb-only: официальная прошивка без мода, только с включённым ADB
    adb_only = "--adb-only" in sys.argv
    # oem — настройки программы и заставка сна: прошивка их сбрасывает. Пропускать можно,
    # только если на книге уже та же сборка: oem другой сборки может не подойти программе
    if "--keep-oem" in sys.argv:
        DROP.append("oem")
    args = [a for a in sys.argv[1:] if a not in ("--adb-only", "--keep-oem")]
    if len(args) != 2:
        sys.exit(__doc__)
    src, dst = args
    fw = open(src, "rb").read()
    hlen, (lo, ls, io), af, parts = parse(fw)
    print("исходник:", ", ".join(p["name"] for p in parts))

    with tempfile.TemporaryDirectory() as work:
        old_rootfs = os.path.join(work, "rootfs.orig")
        new_rootfs = os.path.join(work, "rootfs.img")
        rp = next(p for p in parts if p["name"] == "rootfs")
        open(old_rootfs, "wb").write(rp["data"])
        size = (build_rootfs_adb_only if adb_only else build_rootfs)(old_rootfs, new_rootfs, work)
        print(f"rootfs: {len(rp['data'])} -> {size} байт")
        rp["data"] = open(new_rootfs, "rb").read()

    keep = [p for p in parts if p["name"] not in DROP]
    pkg = next(p for p in keep if p["name"] == "package-file")
    lines = [l for l in pkg["data"].decode().splitlines()
             if not l.strip() or l.startswith("#") or l.split()[0] not in DROP]
    pkg["data"] = ("\n".join(lines) + "\n").encode()

    # новый RKAF: заголовок исходника, только нужные записи, данные с выравниванием 2048
    hdr = bytearray(af[:0x800])
    hdr[0x8C:0x8C + 112 * 16] = bytes(112 * 16)
    hdr[0x88:0x8C] = struct.pack("<I", len(keep))
    body = bytearray()
    pos = 0x800
    for i, p in enumerate(keep):
        e = bytearray(p["raw"])
        padded = (len(p["data"]) + 2047) // 2048
        nand_size, _, nand_addr, _, _ = struct.unpack("<5I", e[92:112])
        e[92:112] = struct.pack("<5I", nand_size, pos, nand_addr, padded, len(p["data"]))
        hdr[0x8C + i * 112:0x8C + (i + 1) * 112] = e
        body += p["data"] + bytes(padded * 2048 - len(p["data"]))
        pos += padded * 2048
    rkaf = hdr + body
    rkaf[4:8] = struct.pack("<I", len(rkaf))
    rkaf += struct.pack("<I", rkcrc(rkaf))

    rkfw = bytearray(fw[:io])  # заголовок RKFW + загрузчик как есть
    rkfw[33:41] = struct.pack("<II", io, len(rkaf))
    out = bytes(rkfw) + bytes(rkaf)
    out += hashlib.md5(out).hexdigest().encode()
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    open(dst, "wb").write(out)

    # самопроверка: разбираем результат тем же парсером
    _, _, _, chk = parse(out)
    print("собрано:", ", ".join(f"{p['name']} ({p['size']} байт)" for p in chk))
    print(f"готово: {dst} ({len(out)} байт)")


if __name__ == "__main__":
    main()
