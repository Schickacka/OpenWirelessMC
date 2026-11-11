OpenWirelessMC v1.5.3 - Release Package
=======================================
Desenvolvido por Schickacka

Conteudo deste pacote
---------------------
  bootloader.bin        gravacao serial (0x1000)
  partitions.bin        tabela dual-OTA (0x8000)
  ota_data_initial.bin  inicializa o seletor OTA (0xd000)
  firmware_v1.5.bin     aplicacao (0x10000), versao 1.5.3
  sdcard_assets.zip     default.mc + titles.db
  flash_windows.bat     gravador serial assistido

O que mudou na 1.5.3 (sobre a 1.5.2)
------------------------------------
  - Montagem por GameID agora procura o cartao em todos os canais da pasta
    do jogo (1..MaxChannels, com precedencia para o canal 1). Um save em
    -2.mcd ou -3.mcd e montado em vez de um cartao novo em branco ser
    criado ao lado.
  - Arquivo de cartao com tamanho invalido (diferente de 128 KiB) vai para
    <nome>.invalid em vez de ser sobrescrito.
  - Mover cartao entre pastas preserva o numero do canal quando o destino
    esta livre; mover para a propria pasta nao faz nada.
  - Configuracao de rede (mode, ssid, password) passa a ficar na NVS do
    ESP32 e sobrevive a formatar, trocar ou remover o cartao SD.
  - /api/ota recusa firmware maior que a particao de OTA (HTTP 413) antes
    de apagar qualquer setor e aborta a gravacao em caso de erro. A
    interface bloqueia o arquivo antes de enviar.

O que mudou na 1.5.2 (sobre a 1.5.1)
------------------------------------
  - Troca a quente de Memory Cards com settle delay de 2500 ms: a BIOS do PS1
    detecta a desconexao ao trocar de cartao pela web e rele o diretorio sem
    sair do menu. GameID repetido do cartao ja montado nao desconecta o barramento.
  - Higienizacao visual automatica de titulos multi-disco na Web UI: titulos
    como Chrono Cross (Disc 1) e Driver 2 (Disc 1) passam a ser exibidos de
    forma limpa como Chrono Cross e Driver 2, mantendo as tags de regiao.

O que mudou na 1.5.1 (sobre a 1.5.0)
------------------------------------
  - BootCard-1.mcd invalido deixa de ser sobrescrito: vai para
    BootCard-1.mcd.invalid e um cartao novo e criado no lugar.
  - /api/rename exige nome simples e mantem a extensao do cartao
    (.mc/.mcd/.mcr). Renomear para .txt nao e mais aceito.
  - /api/card/name valida a pasta e o canal (1..254).
  - /api/fs/* nao aceita mais *.mcd/*.ini/*.bak fora de MemoryCards/PS1,
    .sd2psx e games; cartao solto na raiz nao e criado pela API.
  - /api/fs/delete remove a pasta quando nao ha Memory Card dentro (os
    rotulos [ChannelName] vao junto). Com cartao dentro, recusa. As raizes
    .sd2psx/MemoryCards so saem vazias: nunca perdem conteudo.
  - Upload por /api/fs/put nao bloqueia mais a gravacao dos saves do
    console enquanto o corpo e recebido.
  - /api/status conta os cartoes em MemoryCards/PS1 e informa a versao
    real do firmware.
  - Cartao criado com nome livre vai para UserCards (o nome fica em
    [ChannelName]), sem criar pasta com o nome digitado.

Layout do cartao MicroSD (v1.5)
-------------------------------
  /sdcard/
  |- titles.db                     (23.465 titulos; usado pela interface web)
  |- .sd2psx/Game2Folder.ini       (opcional: overrides de pasta, secao [PS1])
  |- .sd2psx/config.txt.imported   (config.txt antigo, ja importado, se houver)
  |- MemoryCards/PS1/
     |- BOOT/BootCard-1.mcd        (criado pelo firmware no primeiro boot)
     |- UserCards/UserCards-N.mcd
     |- SLUS-00421/SLUS-00421-1.mcd   (uma pasta por jogo, um canal por arquivo)

  A rede NAO fica mais no cartao: mode/ssid/password moram na NVS do ESP32.
  Se existir um /sdcard/config.txt de versao antiga, ele e lido uma unica
  vez, importado para a NVS e movido para .sd2psx/config.txt.imported.
  O firmware NAO le default.mc nem multidisc.txt da raiz; o default.mc e
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
  2. Copie titles.db para a raiz (o default.mc so serve para a migracao).
  3. Ligue o aparelho: ele cria MemoryCards/PS1/BOOT e monta o Boot Card.
  4. Configure a rede pela interface web; nao ha arquivo de configuracao
     no cartao.

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
  - Sem configuracao salva, o aparelho sobe em modo AP
    (SSID WirelessMC / senha SenhaDificil!). Troque a senha.
  - Se o modo client nao conectar apos 5 tentativas, ele cai no mesmo AP
    de recuperacao, sem apagar a configuracao salva na NVS.
  - O cartao em uso no console nao pode ser apagado, sobrescrito ou
    renomeado pela interface; desmonte antes.
