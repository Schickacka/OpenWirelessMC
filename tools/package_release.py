#!/usr/bin/env python3
"""Monta o pacote de release do OpenWirelessMC a partir do build do PlatformIO.

Uso:
    python tools/package_release.py                 # usa .pio/build/esp32dev
    python tools/package_release.py --check         # so confere o que ja existe
    python tools/package_release.py --build-dir DIR

O que faz:
  * copia firmware.bin, bootloader.bin e partitions.bin do build para
    releases/v1.5_package/ (e para releases/firmware_v1.5.bin);
  * gera o ota_data_initial.bin (0x2000 bytes 0xFF) se faltar;
  * monta sdcard_assets.zip com config.txt, default.mc e titles.db
    (multidisc.txt NAO entra: o firmware nao o le e, na raiz do SD, ele
    apenas acende o aviso de layout antigo);
  * monta OpenWirelessMC_v1.5_Complete_Package.zip com o conteudo do pacote;
  * grava releases/SHA256SUMS-v1.5.txt e imprime tamanho + sha256.

Sem dependencias externas. Os nomes dos arquivos de release ficam estaveis
(linha 1.5); a versao real vem do app descriptor do firmware.
"""

import argparse
import hashlib
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG_DIR = os.path.join(ROOT, "releases", "v1.5_package")
REL_DIR = os.path.join(ROOT, "releases")
ASSETS = os.path.join(ROOT, "sdcard_assets")
OTA_BLANK = 0x2000
SDCARD_FILES = ["config.txt", "default.mc", "titles.db"]
BUILD_FILES = ["firmware.bin", "bootloader.bin", "partitions.bin"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def project_version():
    cmake = os.path.join(ROOT, "CMakeLists.txt")
    try:
        with open(cmake, "r", encoding="utf-8") as f:
            m = re.search(r"project\s*\(\s*openmc\s+VERSION\s+([0-9.]+)", f.read())
            if m:
                return m.group(1)
    except OSError:
        pass
    return "desconhecida"


def zip_dir_to(path, sources, base):
    """Zip deterministico: ordem fixa, data fixa (o conteudo e que importa)."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in sources:
            full = os.path.join(base, rel)
            info = zipfile.ZipInfo(rel, date_time=(2026, 10, 28, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(full, "rb") as f:
                z.writestr(info, f.read())


def check_only():
    ok = True
    print("versao do projeto: %s" % project_version())
    for name in ("firmware_v1.5.bin", "SHA256SUMS-v1.5.txt"):
        path = os.path.join(REL_DIR, name) if name.endswith(".txt") else os.path.join(PKG_DIR, name)
        if not os.path.isfile(path):
            print("FALTA: %s" % os.path.relpath(path, ROOT))
            ok = False
    for name in SDCARD_FILES:
        if not os.path.isfile(os.path.join(ASSETS, name)):
            print("FALTA: sdcard_assets/%s" % name)
            ok = False
    if os.path.isfile(os.path.join(PKG_DIR, "sdcard_assets.zip")):
        with zipfile.ZipFile(os.path.join(PKG_DIR, "sdcard_assets.zip")) as z:
            names = z.namelist()
            if "multidisc.txt" in names:
                print("AVISO: sdcard_assets.zip ainda distribui multidisc.txt")
                ok = False
            faltando = [n for n in SDCARD_FILES if n not in names]
            if faltando:
                print("AVISO: sdcard_assets.zip sem %s" % ", ".join(faltando))
                ok = False
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", default=os.path.join(ROOT, ".pio", "build", "esp32dev"))
    ap.add_argument("--check", action="store_true", help="nao escreve nada; so confere o pacote")
    args = ap.parse_args()

    if args.check:
        sys.exit(check_only())

    for name in BUILD_FILES:
        if not os.path.isfile(os.path.join(args.build_dir, name)):
            sys.exit("build ausente: %s (rode 'pio run' antes)" % os.path.join(args.build_dir, name))
    for name in SDCARD_FILES:
        if not os.path.isfile(os.path.join(ASSETS, name)):
            sys.exit("asset ausente: sdcard_assets/%s" % name)

    os.makedirs(PKG_DIR, exist_ok=True)

    # Binarios do build.
    for name in BUILD_FILES:
        src = os.path.join(args.build_dir, name)
        with open(src, "rb") as f:
            data = f.read()
        with open(os.path.join(PKG_DIR, name), "wb") as f:
            f.write(data)
        if name == "firmware.bin":
            with open(os.path.join(PKG_DIR, "firmware_v1.5.bin"), "wb") as f:
                f.write(data)
            with open(os.path.join(REL_DIR, "firmware_v1.5.bin"), "wb") as f:
                f.write(data)
    os.unlink(os.path.join(PKG_DIR, "firmware.bin"))

    # Seletor OTA em branco (0x2000 bytes 0xFF) se nao existir.
    ota = os.path.join(PKG_DIR, "ota_data_initial.bin")
    if not os.path.isfile(ota) or os.path.getsize(ota) != OTA_BLANK:
        with open(ota, "wb") as f:
            f.write(b"\xFF" * OTA_BLANK)

    # assets do SD (sem multidisc.txt).
    zip_dir_to(os.path.join(PKG_DIR, "sdcard_assets.zip"), SDCARD_FILES, ASSETS)

    # pacote completo.
    package_names = sorted(
        n for n in os.listdir(PKG_DIR)
        if n not in ("sdcard_assets.zip",) and os.path.isfile(os.path.join(PKG_DIR, n))
    ) + ["sdcard_assets.zip"]
    zip_dir_to(os.path.join(REL_DIR, "OpenWirelessMC_v1.5_Complete_Package.zip"),
               package_names, PKG_DIR)

    # soma de verificacao.
    sums = [
        ("OpenWirelessMC_v1.5_Complete_Package.zip", os.path.join(REL_DIR, "OpenWirelessMC_v1.5_Complete_Package.zip")),
        ("firmware_v1.5.bin", os.path.join(REL_DIR, "firmware_v1.5.bin")),
        ("v1.5_package/firmware_v1.5.bin", os.path.join(PKG_DIR, "firmware_v1.5.bin")),
        ("v1.5_package/sdcard_assets.zip", os.path.join(PKG_DIR, "sdcard_assets.zip")),
    ]
    with open(os.path.join(REL_DIR, "SHA256SUMS-v1.5.txt"), "w", encoding="utf-8") as f:
        for label, path in sums:
            f.write("%s  %s\n" % (sha256(path), label))

    fw = os.path.join(PKG_DIR, "firmware_v1.5.bin")
    print("versao do projeto: %s" % project_version())
    print("firmware_v1.5.bin: %d bytes" % os.path.getsize(fw))
    print("sha256:            %s" % sha256(fw))
    print("sdcard_assets.zip: %d bytes (config.txt, default.mc, titles.db)" %
          os.path.getsize(os.path.join(PKG_DIR, "sdcard_assets.zip")))
    print("sha256:            %s" % sha256(os.path.join(REL_DIR, "OpenWirelessMC_v1.5_Complete_Package.zip")))
    print("pacote completo:   %s" % os.path.join(REL_DIR, "OpenWirelessMC_v1.5_Complete_Package.zip"))


if __name__ == "__main__":
    main()
