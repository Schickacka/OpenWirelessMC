# OpenWirelessMC

Emulador de Memory Card sem fio para PlayStation 1 baseado no microcontrolador ESP32, com armazenamento em cartão MicroSD via barramento SPI, servidor web embarcado para gerenciamento remoto de saves, banco de dados global de títulos com cache dinâmico versionado e atualização de firmware over-the-air (Web OTA).

Desenvolvido por **Schickacka**, baseado na engenharia reversa do projeto [OpenMC](https://github.com/johnbaumann/OpenMC).

---

## 1. Visão Geral da Arquitetura

O OpenWirelessMC implementa a emulação completa do protocolo serial SIO do PlayStation 1 em tempo real, dividindo as operações de barramento e gerenciamento entre os dois núcleos do ESP32:

* **Núcleo 1 (Baremetal / SIO dedicado):**
  * Executa a máquina de estados do barramento SPI/SIO da Sony diretamente a partir da memória IRAM, sem sobrecarga de sistema operacional.
  * Temporização de resposta ACK calibrada empiricamente para 84 iterações de espera no Core 1. Cada iteração inclui leitura de registrador e desvio, portanto o tempo em nanossegundos depende do código gerado; a referência teórica de 84 ciclos a **160 MHz** (`CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ=160`, valor real do build de 1.3) é de aproximadamente **525 ns**, e não os 350 ns que uma versão anterior do README atribuía a 240 MHz. O valor é calibrado contra o console, não derivado de uma frequência teórica, e não deve ser alterado sem revalidar o *Crash Bandicoot* e os demais títulos sensíveis a timing.
  * Topologia de barramento com **Push-Pull Ativo e Alta Impedância Dinâmica** nas linhas `DATA` (GPIO 32) e `ACK` (GPIO 33): operam em acionamento ativo de 3.3V durante a seleção de periférico (`ActiveMode`) para transições de borda ultra-rápidas sem atraso de subida RC, e entram em alta impedância absoluta (*Hi-Z*) em modo inativo (`PassiveMode`), garantindo isolamento total contra contenção de barramento com controles, multitaps e periféricos conectados ao Slot 2.
  * Imagem ativa de 128 KB mantida em DRAM interna (`memory_card_ram`), permitindo leitura e gravação com latência zero durante as transações do barramento do console.
  * Inicialização ultrarrápida do SIO (~1.75 s), ficando operacional aproximadamente 3.5 segundos antes do término da animação de boot da BIOS no PS1.

* **Núcleo 0 (FreeRTOS / Armazenamento e Rede):**
  * **Mecanismo de Escrita In-Place por Setor:** Gravação de alterações no MicroSD em modo `r+b` com deslocamento de ponteiro (`fseek`). Apenas os setores efetivamente modificados (128 bytes por setor) são gravados na mídia física, reduzindo expressivamente o desgaste das células flash do cartão SD e eliminando o risco de corrupção total da imagem em caso de desligamento abrupto do console.
  * **Controle Atômico de Versão de Setores:** Matriz estática de rastreamento (`sector_version[1024]`) alocada em memória BSS, prevenindo perda de dados quando o console realiza gravações sucessivas e evitando fragmentação do heap.
  * **Exclusão Mútua (`StorageLock`):** Mutex recursivo que sincroniza com segurança os acessos simultâneos entre o servidor HTTP, a rotina de detecção automática de GameID e a tarefa de persistência de disco.
  * **Suporte Inteligente a Jogos Multi-Disco (Multi-Disc Parent Mapping):** Mapeamento automático de 471 títulos com múltiplos discos através do arquivo `/sdcard/multidisc.txt` (contagem real do arquivo distribuído em `sdcard_assets/`; versões anteriores do README diziam 475). Ao trocar para discos subsequentes (ex.: Disco 2 de *Final Fantasy*, *Metal Gear Solid*, *Resident Evil 2*, *Policenauts*), o sistema redireciona o arquivo montado para o cartão principal do Disco 1, preservando o progresso contínuo sem gerar cartões vazios indesejados.
  * **Catálogo Global com Cache Versionado Dinâmico:** Identificação de 23.465 títulos catalogados no MicroSD em `titles.db`. O navegador cliente realiza o armazenamento local via `localStorage`, renderizando a biblioteca em menos de 5 ms sem tráfego de rede. O endpoint `/api/status` valida a versão do banco através do hash `size ^ mtime` do arquivo no SD (`titles_version`), atualizando o cache automaticamente em caso de modificação na mídia. Zero jogos comerciais hardcoded nos arquivos de firmware ou HTML.
  * **Cartão Inicial Padronizado:** O arquivo base `default.mc` é identificado e exibido oficialmente como **"Boot Card"** na interface web.
  * **Otimização Crítica de Memória DRAM:** Dimensionamento dinâmico dos buffers de transmissão/recepção Wi-Fi e pilha TCP/IP LWIP, preservando ~45 KB de DRAM interna livre para prevenir qualquer risco de esgotamento de memória (*OOM panic*) durante acessos simultâneos de rede e disco.
  * **Servidor Web Embarcado:** Interface acessível via navegador em `http://wirelessmc.local/` (ou pelo endereço IP do dispositivo), com suporte a upload, download, renomeação, criação de novos cartões formatados, desmontagem/remontagem a quente (*hot remount*) e atualização de firmware over-the-air (**Web OTA**) com verificação de cabeçalho de integridade.

---

## 2. Diferenças Arquiteturais em Relação ao Projeto Original (OpenMC)

Para desenvolvedores, engenheiros de firmware e analistas de código, esta seção detalha minuciosamente todas as divergências técnicas entre a implementação original do [OpenMC](https://github.com/johnbaumann/OpenMC) (upstream) e as inovações introduzidas no **OpenWirelessMC**, explicando as motivações de projeto e os problemas de hardware resolvidos:

| Componente / Recurso | Projeto Original (OpenMC) | OpenWirelessMC (Nosso Projeto) | Motivação Técnica / Benefício |
| :--- | :--- | :--- | :--- |
| **Topologia de Barramento (MISO / ACK)** | Open-Drain estático ou acionamento genérico | **Push-Pull Ativo (3.3V) + Hi-Z Dinâmico** | Pull-ups fracos (~10kΩ) no PS1 causam rampa RC lenta em Open-Drain, corrompendo leituras rápidas (*Crash Bandicoot* falhava com *"Card is not Formatted"*). Push-Pull com Hi-Z garante bordas de subida <10 ns sem contenção de barramento. |
| **Isolamento de Núcleos (Core 1 vs Core 0)** | Execução concorrente com concorrência de memória | **Core 1 Baremetal com Core 0 Stall estendido** | O Core 0 é paralisado por registrador (`core0_stall_start`) durante todo o frame SPI e só é liberado após o retorno de `SEL` para HIGH, evitando jitter de cache/RAM causado por Wi-Fi ou SD. |
| **Persistência de Saves no MicroSD** | Reescrita destrutiva do arquivo inteiro (`fopen("wb")`) | **Gravação In-Place Cirúrgica por Setor (`r+b` / `fseek`)** | Sobrescrever 128 KB truncava o arquivo para 0 bytes, corrompendo saves em desligamentos acidentais e levando ~400 ms. A escrita in-place grava apenas 128 bytes em ~5 ms sem risco de perda. |
| **Versionamento de Setores Modificados** | Ausente ou buffer dinâmico no heap | **Matriz Estática `sector_version[1024]` em BSS** | Rastreamento atômico setor a setor. Se o console salvar repetidamente enquanto o SD grava em segundo plano, as versões não são perdidas e não há fragmentação de memória heap. |
| **Troca Automática de Saves por Jogo (GameID)** | Inexistente (troca exclusivamente manual via web) | **Protocolo MemCard PRO / PicoStation (`0x21`)** | Detecta transmissões de ID de jogos emitidas por xStation, Unirom e Tonyhax, comutando automaticamente para `<GameID>-1.mc` com barreira multicore (`__sync_synchronize`). |
| **Jogos Multi-Disco** | Criação de cartões vazios independentes por disco | **Resolução Inteligente via `multidisc.txt`** | Mapeamento de 471 títulos (FF7, MGS, RE2, etc.) redirecionando discos 2, 3 e 4 para o save pai do Disco 1, mantendo o progresso unificado. |
| **Criação de Cartões Novos Formatados** | Preenchimento com bytes nulos ou `0xFF` | **Template Autêntico Sony de Fábrica (sd2psX)** | Bloco 0 gerado a partir do padrão autêntico Sony com blocos 1–15 em `0xFF`. O PS1 e a BIOS reconhecem o cartão imediatamente como formatado sem exigir formatação manual. |
| **Catálogo de Jogos da Interface Web** | Títulos hardcoded em C/HTML ou ausentes | **Banco `titles.db` + Cache em `localStorage`** | 23.465 títulos catalogados no SD com sincronização delta via hash atômico (`titles_version`), renderização em <5 ms e zero títulos comerciais fixos no firmware. |
| **Consumo de Memória DRAM do ESP32** | Padrão do SDK (~10-15 KB livres, risco de OOM) | **Ajuste Fino de Buffers Wi-Fi/LWIP (~45 KB livres)** | Buffers estáticos/dinâmicos de rede reduzidos mantendo vazão máxima, eliminando travamentos (*OOM panic*) durante salvamentos pesados simultâneos ao acesso web. |
| **Segurança na Atualização Web OTA** | Gravação direta sem validação de cabeçalho | **Validação Prévia do Descritor de Partição** | Apenas imagens com cabeçalho ESP-IDF válido para o projeto `openmc` são gravadas na partição flash de boot, prevenindo "bricking" por uploads acidentais. |
| **Fluxo de Leitura SIO sem Perda de ACK** | Abortos bruscos em setores além de 1024 | **Manutenção Rigorosa de ACK e Tolerância de Limite** | Evita desconexão de barramento se o software da Sony sondar setores superiores durante a rotina de verificação de formatação. |

---

### 2.1. Topologia de Linha: Push-Pull Ativo com Alta Impedância Dinâmica vs Open-Drain Puro
* **O Problema de Hardware no PS1:** No PlayStation 1, a linha `DATA` (MISO) é compartilhada com o Slot 2 e os controles, possuindo um resistor de pull-up passivo fraco de aproximadamente 10 kΩ na placa-mãe. Quando configurado em modo Open-Drain puro (`GPIO_MODE_INPUT_OUTPUT_OD`), o ESP32 atua apenas aterrando a linha para o nível lógico `0`; para o nível lógico `1`, o microcontrolador fica em alta impedância e depende exclusivamente do resistor de pull-up do console para elevar a tensão a 3.3V. Devido à capacitância combinada dos cabos, conectores e pinos do ESP32, a constante de tempo RC de subida fica muito lenta.
* **O Impacto no Crash Bandicoot e Jogos Exigentes:** Jogos que utilizam a biblioteca oficial da Sony em velocidade máxima (250 kHz contínuos) realizam a leitura de cada bit de dados em instantes precisos. Com Open-Drain, os bits `1` não atingiam a tensão limiar antes do console amostrar o sinal, sendo interpretados como `0`. Isso corrompia o *checksum* do setor 0 do diretório, fazendo o jogo exibir *"Checking Format"* seguido de *"Card is not Formatted"*.
* **A Solução Adotada:** Empregamos acionamento ativo *Push-Pull* (3.3V com tempos de subida de nanossegundos) através da chamada `ActiveMode()` enquanto o pino `SEL` estiver em nível baixo, garantindo pulsos digitais perfeitamente quadrados. No instante em que o periférico é desmarcado, a chamada `PassiveMode()` desabilita imediatamente o driver de saída no registrador `GPIO_ENABLE1_W1TC_REG`, colocando as linhas em alta impedância (*Hi-Z*) e eliminando qualquer risco de contenção elétrica no barramento compartilhado.

---

### 2.2. Isolamento de Núcleos e Tratador de Interrupção SIO (Core 1 Baremetal & Core 0 Stall)
* **O Problema de Jitter no ESP32:** O ESP32 possui memória cache compartilhada e barramento de dados interno compartilhado entre os dois núcleos. Tarefas do FreeRTOS executando operações de rede Wi-Fi ou leitura no cartão MicroSD geram acessos constantes à memória flash e ao barramento APB.
* **A Solução Adotada:** O Core 1 foi configurado para executar código baremetal diretamente a partir da IRAM. No tratador de interrupção em nível de registrador da linha `SEL`, o Core 0 é pausado via hardware através da rotina `core0_stall_start()`. Essa pausa é mantida durante todo o tráfego SPI e estendida até que a linha `SEL` retorne comprovadamente para nível alto (`ConfirmUnselected()`) e os registradores de interrupção APB sejam esvaziados. Somente então o Core 0 é retomado (`core0_stall_end()`), impedindo colisões de temporização e perda de pulsos de ACK.

---

### 2.3. Persistência In-Place por Setor vs Sobrescrita Truncada de Arquivo Completo
* **O Perigo do Projeto Original:** O OpenMC tradicional tratava o salvamento abrindo o arquivo `.mc` de 128 KB com a flag `"wb"`. No sistema de arquivos FAT32, `"wb"` trunca o tamanho do arquivo para 0 bytes antes de começar a gravar os dados. Caso o usuário desligasse o console ou ocorresse queda de energia nos 300 ms a 500 ms seguintes, o save era destruído de forma irrecuperável.
* **A Solução Adotada:** O OpenWirelessMC opera exclusivamente com abertura em modo de atualização binária (`"r+b"`). Quando o console grava um setor de 128 bytes, o ponteiro de arquivo é posicionado cirurgicamente com `fseek(f, sector * 128, SEEK_SET)` e apenas os 128 bytes modificados são transferidos para o MicroSD. Isso reduz o tempo de persistência para ~5 ms, diminui o desgaste da mídia flash e torna os saves imunes a perdas catastróficas.

---

### 2.4. Rastreamento Atômico de Versão de Setores (`sector_version[1024]`)
* **Prevenção de Perda por Gravação Concorrente:** Jogos como *Gran Turismo* ou *Tomb Raider* realizam múltiplos acessos de gravação em setores sucessivos em curtos intervalos de tempo. O OpenWirelessMC implementa uma matriz estática `uint16_t sector_version[1024]` alocada na seção BSS da memória. Toda vez que o Core 1 processa uma gravação na DRAM, o contador do respectivo setor é incrementado. A tarefa de persistência no Core 0 compara essa versão com `saved_version[1024]`. Se o console realizar uma nova gravação no mesmo setor enquanto a tarefa de armazenamento ainda está persistindo no disco, o setor permanece marcado como pendente e é regravado na iteração seguinte, sem perdas.

---

### 2.5. Detecção e Comutação Automática de GameID com Barreira Multicore
* **Automação de Cartões por Título:** O OpenWirelessMC suporta o protocolo de transmissão serial de GameID criado pela comunidade (MemCard PRO / PS1Digital / PicoStation no comando `0x21`).
* **Sincronização Segura:** O Core 1 intercepta os caracteres do GameID (ex.: `SCUS-94900` para *Crash Bandicoot*), insere a terminação nula e executa uma barreira de memória de hardware (`__sync_synchronize()`). O Core 0 recebe o evento, consulta as tabelas de mapeamento e comuta o arquivo montado automaticamente para `<GameID>-1.mc`. Se o arquivo ainda não existir, o sistema o gera em tempo real a partir do template pré-formatado da Sony.

---

### 2.6. Mapeamento Inteligente de Jogos Multi-Disco (`multidisc.txt`)
* **Continuidade Transparente:** Em jogos clássicos divididos em múltiplos CDs (como *Final Fantasy VIII*, *Metal Gear Solid*, *Resident Evil 2*, *Chrono Cross*), cada disco possui um GameID diferente. Sem mapeamento, o jogador ao inserir o Disco 2 encontrava um cartão vazio e não conseguia carregar o save do Disco 1.
* **A Solução Adotada:** Um parser rápido no Core 0 lê o arquivo `/sdcard/multidisc.txt` (contendo 471 associações ativas). Ao detectar o ID do Disco 2 (ex.: `SCUS-94164`), o sistema resolve o ID pai do Disco 1 (`SCUS-94163`) e mantém o mesmo cartão montado, permitindo troca de discos transparente e salvamento contínuo.

---

### 2.7. Template Autêntico de Fábrica Sony (sd2psX)
* **Formatação Instantânea:** O Bloco 0 de um Memory Card do PS1 contém a tabela de alocação de blocos e assinaturas de diretório (`"MC"`, checksums e flags de estado). Gerar arquivos com bytes nulos obriga o jogador a abrir o BIOS do PlayStation para formatar o cartão.
* **A Solução Adotada:** Incorporamos o template binário oficial de fábrica da Sony (derivado do projeto sd2psX). Todo novo cartão criado pelo OpenWirelessMC já nasce pré-formatado com a estrutura correta, sendo reconhecido no primeiro milissegundo pelo console sem qualquer solicitação de formatação.

---

### 2.8. Catálogo Global com Cache Versionado no Navegador e Zero Hardcode
* **Performance e Escalabilidade:** Nenhum nome de jogo comercial está embutido no código C++ ou nos arquivos HTML. Uma base completa de 23.465 títulos reside no arquivo `titles.db` no MicroSD.
* **Cache Inteligente:** O navegador armazena os dados em `localStorage`. Ao carregar a interface web, a tabela de jogos é exibida em menos de 5 ms. O endpoint `/api/status` envia o hash atômico `titles_version` (`tamanho ^ data_modificação`). Se o arquivo físico no SD for atualizado pelo usuário, o navegador detecta a divergência e faz o download delta em segundo plano.

---

### 2.9. Otimização Crítica de Memória DRAM do ESP32 (~45 KB livres)
* **Estabilidade Contra OOM Panic:** No ESP-IDF original, os buffers padrão de recepção/transmissão Wi-Fi e os buffers de janela TCP da pilha LWIP consumiam praticamente toda a memória interna livre. Durante sessões em que o console gravava saves enquanto o usuário navegava na página web, o heap caía abaixo de 8 KB, disparando reinicializações por falta de memória (*Out of Memory Panic*).
* **Ajuste Fino:** Reconfiguramos os descritores de rede (`STATIC_RX_BUFFER_NUM=6`, `DYNAMIC_RX_BUFFER_NUM=16`, `DYNAMIC_TX_BUFFER_NUM=16`, janela RX=4 e buffers TCP em `4320` bytes). Isso estabilizou a DRAM livre em confortáveis **~45 KB** (com marca d'água mínima acima de 42 KB), garantindo imunidade absoluta contra reinicializações inesperadas.

---

### 2.10. Validação Criptográfica e de Cabeçalho no Web OTA
* **Proteção Contra Imagens Inválidas:** O manipulador da rota `POST /api/ota` não aceita gravações cegas. Antes de comutar a partição de boot ativa via `esp_ota_set_boot_partition()`, o firmware lê o descritor da partição recém-gravada (`esp_ota_get_partition_description()`) e valida a estrutura de cabeçalho do ESP-IDF, o nome do projeto (`openmc`) e a integridade da imagem, eliminando o risco de corrupção ou travamento do dispositivo.

---

## 3. Compatibilidade de Hardware e Plataforma

* **PlayStation 1 (PS1 / PSOne):** Compatibilidade total com todos os modelos e revisões de placa-mãe.
* **PlayStation 2 (PS2):** Compatível **exclusivamente com jogos de PlayStation 1** executados no PS2 (o console reduz a frequência do barramento para o padrão do PS1 de ~250 kHz).
  > **Nota técnica sobre jogos nativos de PS2:** O OpenWirelessMC **não** suporta jogos nativos de PlayStation 2. O barramento de Memory Card nativo do PS2 opera a frequências de até 4 MHz, requer cartões de alta capacidade (mínimo de 8 MB, inviáveis na DRAM do ESP32) e exige autenticação criptográfica proprietária por hardware (**MagicGate**).

---

## 4. Esquema de Ligações (Pinout)

### 4.1. Conexão ESP32 com o Barramento SIO do PlayStation 1

```text
               [ 9   7   6 ]   [ 5   4   3 ]   [ 2   1 ]  (Porta do Console)
               -----------------------------------------
```

| Pino PS1 | Sinal | Descrição | Pino ESP32 | Modo / Observação |
| :---: | :---: | :--- | :---: | :--- |
| **1** | **DATA** | Linha serial de dados (Memory Card -> Console) | **GPIO 32** | Push-Pull Ativo (3.3V) / Hi-Z Dinâmico |
| **2** | **CMND** | Comandos seriais (Console -> Memory Card) | **GPIO 34** | Entrada lógica direta (3.5V) |
| **4** | **GND** | Referência de terra comum | **GND** | Nível 0V de referência |
| **6** | **ATT (SEL)**| Seleção de periférico (ativo em nível baixo) | **GPIO 35** | Entrada lógica direta (3.5V) |
| **7** | **CLK** | Clock de sincronismo serial (~250 kHz) | **GPIO 39** *(SENSOR_VN)* | Entrada lógica direta (3.5V) |
| **8 (MC) / 9 (Pad)** | **ACK** | Pulso de confirmação de recepção de byte | **GPIO 33** | Push-Pull Ativo (3.3V) / Hi-Z Dinâmico |

> **Aviso de segurança:** Não conecte os pinos 3 (7.6V) e 8 (N/C) da ranhura de Memory Card diretamente aos pinos de GPIO do ESP32.

---

### 4.2. Conexão ESP32 com o Módulo MicroSD (SPI)

| Pino MicroSD | Função SPI | Pino ESP32 |
| :---: | :---: | :---: |
| **CS** | Chip Select | **GPIO 5** |
| **SCK** | Serial Clock | **GPIO 18** |
| **MOSI** | Master Out Slave In | **GPIO 23** |
| **MISO** | Master In Slave Out | **GPIO 19** |
| **VCC** | Tensão de alimentação | **3.3V** ou **5V (VIN)** |
| **GND** | Referência de terra | **GND** |

---

## 5. Alimentação e Regulagem de Tensão

### 5.1. Limitações da Linha de 3.5V Original (Pino 5)
A linha de 3.5V da ranhura de Memory Card do PS1 foi dimensionada pela Sony exclusivamente para pequenos chips EEPROM com consumo de 10 mA a 20 mA, sendo protegida por um microfusível de 0.7A (`PS605`). O microcontrolador ESP32 com o rádio Wi-Fi em transmissão atinge picos de consumo entre 300 mA e 450 mA. Alimentar o ESP32 a partir deste pino provoca quedas bruscas de tensão (*brownout*) e risco de queima do fusível da placa-mãe.

### 5.2. Instalação Recomendada: Regulador Step-Down DC-DC nos 7.6V
A porta de controles do PlayStation fornece **7.6V a 8.0V** no **Pino 3**, linha dimensionada para alimentar os motores de vibração de controles DualShock (suporta até 1A contínuo através do fusível de potência `PS603`/`PS604`).

Recomenda-se a utilização de um conversor Step-Down chaveado (Buck Converter DC-DC, ex.: MP1584EN regulado para 5.0V):

```text
       Porta do Controle PS1                                  ESP32 DevKit
   +---------------------------+                         +---------------------+
   | Pino 3 (7.6V Motores)     |------> [ IN+ ]          |                     |
   | Pino 4 (GND Comum)        |---+--> [ IN- ]          |                     |
   +---------------------------+   |                     |                     |
                                   |  REGULADOR STEP-DOWN|                     |
                                   |  (Ajustado em 5.0V) |                     |
                                   |                     |                     |
                                   |    [ OUT+ (5V) ]--->| VIN (ou 5V)         |
                                   +--->[ OUT- (GND) ]-->| GND (Terra Comum)   |
                                                         +---------------------+
```

1. Regule o módulo Step-Down previamente para fornecer **5.0V** estáveis na saída.
2. Conecte a entrada `IN+` do regulador ao **Pino 3** da porta de controle.
3. Conecte o terra comum `IN-` / `OUT-` ao **GND** da placa do console.
4. Conecte a saída `OUT+` (5.0V) ao pino **VIN** do módulo ESP32.

---

## 6. Estrutura do Cartão MicroSD

1. Formate o cartão MicroSD no sistema de arquivos **FAT32**.
2. Copie os arquivos da pasta [`sdcard_assets/`](sdcard_assets/) para a raiz do cartão:
   * `titles.db`: Banco de dados global de identificação de títulos por GameID (23.465 entradas).
   * `multidisc.txt`: Tabela de mapeamento para unificação de jogos multi-disco (471 entradas ativas).
   * `default.mc`: Boot Card formatado no padrão original Sony (128 KB).
   * `config.txt`: Arquivo de parâmetros de inicialização de rede Wi-Fi.

### 6.1. Estrutura do `config.txt`
```ini
# OpenWirelessMC - Parametros de Rede
# Modos validos: client, ap, none
mode=client
ssid=NomeDaSuaRede
password=SuaSenhaWPA2
```

Se o modo configurado for `client` e o roteador estiver inacessível, o sistema ativa automaticamente um Ponto de Acesso temporário de recuperação (**Rescue AP**: `SSID: WirelessMC` / `Senha: SenhaDificil!`) em memória RAM, sem sobrescrever o arquivo de configurações salvo no cartão.

---

## 7. Compilação, Gravação e Atualização

### 7.1. Compilação via PlatformIO
O projeto utiliza o ecossistema **PlatformIO** integrado ao framework ESP-IDF:

```bash
# Compilacao do firmware
pio run

# Gravacao direta via porta serial
pio run --target upload
```

### 7.2. Pacote de Lançamento e Gravação Automatizada (v1.4)
Os binários pré-compilados e scripts de gravação para Windows encontram-se na pasta [`releases/v1.4_package/`](releases/v1.4_package/):

1. Conecte o ESP32 ao computador via cabo USB.
2. Execute o script automatizado:
   ```cmd
   cd releases\v1.4_package
   flash_windows.bat
   ```
3. Informe a porta COM correspondente ao dispositivo. O script executará a gravação completa (`bootloader.bin`, `partitions.bin`, `ota_data_initial.bin` e `firmware_v1.4.bin`).

### 7.3. Gravação Manual com esptool
Caso prefira gravar manualmente via terminal:
```bash
python -m esptool --chip esp32 --port COMx --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xd000 ota_data_initial.bin 0x10000 firmware_v1.4.bin
```

### 7.4. Atualização Remota (Web OTA)
Com o dispositivo conectado à rede local, atualizações podem ser realizadas diretamente através da interface web (`http://wirelessmc.local/`) na seção **Atualização de Firmware (Web OTA)**:
1. Selecione o arquivo `firmware_v1.4.bin` gerado ou obtido em [`releases/`](releases/).
2. Clique em **Atualizar Firmware**.
3. O servidor validará a integridade do cabeçalho da imagem antes de comutar a partição de boot e reiniciará o microcontrolador automaticamente.

---

## 8. Histórico de Versões

Para consultar o histórico detalhado de alterações de cada versão, consulte o arquivo [CHANGELOG.md](CHANGELOG.md).

* **v1.4.0 (Atual):** Realinhamento do layout GameID `0x21` (tick 2 comprimento, tick 3+ caracteres), streaming contínuo no FastRead `0x42` até desasserção de SEL, recuperação automática e validação estrita de integridade para `default.mc` (com regeneração do asset Sony com checksum `0x0E`), liberação de barramento SPI em falhas de montagem e unmount, restauração do flag `0x08` ("diretório não lido") após escrita no setor 0, buffer SSID expandido para 32 caracteres completos, stubs de teste host atualizados e sincronização de XTAL para 40 MHz.
* **v1.3.0:** Restauração do acionamento Push-Pull ativo forte nos pinos MISO (DATA) e ACK com alta impedância dinâmica (*Hi-Z*) em modo passivo (solução definitiva para *Crash Bandicoot* e jogos com leitura rápida de diretório), remoção de cortes de ACK intempestivos em limites de setor, posicionamento correto de `core0_stall_end` pós-desasserção de SEL, documentação comparativa completa contra o OpenMC original e atualização da Web UI.
* **v1.2.0:** Cache versionado dinâmico para `titles.db`, zero títulos hardcoded no HTML, identificação do `default.mc` como `Boot Card`, otimização crítica de DRAM (~45 KB livres), verificação de integridade no Web OTA e suporte estendido a jogos multi-disco (*Policenauts*, *Resident Evil 3*).
* **v1.1.0:** Suporte a multi-disco via `multidisc.txt`, desmontagem/remontagem a quente e estabilização de buffers.
* **v1.0.0:** Lançamento inicial estável com escrita in-place por setor, SIO baremetal no Core 1 calibrado a 84 ciclos, servidor web e partição dual-OTA.

---

## 9. Licença e Reconhecimentos

* **OpenWirelessMC:** Desenvolvido e mantido por **Schickacka**.
* **Engenharia Reversa OpenMC:** Baseado no trabalho original pioneiro de John Baumann, rama, sickle, Nicolas Noble e danhans42.
* **Ecossistema MMCE / sd2psXtd:** Referências de boas práticas e padronização baseadas no firmware [sd2psXtd](https://github.com/sd2psXtd/firmware).
* **Especificações do Protocolo SIO:** Documentação de temporização e comportamento de registradores por nocash ([psx-spx](https://problemkaputt.de/psx-spx.htm)).
* **Catalogação de Jogos:** Base de dados derivada das coleções comunitárias do DuckStation e PCSX2. Todos os direitos e marcas comerciais são de propriedade de suas respectivas publicadoras.
