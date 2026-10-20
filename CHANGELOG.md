# Changelog

Todas as alterações notáveis no projeto **OpenWirelessMC** estão documentadas neste arquivo.

O formato segue as diretrizes do [Keep a Changelog](https://keepachangelog.com/pt-BR/1.0.0/) e este projeto adota o [Versionamento Semântico](https://semver.org/lang/pt-BR/).

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
  - Mapeamento de 475 títulos multi-disco através do arquivo `/sdcard/multidisc.txt`.
  - Ao alternar para discos subsequentes (ex.: Disco 2 de *Final Fantasy*, *Metal Gear Solid* ou *Chrono Cross*), o sistema redireciona o arquivo montado para o cartão principal do Disco 1, mantendo o progresso unificado sem gerar cartões vazios indesejados.
- **Topologia de Barramento 100% Open-Drain:**
  - Reconfiguração das linhas `DATA` (MISO / GPIO 32) e `ACK` (GPIO 33) para modo dreno aberto com dreno ativo (`GPIO_MODE_INPUT_OUTPUT_OD`).
  - Compatibilidade com os resistores de pull-up da placa-mãe do console, prevenindo contenção de sinal quando outros periféricos (controles, multitap ou Memory Card no Slot 2) compartilham as linhas do barramento.
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
  - Núcleo 1 dedicado em modo baremetal com execução em IRAM a 240 MHz.
  - ACK com temporização de 84 ciclos (~350 ns), com suporte a rotinas de detecção rápida da BIOS e jogos sensíveis a timing.
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
