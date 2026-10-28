OpenWirelessMC v1.5.1 - Release Package
=======================================
Desenvolvido por Schickacka

Conteudo deste pacote
---------------------
  bootloader.bin        gravacao serial (0x1000)
  partitions.bin        tabela dual-OTA (0x8000)
  ota_data_initial.bin  inicializa o seletor OTA (0xd000)
  firmware_v1.5.bin     aplicacao (0x10000), versao 1.5.1
  sdcard_assets.zip     titles.db + config.txt para a raiz do cartao
  flash_windows.bat     gravador serial assistido

O que mudou na 1.5.1 (sobre a 1.5.0)
------------------------------------
  - BootCard-1.mcd invalido deixa de ser sobrescrito: vai para
    BootCard-1.mcd.invalid e um cartao novo e criado no lugar.
  - /api/rename exige nome simples e mantem a extensao do cartao
    (.mc/.mcd/.mcr). Renomear para .txt nao e mais aceito.
  - /api/card/name valida a pasta e o canal (1..254).
  - /api/fs/* nao aceita mais *.mcd/*.ini/*.bak fora de MemoryCards/PS1,
    .sd2psx e games; cartao solto na raiz nao e criado pela API.
  - /api/fs/delete so remove pasta VAZIA: nunca apaga o conteudo dela.
  - Upload por /api/fs/put nao bloqueia mais a gravacao dos saves do
    console enquanto o corpo e recebido.
  - /api/status conta os cartoes em MemoryCards/PS1 e informa a versao
    real do firmware.
  - Cartao criado com nome livre vai para UserCards (o nome fica em
    [ChannelName]), sem criar pasta com o nome digitado.

Layout do cartao MicroSD (v1.5)
-------------------------------
  /sdcard/
  |- config.txt                 (rede: veja o modelo em config.txt)
  |- titles.db                  (23.465 titulos; usado pela interface web)
  |- .sd2psx/Game2Folder.ini    (opcional: overrides de pasta, secao [PS1])
  |- MemoryCards/PS1/
     |- BOOT/BootCard-1.mcd     (criado pelo firmware no primeiro boot)
     |- UserCards/UserCards-N.mcd
     |- SLUS-00421/SLUS-00421-1.mcd   (uma pasta por jogo, um canal por arquivo)

  O firmware NAO le default.mc nem multidisc.txt da raiz. O default.mc e
  apenas a semente usada pela migracao da 1.4 para criar o Boot Card.

Gravacao serial (cabo USB)
--------------------------
  1. Conecte o ESP32 ao computador.
  2. Execute flash_windows.bat e informe a porta COM (ex.: COM6).
  3. O script grava bootloader, particoes, seletor OTA e firmware.

Atualizacao via Web OTA (sem fio)
---------------------------------
  1. Acesse http://wirelessmc.local/ (ou o IP do aparelho).
  2. Em "Atualizacao de Firmware", selecione firmware_v1.5.bin.
  3. Clique em "Atualizar Firmware". O ESP32 reinicia sozinho.

Cartao MicroSD novo
-------------------
  1. Formate em FAT32.
  2. Copie titles.db e config.txt (do sdcard_assets.zip) para a raiz.
  3. Ligue o aparelho: ele cria MemoryCards/PS1/BOOT e monta o Boot Card.

Vindo da v1.4 (layout antigo)
-----------------------------
  Nada de copiar arquivo a mao. Use tools/migrate_sd.py (Python 3):

    python tools/migrate_sd.py --root "X:\" --backup "%USERPROFILE%\owmc-backup"
    python tools/migrate_sd.py --root "X:\" --apply --yes
    python tools/migrate_sd.py --root "X:\" --verify

  Com o aparelho na rede, troque --root "X:\" por --root http://<ip> para
  migrar por Wi-Fi (console desligado durante a migracao).

  O script move default.mc, os cartoes <GAMEID>-<n>.mc e o
  multidisc.txt/Game2Folder.ini da raiz para o layout novo, preservando o
  numero do canal. Nada e apagado: o que nao tem destino vai para
  .sd2psx/migrate-trash/<data>/. Enquanto o SD estiver no layout antigo,
  /api/status responde "legacy":{"detected":true}.

Observacoes
-----------
  - Nao ha autenticacao: use o aparelho apenas na sua rede.
  - config.txt de fabrica sobe em modo AP (SSID WirelessMC). Troque a senha.
  - O cartao em uso no console nao pode ser apagado, sobrescrito ou
    renomeado pela interface; desmonte antes.
