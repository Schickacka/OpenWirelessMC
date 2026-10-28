#!/usr/bin/env python3
import os
import sys
import shutil
import tempfile
import hashlib
import subprocess
import struct

PYTHON_EXE = sys.executable
SCRIPT_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "migrate_sd.py"))
PARENT_DB_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sdcard_assets", "multidisc.txt"))

sys.path.insert(0, os.path.dirname(__file__))
from migrate_sd import xor_checksum

def make_blank_card(pattern_byte=0x00) -> bytes:
    card = bytearray(131072)
    card[0:2] = b"MC"
    for b in range(1, 16):
        d_off = b * 128
        card[d_off : d_off + 4] = b"\xA0\x00\x00\x00"
        card[d_off + 4 : d_off + 8] = b"\x00\x00\x00\x00"
        card[d_off + 8 : d_off + 10] = b"\xFF\xFF"
    if pattern_byte != 0x00:
        d_off = 1 * 128
        card[d_off : d_off + 4] = b"\x51\x00\x00\x00"
        card[d_off + 4 : d_off + 8] = struct.pack("<I", 8192)
        card[d_off + 8 : d_off + 10] = b"\xFF\xFF"
        name = f"BASLUS-00421-{pattern_byte}".encode("ascii")
        card[d_off + 10 : d_off + 10 + len(name)] = name
        b1_off = 8192
        card[b1_off : b1_off + 2] = b"SC"
        card[b1_off + 2 : b1_off + 8192] = bytes([pattern_byte]) * 8190
    for f in range(1024):
        fo = f * 128
        card[fo + 127] = xor_checksum(card[fo : fo + 127])
    return bytes(card)

def run_cmd(cmd):
    print("$ " + " ".join(cmd))
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.stdout:
        print(res.stdout.strip())
    if res.stderr:
        print(res.stderr.strip())
    return res

def case_1():
    print("================================================================================")
    print("CASO 1: default.mc + BootCard-1.mcd existente -> CONFLITO (SHA-256 preservado)")
    print("================================================================================")
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("owmc_test")
        boot_path = os.path.join(td, "MemoryCards", "PS1", "BOOT", "BootCard-1.mcd")
        os.makedirs(os.path.dirname(boot_path), exist_ok=True)
        boot_content = make_blank_card(0x11)
        with open(boot_path, "wb") as f: f.write(boot_content)
        sha_before = hashlib.sha256(boot_content).hexdigest()

        def_path = os.path.join(td, "default.mc")
        def_content = make_blank_card(0x22)
        with open(def_path, "wb") as f: f.write(def_content)

        print(f"BootCard-1.mcd SHA-256 antes: {sha_before}")
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res = run_cmd(cmd)

        with open(boot_path, "rb") as f:
            sha_after = hashlib.sha256(f.read()).hexdigest()
        print(f"BootCard-1.mcd SHA-256 apos:  {sha_after}")
        assert sha_before == sha_after, "BootCard-1.mcd foi modificado!"
        print(f"CONFERENCIA: SHA-256 identico: {sha_before == sha_after}. Boot card intacto.")
    finally:
        shutil.rmtree(td, ignore_errors=True)
    print()

def case_2():
    print("================================================================================")
    print("CASO 2: SLUS-00421-0/-1/-2/-3 -> -0->4, -1->1, -2->2, -3->3 (duas passadas)")
    print("================================================================================")
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("owmc_test")
        # Criados em ordem propositalmente embaralhada
        for suff, b in [("3", 0x33), ("0", 0x01), ("2", 0x22), ("1", 0x11)]:
            with open(os.path.join(td, f"SLUS-00421-{suff}.mc"), "wb") as f:
                f.write(make_blank_card(b))

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        run_cmd(cmd)

        re2 = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421")
        files = sorted(os.listdir(re2))
        print("Arquivos gerados em MemoryCards/PS1/SLUS-00421/:")
        for f in files:
            print(f"  {f}")
        assert "SLUS-00421-1.mcd" in files
        assert "SLUS-00421-2.mcd" in files
        assert "SLUS-00421-3.mcd" in files
        assert "SLUS-00421-4.mcd" in files
        print("CONFERENCIA: Canais 1, 2, 3 reservados pelos explicitos; canal -0 alocado no 4.")
    finally:
        shutil.rmtree(td, ignore_errors=True)
    print()

def case_3():
    print("================================================================================")
    print("CASO 3: multidisc.txt custom cujo pai tem override -> FILHO = <pasta resolvida>")
    print("================================================================================")
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("owmc_test")
        # Override pre-existente: SLUS-00421 (pai) vai para RE2-CUSTOM
        sd2 = os.path.join(td, ".sd2psx")
        os.makedirs(sd2, exist_ok=True)
        with open(os.path.join(sd2, "Game2Folder.ini"), "w") as f:
            f.write("[PS1]\nSLUS-00421 = RE2-CUSTOM\n")

        # multidisc.txt na raiz com par custom: SLPS-01234 tem como pai SLUS-00421
        with open(os.path.join(td, "multidisc.txt"), "w") as f:
            f.write("SLPS-01234 = SLUS-00421 # Par customizado de usuario\n")

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        run_cmd(cmd)

        ini_path = os.path.join(td, ".sd2psx", "Game2Folder.ini")
        with open(ini_path, "r") as f:
            content = f.read()
        print("Conteudo final de .sd2psx/Game2Folder.ini:")
        print(content.strip())
        assert "SLPS-01234 = RE2-CUSTOM" in content
        print("CONFERENCIA: SLPS-01234 gravado com a pasta resolvida do pai (RE2-CUSTOM).")
    finally:
        shutil.rmtree(td, ignore_errors=True)
    print()

def case_4():
    print("================================================================================")
    print("CASO 4: Game2Folder.ini da raiz com [PS1] e [PS2] -> chave inserida dentro de [PS1]")
    print("================================================================================")
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("owmc_test")
        with open(os.path.join(td, "Game2Folder.ini"), "w") as f:
            f.write("[PS1]\nOLD_GAME = OLD_DIR\n\n[PS2]\nPS2_GAME = PS2_DIR\n")

        with open(os.path.join(td, "multidisc.txt"), "w") as f:
            f.write("SLPS-99999 = SLUS-00421\n")

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        run_cmd(cmd)

        ini_path = os.path.join(td, ".sd2psx", "Game2Folder.ini")
        with open(ini_path, "r") as f:
            content = f.read()
        print("Conteudo final de .sd2psx/Game2Folder.ini:")
        print(content.strip())
        lines = content.splitlines()
        ps1_idx = lines.index("[PS1]")
        ps2_idx = lines.index("[PS2]")
        child_line = [i for i, l in enumerate(lines) if "SLPS-99999" in l][0]
        assert ps1_idx < child_line < ps2_idx, "Chave nao foi inserida estritamente dentro de [PS1]!"
        print(f"CONFERENCIA: Chave SLPS-99999 na linha {child_line+1}, entre [PS1] (linha {ps1_idx+1}) e [PS2] (linha {ps2_idx+1}).")
    finally:
        shutil.rmtree(td, ignore_errors=True)
    print()

def case_5():
    print("================================================================================")
    print("CASO 5: --merge-policy merge: sucesso em <pasta>-1.mcd e fallback em keep")
    print("================================================================================")
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("owmc_test")
        c1 = make_blank_card(0x11)
        c2 = make_blank_card(0x22)
        with open(os.path.join(td, "SLUS-00421-1.mc"), "wb") as f: f.write(c1)
        with open(os.path.join(td, "SLUS-00421-2.mc"), "wb") as f: f.write(c2)

        print("--- Subcaso 5.1: Fusao com blocos livres disponíveis ---")
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--merge-policy", "merge", "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        run_cmd(cmd)

        fused_path = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421-1.mcd")
        assert os.path.isfile(fused_path), "SLUS-00421-1.mcd nao foi gerado!"
        trash_dir = os.path.join(td, ".sd2psx", "migrate-trash")
        quarantined = [f for _, _, files in os.walk(trash_dir) for f in files]
        print(f"Arquivo fundido gerado: {os.path.relpath(fused_path, td)}")
        print(f"Arquivos enviados para quarentena: {quarantined}")
        assert "SLUS-00421-1.mc" in quarantined and "SLUS-00421-2.mc" in quarantined
    finally:
        shutil.rmtree(td, ignore_errors=True)

    print()
    print("--- Subcaso 5.2: Fallback para 'keep' quando nao ha bloco livre ---")
    td2 = tempfile.mkdtemp()
    try:
        with open(os.path.join(td2, "config.txt"), "w") as f: f.write("owmc_test")
        # Cria Card 1 lotado (todos os 15 blocos ocupados)
        full_card = bytearray(make_blank_card())
        for b in range(1, 16):
            d_off = b * 128
            full_card[d_off : d_off + 4] = b"\x51\x00\x00\x00"
            full_card[d_off + 4 : d_off + 8] = struct.pack("<I", 8192)
            full_card[d_off + 8 : d_off + 10] = b"\xFF\xFF"
            name = f"FULL_SAVE_{b}".encode("ascii")
            full_card[d_off + 10 : d_off + 10 + len(name)] = name
            d_chk = 0
            for bb in full_card[d_off : d_off + 127]: d_chk ^= bb
            full_card[d_off + 127] = d_chk
        # Card 2 com outro save
        card2 = make_blank_card(0x99)

        with open(os.path.join(td2, "SLUS-00421-1.mc"), "wb") as f: f.write(full_card)
        with open(os.path.join(td2, "SLUS-00421-2.mc"), "wb") as f: f.write(card2)

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td2, "--merge-policy", "merge", "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        run_cmd(cmd)

        re2 = os.path.join(td2, "MemoryCards", "PS1", "SLUS-00421")
        files = sorted(os.listdir(re2))
        print("Arquivos gerados apos fallback:")
        for f in files:
            print(f"  {f}")
        assert "SLUS-00421-1.mcd" in files
        assert "SLUS-00421-2.mcd" in files
        print("CONFERENCIA: Sem blocos livres para fusao, recorreu com seguranca para 'keep' (canais 1 e 2).")
    finally:
        shutil.rmtree(td2, ignore_errors=True)

def main():
    case_1()
    case_2()
    case_3()
    case_4()
    case_5()

if __name__ == "__main__":
    main()
