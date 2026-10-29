# Changelog

Todas as alterações notáveis no projeto **OpenWirelessMC** estão documentadas neste arquivo.

O formato segue as diretrizes do [Keep a Changelog](https://keepachangelog.com/pt-BR/1.0.0/) e este projeto adota o [Versionamento Semântico](https://semver.org/lang/pt-BR/).

---

## [1.5.2] - 2026-10-28

### Adicionado
- **Troca a Quente Dinâmica no Console (Bus Settle Delay de 2500 ms):** Ao alternar entre cartões virtuais via Web UI com o console ligado, o barramento SIO é desativado por 2500 ms antes da montagem da nova imagem. Isso permite que a BIOS do PS1 detecte a desconexão e releia o diretório na tela de Memory Card sem exigir saída do menu, inclusive na troca de disco de jogos que compartilham o mesmo save (ex.: Resident Evil 2). Um GameID repetido para o cartão já montado não desconecta o barramento.
- **Higienização Visual de Títulos de Jogos Multi-Disco (sanitizeGameTitle):** Implementada remoção automática no frontend (web_assets/footer.html) de marcadores de disco como (Disc 1), (Disc 2), [Disc X] e equivalentes, mantendo a listagem limpa apenas com o nome principal do título e as tags de região ((US), (JP), (EU)). Compatibilidade integral com cache do navegador e preservação total das entradas originais em `titles.db`.

---

## [1.5.1] - 2026-10-28

### Corrigido
- **Cartão de boot preservado:** `MountBootCard()` não sobrescreve mais um `BootCard-1.mcd` com header inválido. O arquivo é movido para `BootCard-1.mcd.invalid` e um cartão novo é criado no lugar; antes, o `fopen(..., "wb")` apagava o conteúdo sem cópia.
- **`/api/rename` com validação de destino:** o nome novo precisa ser um nome simples (sem `/`, `\`, `:`, `*`, `?`, `"`, `<`, `>`, `|`) e manter a **mesma extensão** da origem (`.mc`, `.mcd` ou `.mcr`). Renomear `Cartao-1.mcd` para `Cartao.txt` deixou de ser possível.
- **`/api/card/name` valida pasta e canal:** `folder` passa por `IsValidFolderName()` e `channel` fica em `1..254`; antes qualquer string ia direto para o caminho do `.ini`.
- **Allowlist de `/api/fs/*` sem catch-all:** `*.mcd`, `*.mc`, `*.mcr`, `*.sav`, `*.ini` e qualquer nome com `.bak` só são aceitos dentro de `MemoryCards/PS1/`, `.sd2psx/` e `games/` (mais os três arquivos legados da raiz). Cartão solto na raiz deixa de ser criado/gravado pela API — era o que o teste 11 já exigia.
- **`/api/fs/delete` com regra de pasta:** uma pasta de jogo é removida quando **não há Memory Card dentro**; os arquivos auxiliares (rótulos `[ChannelName]` em `.ini`, `.bak`) vão junto, para a pasta não ficar impossível de apagar depois que o usuário exclui os cartões. Pasta com cartão, subpasta ou as raízes (`MemoryCards`, `MemoryCards/PS1`, `.sd2psx`, `games`) continuam recusadas — nelas nada é apagado em cascata.
- **Upload sem prender o `StorageLock`:** `/api/fs/put` toma o mutex apenas nas checagens iniciais e no commit (`.part` → `.bak` → rename). O recebimento do corpo roda fora do lock, então a tarefa que grava os saves do console não fica bloqueada durante um upload longo. O commit revalida se o cartão foi montado no meio do envio.
- **`/api/fs/put` com percent-decode e limite de 8 MB:** caminhos codificados (`%20`) deixam de virar nomes literais e corpos acima de `MAX_FILE_SIZE` são recusados com `413`.
- **`/api/status` com contagem correta:** `ps1_count`/`mc_count` percorrem `MemoryCards/PS1/<pasta>/*.mcd` (antes contavam na raiz do SD, sempre zero no layout novo) e `version` vem do app descriptor (`esp_ota_get_app_description()`), com fallback só para builds de host.
- **Cartão com nome livre volta para `UserCards`:** `/api/card/create` e `/api/create` com nome que não é GameID criam `MemoryCards/PS1/UserCards/UserCards-<n>.mcd` e gravam o nome em `[ChannelName]`, em vez de criar uma pasta com o nome digitado.
- **Testes:** `tests/host/run.sh` limpa `tests_sdcard/` antes de rodar (restos de execução interrompida mudavam o resultado), executa as guardas de tamanho de `test_ram.cc` e o suite fica verde (antes abortava em `test_web.cc:75`). Arquivos de teste avulsos e não compiláveis foram movidos para `tests/host/manual/` com explicação.

### Alterado
- **Documentação (`README.md`):** 475 pares (não 471), tabela de pais **compilada** em vez de parser de `/sdcard/multidisc.txt`, árvore `MemoryCards/PS1/<pasta>/<pasta>-<canal>.mcd` documentada, `default.mc` explicado como semente de migração (não é mais lido da raiz) e seção 6.2 com o roteiro de atualização da 1.4 via `tools/migrate_sd.py`.
- **Pacote de release:** `multidisc.txt` removido de `sdcard_assets.zip` (não é lido pelo firmware e, na raiz do SD, só acende `legacy.detected`); `README.txt` reescrito com o layout novo e o caminho de upgrade.

---

## [1.5.0] - 2026-10-28

### Adicionado
- **Duplicação de Cartões de Memória (`POST /api/card/duplicate`):** Duplicação atômica de arquivos de save de 128 KB em DRAM e MicroSD com incremento automático para o próximo canal livre (ex.: `Jogo-1.mcd` para `Jogo-2.mcd`), alocação segura em heap dinâmico e proteção de concorrência com `StorageLock`.
- **Renomeação Direta e Segura de Arquivos (`POST /api/rename`):** Suporte à renomeação de imagens de Memory Card pela interface web, com validação de caminhos absolutos sob o cartão SD, retenção obrigatória de extensões permitidas (`.mc`, `.mcd`, `.mcr`), detecção de colisão de arquivos existentes e bloqueio contra renomeação do cartão atualmente em uso no console (`storage::loaded_file_path`).
- **Criação e Montagem Automatizada de Cartões (`POST /api/card/create` e `/api/create`):** Criação de novos cartões virtuais com template oficial de fábrica Sony (sd2psX), resolução automática de pastas por título/GameID e alocação do primeiro canal livre.
- **Filtro de Pastas na Interface Web:** Alternador de exibição de pastas na raiz de saves (`Todas as Pastas`, `Com Saves`, `Apenas Vazias`), permitindo auditoria visual rápida da estrutura do MicroSD.
- **Contadores de Conteúdo por Diretório:** Exibição dinâmica da quantidade de cartões presentes dentro de cada pasta na listagem principal (`X cartões` ou `0 cartões` para pastas vazias).
- **Migrador de Estrutura de SD (`tools/migrate_sd.py`):** Script de linha de comando para verificação, limpeza e migração do layout de pastas legado para a hierarquia estruturada `/MemoryCards/PS1/<Jogo>/`.
- **Pacote de Distribuição v1.5:** Inclusão dos binários compilados (`firmware_v1.5.bin`, bootloader, partições dual-OTA) e pacote zip completo em `releases/v1.5_package/`.

### Modificado
- **Centralização da Navegação em `/MemoryCards/PS1/`:** Redirecionamento da raiz de visualização do gerenciador de saves para a pasta principal de jogos, ocultando a raiz técnica de arquivos do MicroSD e eliminando botões de navegação redundantes ("Raiz das Pastas").
- **Ordenação Natural de Títulos (Natural Collation):** Extração limpa do nome do jogo antes de avaliar tags regionais como `(US)`, assegurando ordenação alfanumérica consistente de sequências e franquias (ex.: *Dino Crisis 1* listado antes de *Dino Crisis 2*).
- **Padronização Visual da Interface:** Unificação de todos os botões de ação (Montar, Baixar, Duplicar, Renomear, Excluir) com altura fixa de 30 px, cantos arredondados consistentes, transparência moderna e alinhamento tipográfico.
- **Tratamento Percent-Decode no Servidor HTTP:** Implementação de decodificação percentual de URIs (`web::PercentDecode`) nas rotas `/api/fs/delete` e `/api/fs/list`, resolvendo falhas de exclusão e listagem de diretórios ou arquivos contendo espaços e caracteres codificados (`%20`, `%2F`).
- **Exclusão de Saves sem Interrupção Visual:** Transição do envio de formulário padrão para exclusão assíncrona via AJAX (`deleteCard()`) com notificação em tela (toast), eliminando o redirecionamento indesejado para páginas de resposta JSON bruta.
- **Headroom de Memória e Pilha do Servidor Web:** Ampliação da pilha da tarefa do servidor HTTP para 10240 bytes e capacidade de até 36 rotas simultâneas em `file_server.cc`, prevenindo estouro de pilha durante operações intensivas de I/O em JSON.

---

## [1.4.0] - 2026-10-21

### Corrigido
- **GameID `0x21`:** o firmware 1.3 deslocava o GameID em um byte (gravava o comprimento em `game_id[0]` e iniciava os caracteres em `command_ticks - 2`). A comutação automática de cartão agora usa o layout real do protocolo: tick 2 = comprimento, tick 3 = primeiro caractere.
- **FastRead `0x42`:** passou a transmitir páginas consecutivas de 128 bytes até `SEL` subir, sem checksum/`0x47` por página, em vez de encerrar após um setor como uma leitura `0x52`.
- **`default.mc` inválido:** o asset distribuído no 1.3 era todo zerado (checksum do setor 0 `0x00` em vez de `0x0E`). O firmware agora valida tamanho, assinatura `MC` e checksum; se inválido, move para `default.mc.invalid` e cria um Boot Card novo. `tools/regenerate_default_mc.py` regenera o asset.
- **Vazamento do barramento SPI:** uma falha de mount não chamava `spi_bus_free()`, impedindo o *hot remount* por `/api/mount_sd`. O barramento agora é liberado em falha e no unmount.
- **Flag de diretório (setor 0):** após commit do setor 0, o cartão volta a reportar `0x08` ("diretório não lido"), como o OpenMC original e o hardware real.
- **Comandos MMCE `0x22`–`0x26`:** não são mais respondidos com sucesso falso (`0x20`) sem trocar nada; caem no caminho de comando não suportado.
- **SSID de 32 caracteres:** o buffer de status foi ampliado para 33 bytes e as cópias para `wifi_config` deixaram de truncar em 31 caracteres.
- **Documentação:** 240 MHz/350 ns e 475 títulos multi-disco foram corrigidos para 160 MHz/~525 ns nominais e 475 pares ativos; o exemplo de multi-disco agora usa IDs presentes em `multidisc.txt`.
- **`sdkconfig` raiz:** XTAL corrigido de 26 MHz para 40 MHz, alinhado ao `sdkconfig.esp32dev` usado pelo build PlatformIO.
- **Testes host:** stubs de `esp_http_server.h`/`esp_ota_ops.h` atualizados (`stack_size`, `esp_app_desc_t`, `esp_ota_get_partition_description`), permitindo compilar `test_web`.

---

## [1.3.0] - 2026-10-20

### Adicionado
- **Documentação Arquitetural Comparativa Completa:**
  - Seção detalhada e aprofundada no `README.md` documentando todas as divergências técnicas e de engenharia entre o OpenWirelessMC e o projeto original OpenMC (topologia de barramento, isolamento de núcleos com Core 0 stall, persistência in-place por setor, sincronização GameID multicore, mapeamento multi-disco, template Sony sd2psX, cache versionado do cliente e alocação de DRAM).
- **Pacote de Distribuição v1.3:**
  - Lançamento do pacote `v1.3_package` e arquivo `OpenWirelessMC_v1.3_Complete_Package.zip` com binários dual-OTA e scripts atualizados.

### Corrigido
- **Restauração do Acionamento Push-Pull Ativo para MISO/DATA e ACK:**
  - Reversão do modo Open-Drain puro que causava rampa de subida lenta (tempo RC) devido aos resistores de pull-up fracos (~10kΩ) da placa-mãe do PS1 na linha DATA.
  - O ESP32 opera em Push-Pull ativo (3.3V com tempos de subida de nanossegundos) quando selecionado (`ActiveMode`), garantindo leitura íntegra e sem corrupção de checksum em alta velocidade no *Crash Bandicoot* e jogos que utilizam o SDK Sony com amostragem rápida ("Checking Format" -> formato reconhecido com sucesso imediato).
  - Em modo inativo (`PassiveMode`), os pinos permanecem em alta impedância (*Hi-Z* via `GPIO_ENABLE1_W1TC_REG`), mantendo total isolamento elétrico contra contenção de barramento com controles e Slot 2.
- **Restauração da Sequência Contínua de Leitura sem Aborto Prematuro de ACK:**
  - Remoção de cortes de ACK intempestivos em `TickReadCommand` nos setores limítrofes, assegurando confirmação contínua de blocos segundo a especificação oficial de Memory Card PS1.
- **Isolamento de Barramento no Tratador de Interrupção SPI:**
  - Restauração de `core0_stall_end()` ao término completo do frame do barramento (incluindo desasserção de SEL), prevenindo interferência de tarefas de segundo plano durante transações críticas de I/O.

---

## [1.2.0] - 2026-10-20

### Adicionado
- **Cache Versionado no Cliente para `titles.db`:**
  - Armazenamento local no navegador via `localStorage`, permitindo renderização instantânea do catálogo de jogos (< 5 ms) ao abrir a interface web.
  - Validação de integridade atômica via endpoint `/api/status`, que retorna `titles_version` calculado a partir do hash `size ^ mtime` do arquivo físico no cartão MicroSD.
  - Sincronização automática em segundo plano: se o arquivo no cartão SD for atualizado pelo usuário, o navegador detecta a alteração de versão e faz o download delta sem intervenção manual.
  - Zero jogos comerciais hardcoded nos arquivos HTML/JS: todos os títulos são lidos exclusivamente do `titles.db`.
- **Identificação Oficial do Cartão Padrão:**
  - Mapeamento explícito de `default.mc` para o nome de exibição **"Boot Card"** na tabela de saves e no banner de status ativo.
- **Expansão de Títulos no Banco de Dados:**
  - Inclusão dos IDs de disco para *Policenauts* (`SLPS-00215` Disco 1 e `SLPS-00216` Disco 2).
  - Resolução dinâmica correta para *Resident Evil 3: Nemesis* (`SLUS-00923`).
- **Verificação de Cabeçalho na Atualização Web OTA:**
  - Validação do cabeçalho binário da aplicação (`esp_ota_get_partition_description`) antes de ativar a partição de boot, prevenindo gravações acidentais de arquivos corrompidos ou incompatíveis.

### Modificado
- **Otimização Crítica de Memória DRAM:**
  - Reconfiguração dinâmica dos buffers de recepção e transmissão da pilha Wi-Fi (`CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=6`, `CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM=16`, `CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM=16`).
  - Redução da janela de recepção Wi-Fi (`CONFIG_ESP32_WIFI_RX_BA_WIN=4`) e ajuste dos buffers TCP da pilha LWIP (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT=4320`, `CONFIG_LWIP_TCP_WND_DEFAULT=4320`).
  - Redução do buffer estático de logs web de 16 KB para 8 KB, eliminando picos de alocação no heap.
  - Memória DRAM interna livre restabelecida para **~45 KB** (com marca d'água mínima em 35-42 KB), garantindo estabilidade contra quedas por falta de memória (*OOM panic*) durante gravações pesadas de saves concomitantes com requisições HTTP.
- **Pacote de Distribuição:**
  - Disponibilização do pacote `v1.2_package` contendo scripts de gravação automatizada via porta serial (`flash_windows.bat`), binários dual-OTA e conjunto completo de assets para o cartão MicroSD.

---

## [1.1.0] - 2026-10-19

### Adicionado
- **Suporte Inteligente a Jogos Multi-Disco (Multi-Disc Parent Mapping):**
  - Mapeamento de 475 títulos multi-disco através da tabela de pais compilada em `src/storage/parent_db.cc` (o arquivo `multidisc.txt` é apenas a fonte dessa tabela).
  - Ao alternar para discos subsequentes (ex.: Disco 2 de *Final Fantasy*, *Metal Gear Solid* ou *Chrono Cross*), o sistema redireciona o arquivo montado para o cartão principal do Disco 1, mantendo o progresso unificado sem gerar cartões vazios indesejados.
- **Topologia de Barramento Open-Drain (experimento revertido na 1.3):**
  - Reconfiguração das linhas `DATA` (MISO / GPIO 32) e `ACK` (GPIO 33) para modo dreno aberto com dreno ativo (`GPIO_MODE_INPUT_OUTPUT_OD`).
  - **Atenção:** essa topologia foi a causa da falha de leitura do *Crash Bandicoot* ("Card is not Formatted") por rampa RC lenta nos bits `1`. Ela foi revertida para Push-Pull Ativo + Hi-Z na 1.3. Não reintroduzir Open-Drain sem revalidar o console; ver documentação da v1.4, seção 3.
- **Desmontagem Segura e Remontagem a Quente (Hot Remount):**
  - Endpoints dedicados para desmontar a pilha FAT32 via interface web, permitindo troca do cartão físico sem desligar o console.
  - Opção de remontagem imediata pela interface.

### Corrigido
- **Alocação Estática de Rastreamento de Setores:**
  - Substituição da alocação dinâmica por ponteiro em `saved_version` por matriz estática na memória BSS, eliminando fragmentação contínua de heap no Core 0.
- **Expansão do Buffer de Traces SIO:**
  - Aumento da fila circular de eventos SIO de 8 para 32 posições, prevenindo perda de comandos de alta velocidade durante trocas de cartões virtuais.

---

## [1.0.0] - 2026-10-14

### Adicionado
- **Emulação Completa do Barramento SIO do PS1:**
  - Núcleo 1 dedicado em modo baremetal com execução em IRAM. O build de 1.3 usa 160 MHz (`CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ=160`); a menção anterior a 240 MHz não correspondia ao binário distribuído.
  - ACK com atraso calibrado empiricamente de 84 iterações de espera (referência teórica de ~525 ns a 160 MHz, dependente do código gerado), com suporte a rotinas de detecção rápida da BIOS e jogos sensíveis a timing. O valor de ~350 ns só faria sentido a 240 MHz e não deve ser usado como referência do build atual.
  - Imagem de 128 KB mantida em DRAM com espelhamento contínuo.
- **Gravação In-Place por Setor:**
  - Escrita direta no MicroSD utilizando abertura em modo `r+b` e endereçamento por setor (128 bytes).
  - Controle de versão atômico por setor (`sector_version`).
  - Redução drástica de ciclos de apagamento do cartão MicroSD e proteção contra corrupção de imagens em quedas de energia.
- **Servidor Web Embarcado e Atualização Web OTA:**
  - Interface web responsiva acessível via `http://wirelessmc.local/`.
  - Gerenciamento de saves (download, upload, renomeação, criação e exclusão).
  - Tabela de partição dual-OTA com atualização de firmware sem fio.
- **Ponto de Acesso de Resgate (Rescue AP):**
  - Fallback automático para AP em memória RAM (`WirelessMC`) caso a rede configurada em `config.txt` não esteja acessível.
