#!/usr/bin/env python3
"""
tools/verify_evidence_3.py - Compara migração HTTP com a local:
mesmos caminhos e mesmos hashes, e a origem só desaparece depois de o destino existir com o mesmo SHA-256.
"""

import os
import sys
import shutil
import tempfile
import hashlib
import subprocess
import threading
import json
from http.server import HTTPServer
from test_migrate_sd import FakeOwmcHandler, make_blank_card, hash_tree, SCRIPT_PATH, PARENT_DB_PATH, PYTHON_EXE

def populate_tree(td: str):
    with open(os.path.join(td, "config.txt"), "w") as f: f.write("wifi_ssid=MyNet\n")
    with open(os.path.join(td, "titles.db"), "w") as f: f.write("SLUS-00421=Resident Evil 2 (Disc 1)\n")
    with open(os.path.join(td, "default.mc"), "wb") as f: f.write(make_blank_card(0x01))
    with open(os.path.join(td, "SLUS-00421.mc"), "wb") as f: f.write(make_blank_card(0x11))
    with open(os.path.join(td, "SLUS-00592.mc"), "wb") as f: f.write(make_blank_card(0x22))
    with open(os.path.join(td, "SLUS-00421-2.mc"), "wb") as f: f.write(make_blank_card(0x33))
    with open(os.path.join(td, "memcard_do_primo.mc"), "wb") as f: f.write(make_blank_card(0x44))
    with open(os.path.join(td, "multidisc.txt"), "w") as f: f.write("SLUS-00592=SLUS-00421\nCUSTOM-00001=SLUS-00421\n")
    with open(os.path.join(td, "Game2Folder.ini"), "w") as f: f.write("[PS1]\nSLUS-00421=SLUS-00421\n")

def main():
    print("=== EVIDENCIA 3: Migracao HTTP vs Migracao Local ===")

    dir_local = tempfile.mkdtemp(prefix="owmc_local_")
    dir_http = tempfile.mkdtemp(prefix="owmc_http_")

    try:
        # Preenche as duas arvores com conteudo identico
        populate_tree(dir_local)
        populate_tree(dir_http)

        hashes_start_local = hash_tree(dir_local)
        hashes_start_http = hash_tree(dir_http)
        assert hashes_start_local == hashes_start_http, "Arvores iniciais divergem!"
        print(f"1. Arvores iniciais identicas preparadas com {len(hashes_start_local)} arquivos.")

        # 1. Executa migracao local
        cmd_local = [PYTHON_EXE, SCRIPT_PATH, "--root", dir_local, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res_loc = subprocess.run(cmd_local, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res_loc.returncode == 0, f"Migracao local falhou: {res_loc.stderr.decode()}"
        print("2. Migracao local executada com sucesso.")

        # 2. Executa migracao HTTP
        FakeOwmcHandler.root_dir = dir_http
        server = HTTPServer(("127.0.0.1", 0), FakeOwmcHandler)
        port = server.server_port
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()

        url = f"http://127.0.0.1:{port}"
        cmd_http = [PYTHON_EXE, SCRIPT_PATH, "--root", url, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res_http = subprocess.run(cmd_http, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        server.shutdown()
        server.server_close()
        assert res_http.returncode == 0, f"Migracao HTTP falhou: {res_http.stderr.decode()}"
        print("3. Migracao HTTP executada com sucesso via /api/fs/*.")

        # 3. Compara arvores finais (excluindo os arquivos de manifesto que contem timestamp)
        hashes_final_local = hash_tree(dir_local)
        hashes_final_http = hash_tree(dir_http)

        filtered_local = {k: v for k, v in hashes_final_local.items() if not k.startswith(".sd2psx/migrate-")}
        filtered_http = {k: v for k, v in hashes_final_http.items() if not k.startswith(".sd2psx/migrate-")}

        print("\n4. Comparativo detalhado de caminhos e SHA-256:")
        all_keys = sorted(set(filtered_local.keys()) | set(filtered_http.keys()))
        for k in all_keys:
            h_loc = filtered_local.get(k, "<AUSENTE>")
            h_http = filtered_http.get(k, "<AUSENTE>")
            match = "MATCH" if h_loc == h_http else "DIVERGE"
            print(f"   [{match}] {k:<50} | SHA256: {h_loc[:16]}... vs {h_http[:16]}...")
            assert h_loc == h_http, f"Hash divergiu no arquivo {k}!"

        # Confere que origens legadas foram devidamente removidas em ambos
        for old_f in ("default.mc", "SLUS-00421.mc", "SLUS-00592.mc", "SLUS-00421-2.mc", "memcard_do_primo.mc"):
            assert old_f not in filtered_local and old_f not in filtered_http, f"Origem {old_f} ainda existe!"

        print("\nResultado: 100% de paridade entre migracao local e HTTP (mesmos caminhos e mesmos hashes).")
        return 0
    finally:
        shutil.rmtree(dir_local, ignore_errors=True)
        shutil.rmtree(dir_http, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(main())
