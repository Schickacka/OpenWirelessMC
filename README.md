# OpenWirelessMC

Emulador de Memory Card sem fio para PlayStation 1 baseado no microcontrolador ESP32, com armazenamento em cartão MicroSD via barramento SPI, servidor web embarcado para gerenciamento remoto de saves, banco de dados global de títulos com cache dinâmico versionado e atualização de firmware over-the-air (Web OTA).

Desenvolvido por **Schickacka**, baseado na engenharia reversa do projeto [OpenMC](https://github.com/johnbaumann/OpenMC).

---

## 1. Visão Geral da Arquitetura

O OpenWirelessMC implementa a emulação completa do protocolo serial SIO do PlayStation 1 em tempo real, dividindo as operações de barramento e gerenciamento entre os dois núcleos do ESP32:

* **Núcleo 1 (Baremetal / SIO dedicado):**
  * Executa a máquina de estados do barramento SPI/SIO da Sony diretamente a partir da memória IRAM.
  * Temporização de resposta ACK calibrada para 84 ciclos (~350 ns) no Core 1 a 240 MHz, garantindo compatibilidade total com o BIOS do console e títulos com rotinas estritas de detecção de barramento (*Crash Bandicoot*, *Gran Turismo 1 & 2*, *Chrono Cross*, *Driver 2*).
  * Topologia de barramento **100% Open-Drain** nas linhas `DATA` (GPIO 32) e `ACK` (GPIO 33), operando em dreno aberto (`GPIO_MODE_INPUT_OUTPUT_OD`) e respeitando os resistores de pull-up da placa-mãe do console. Previne conflitos de sinal com controles, multitaps e periféricos conectados ao Slot 2.
  * Imagem ativa de 128 KB mantida em DRAM interna (`memory_card_ram`), permitindo leitura e gravação com latência zero durante as transações do barramento do console.
  * Inicialização ultrarrápida do SIO (~1.75 s), ficando operacional aproximadamente 3.5 segundos antes do término da animação de boot da BIOS no PS1.

* **Núcleo 0 (FreeRTOS / Armazenamento e Rede):**
  * **Mecanismo de Escrita In-Place por Setor:** Gravação de alterações no MicroSD em modo `r+b` com deslocamento de ponteiro (`fseek`). Apenas os setores efetivamente modificados (128 bytes por setor) são gravados na mídia física, reduzindo expressivamente o desgaste das células flash do cartão SD e eliminando o risco de corrupção total da imagem em caso de desligamento abrupto do console.
  * **Controle Atômico de Versão de Setores:** Matriz estática de rastreamento (`sector_version[1024]`) alocada em memória BSS, prevenindo perda de dados quando o console realiza gravações sucessivas e evitando fragmentação do heap.
  * **Exclusão Mútua (`StorageLock`):** Mutex recursivo que sincroniza com segurança os acessos simultâneos entre o servidor HTTP, a rotina de detecção automática de GameID e a tarefa de persistência de disco.
  * **Suporte Inteligente a Jogos Multi-Disco (Multi-Disc Parent Mapping):** Mapeamento automático de 475 títulos com múltiplos discos através do arquivo `/sdcard/multidisc.txt`. Ao trocar para discos subsequentes (ex.: Disco 2 de *Final Fantasy*, *Metal Gear Solid*, *Resident Evil 2*, *Policenauts*), o sistema redireciona o arquivo montado para o cartão principal do Disco 1, preservando o progresso contínuo sem gerar cartões vazios indesejados.
  * **Catálogo Global com Cache Versionado Dinâmico:** Identificação de 23.465 títulos catalogados no MicroSD em `titles.db`. O navegador cliente realiza o armazenamento local via `localStorage`, renderizando a biblioteca em menos de 5 ms sem tráfego de rede. O endpoint `/api/status` valida a versão do banco através do hash `size ^ mtime` do arquivo no SD (`titles_version`), atualizando o cache automaticamente em caso de modificação na mídia. Zero jogos comerciais hardcoded nos arquivos de firmware ou HTML.
  * **Cartão Inicial Padronizado:** O arquivo base `default.mc` é identificado e exibido oficialmente como **"Boot Card"** na interface web.
  * **Otimização Crítica de Memória DRAM:** Dimensionamento dinâmico dos buffers de transmissão/recepção Wi-Fi e pilha TCP/IP LWIP, preservando ~45 KB de DRAM interna livre para prevenir qualquer risco de esgotamento de memória (*OOM panic*) durante acessos simultâneos de rede e disco.
  * **Servidor Web Embarcado:** Interface acessível via navegador em `http://wirelessmc.local/` (ou pelo endereço IP do dispositivo), com suporte a upload, download, renomeação, criação de novos cartões formatados, desmontagem/remontagem a quente (*hot remount*) e atualização de firmware over-the-air (**Web OTA**) com verificação de cabeçalho de integridade.

---

## 2. Compatibilidade de Hardware e Plataforma

* **PlayStation 1 (PS1 / PSOne):** Compatibilidade total com todos os modelos e revisões de placa-mãe.
* **PlayStation 2 (PS2):** Compatível **exclusivamente com jogos de PlayStation 1** executados no PS2 (o console reduz a frequência do barramento para o padrão do PS1 de ~250 kHz).
  > **Nota técnica sobre jogos nativos de PS2:** O OpenWirelessMC **não** suporta jogos nativos de PlayStation 2. O barramento de Memory Card nativo do PS2 opera a frequências de até 4 MHz, requer cartões de alta capacidade (mínimo de 8 MB, inviáveis na DRAM do ESP32) e exige autenticação criptográfica proprietária por hardware (**MagicGate**).

---

## 3. Esquema de Ligações (Pinout)

### 3.1. Conexão ESP32 com o Barramento SIO do PlayStation 1

```text
               [ 9   7   6 ]   [ 5   4   3 ]   [ 2   1 ]  (Porta do Console)
               -----------------------------------------
```

| Pino PS1 | Sinal | Descrição | Pino ESP32 | Modo / Observação |
| :---: | :---: | :--- | :---: | :--- |
| **1** | **DATA** | Linha serial de dados (Memory Card -> Console) | **GPIO 32** | Open-Drain (`GPIO_MODE_INPUT_OUTPUT_OD`) |
| **2** | **CMND** | Comandos seriais (Console -> Memory Card) | **GPIO 34** | Entrada lógica direta (3.5V) |
| **4** | **GND** | Referência de terra comum | **GND** | Nível 0V de referência |
| **6** | **ATT (SEL)**| Seleção de periférico (ativo em nível baixo) | **GPIO 35** | Entrada lógica direta (3.5V) |
| **7** | **CLK** | Clock de sincronismo serial (~250 kHz) | **GPIO 39** *(SENSOR_VN)* | Entrada lógica direta (3.5V) |
| **8 (MC) / 9 (Pad)** | **ACK** | Pulso de confirmação de recepção de byte | **GPIO 33** | Open-Drain (`GPIO_MODE_INPUT_OUTPUT_OD`) |

> **Aviso de segurança:** Não conecte os pinos 3 (7.6V) e 8 (N/C) da ranhura de Memory Card diretamente aos pinos de GPIO do ESP32.

---

### 3.2. Conexão ESP32 com o Módulo MicroSD (SPI)

| Pino MicroSD | Função SPI | Pino ESP32 |
| :---: | :---: | :---: |
| **CS** | Chip Select | **GPIO 5** |
| **SCK** | Serial Clock | **GPIO 18** |
| **MOSI** | Master Out Slave In | **GPIO 23** |
| **MISO** | Master In Slave Out | **GPIO 19** |
| **VCC** | Tensão de alimentação | **3.3V** ou **5V (VIN)** |
| **GND** | Referência de terra | **GND** |

---

## 4. Alimentação e Regulagem de Tensão

### 4.1. Limitações da Linha de 3.5V Original (Pino 5)
A linha de 3.5V da ranhura de Memory Card do PS1 foi dimensionada pela Sony exclusivamente para pequenos chips EEPROM com consumo de 10 mA a 20 mA, sendo protegida por um microfusível de 0.7A (`PS605`). O microcontrolador ESP32 com o rádio Wi-Fi em transmissão atinge picos de consumo entre 300 mA e 450 mA. Alimentar o ESP32 a partir deste pino provoca quedas bruscas de tensão (*brownout*) e risco de queima do fusível da placa-mãe.

### 4.2. Instalação Recomendada: Regulador Step-Down DC-DC nos 7.6V
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

## 5. Estrutura do Cartão MicroSD

1. Formate o cartão MicroSD no sistema de arquivos **FAT32**.
2. Copie os arquivos da pasta [`sdcard_assets/`](sdcard_assets/) para a raiz do cartão:
   * `titles.db`: Banco de dados global de identificação de títulos por GameID (23.465 entradas).
   * `multidisc.txt`: Tabela de mapeamento para unificação de jogos multi-disco (475 entradas).
   * `default.mc`: Boot Card formatado no padrão original Sony (128 KB).
   * `config.txt`: Arquivo de parâmetros de inicialização de rede Wi-Fi.

### 5.1. Estrutura do `config.txt`
```ini
# OpenWirelessMC - Parametros de Rede
# Modos validos: client, ap, none
mode=client
ssid=NomeDaSuaRede
password=SuaSenhaWPA2
```

Se o modo configurado for `client` e o roteador estiver inacessível, o sistema ativa automaticamente um Ponto de Acesso temporário de recuperação (**Rescue AP**: `SSID: WirelessMC` / `Senha: SenhaDificil!`) em memória RAM, sem sobrescrever o arquivo de configurações salvo no cartão.

---

## 6. Compilação, Gravação e Atualização

### 6.1. Compilação via PlatformIO
O projeto utiliza o ecossistema **PlatformIO** integrado ao framework ESP-IDF:

```bash
# Compilacao do firmware
pio run

# Gravacao direta via porta serial
pio run --target upload
```

### 6.2. Pacote de Lançamento e Gravação Automatizada (v1.2)
Os binários pré-compilados e scripts de gravação para Windows encontram-se na pasta [`releases/v1.2_package/`](releases/v1.2_package/):

1. Conecte o ESP32 ao computador via cabo USB.
2. Execute o script automatizado:
   ```cmd
   cd releases\v1.2_package
   flash_windows.bat
   ```
3. Informe a porta COM correspondente ao dispositivo. O script executará a gravação completa (`bootloader.bin`, `partitions.bin`, `ota_data_initial.bin` e `firmware_v1.2.bin`).

### 6.3. Gravação Manual com esptool
Caso prefira gravar manualmente via terminal:
```bash
python -m esptool --chip esp32 --port COMx --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xd000 ota_data_initial.bin 0x10000 firmware_v1.2.bin
```

### 6.4. Atualização Remota (Web OTA)
Com o dispositivo conectado à rede local, atualizações podem ser realizadas diretamente através da interface web (`http://wirelessmc.local/`) na seção **Atualização de Firmware (Web OTA)**:
1. Selecione o arquivo `firmware_v1.2.bin` gerado ou obtido em [`releases/`](releases/).
2. Clique em **Atualizar Firmware**.
3. O servidor validará a integridade do cabeçalho da imagem antes de comutar a partição de boot e reiniciará o microcontrolador automaticamente.

---

## 7. Histórico de Versões

Para consultar o histórico detalhado de alterações de cada versão, consulte o arquivo [CHANGELOG.md](CHANGELOG.md).

* **v1.2.0 (Atual):** Cache versionado dinâmico para `titles.db`, zero títulos hardcoded no HTML, identificação do `default.mc` como `Boot Card`, otimização crítica de DRAM (~45 KB livres), verificação de integridade no Web OTA e suporte estendido a jogos multi-disco (*Policenauts*, *Resident Evil 3*).
* **v1.1.0:** Suporte a multi-disco via `multidisc.txt`, topologia 100% open-drain para DATA e ACK, desmontagem/remontagem a quente e estabilização de buffers.
* **v1.0.0:** Lançamento inicial estável com escrita in-place por setor, SIO baremetal no Core 1 calibrado a 84 ciclos, servidor web e partição dual-OTA.

---

## 8. Licença e Reconhecimentos

* **OpenWirelessMC:** Desenvolvido e mantido por **Schickacka**.
* **Engenharia Reversa OpenMC:** Baseado no trabalho original pioneiro de John Baumann, rama, sickle, Nicolas Noble e danhans42.
* **Ecossistema MMCE / sd2psXtd:** Referências de boas práticas e padronização baseadas no firmware [sd2psXtd](https://github.com/sd2psXtd/firmware).
* **Especificações do Protocolo SIO:** Documentação de temporização e comportamento de registradores por nocash ([psx-spx](https://problemkaputt.de/psx-spx.htm)).
* **Catalogação de Jogos:** Base de dados derivada das coleções comunitárias do DuckStation e PCSX2. Todos os direitos e marcas comerciais são de propriedade de suas respectivas publicadoras.
