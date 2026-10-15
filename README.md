# OpenWirelessMC

Emulador de Memory Card sem fio para PlayStation 1 baseado no microcontrolador ESP32, com armazenamento em cartão MicroSD via barramento SPI, servidor web embarcado para gerenciamento remoto de saves e atualização de firmware over-the-air (Web OTA).

Desenvolvido por **Schickacka**, baseado na engenharia reversa do projeto [OpenMC](https://github.com/johnbaumann/OpenMC).

---

## 1. Visão Geral da Arquitetura

O OpenWirelessMC implementa a emulação completa do protocolo serial SIO do PlayStation 1 em tempo real, dividindo as operações de barramento e gerenciamento entre os dois núcleos do ESP32:

* **Núcleo 1 (Baremetal / SIO dedicado):**
  * Executa a máquina de estados do barramento SPI/SIO da Sony diretamente a partir da memória IRAM.
  * Temporização de resposta ACK calibrada para 84 ciclos (~350 ns) no Core 1 a 240 MHz, garantindo compatibilidade total com o BIOS e jogos com rotinas de leitura estritas (como *Crash Bandicoot*, *Gran Turismo 1 & 2*, *Chrono Cross* e *Driver 2*).
  * Imagem de 128 KB mantida em DRAM interna (`memory_card_ram`), permitindo leitura e gravação com latência zero durante as transações do barramento do console.

* **Núcleo 0 (FreeRTOS / Armazenamento e Rede):**
  * **Mecanismo de Escrita In-Place por Setor:** Gravação de alterações no MicroSD utilizando abertura em modo `r+b` com deslocamento de ponteiro (`fseek`). Apenas os setores efetivamente modificados (128 bytes por setor) são gravados na mídia física, reduzindo o desgaste do cartão SD e eliminando o risco de corrupção total da imagem em caso de desligamento abrupto do console.
  * **Controle de Versão de Setores:** Rastreamento atômico de modificações por setor (`sector_version`) para evitar perda de dados quando o console realiza gravações sucessivas durante uma operação de gravação em andamento.
  * **Exclusão Mútua (`StorageLock`):** Mutex recursivo que sincroniza acessos simultâneos entre o servidor HTTP, a rotina de detecção automática de GameID e a tarefa de persistência de disco.
  * **Servidor Web Embarcado:** Interface de gerenciamento acessível via navegador em `http://wirelessmc.local/` (ou pelo endereço IP do dispositivo), com suporte a upload, download, renomeação, criação de cartões formatados e atualização de firmware (Web OTA).

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
| **1** | **DATA** | Linha serial de dados (Memory Card -> Console) | **GPIO 32** | Open-Drain (Pull-up interno no console) |
| **2** | **CMND** | Comandos seriais (Console -> Memory Card) | **GPIO 34** | Entrada lógica direta (3.5V) |
| **4** | **GND** | Referência de terra comum | **GND** | Nível 0V de referência |
| **6** | **ATT (SEL)**| Seleção de periférico (ativo em nível baixo) | **GPIO 35** | Entrada lógica direta (3.5V) |
| **7** | **CLK** | Clock de sincronismo serial (~250 kHz) | **GPIO 39** *(SENSOR_VN)* | Entrada lógica direta (3.5V) |
| **9** | **ACK** | Pulso de confirmação de recepção de byte | **GPIO 33** | Drive push-pull com saída ativa |

> **Aviso de segurança:** Não conecte os pinos 3 (7.6V) e 8 (N/C) diretamente aos pinos de GPIO do ESP32.

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
A linha de 3.5V da ranhura de Memory Card do PS1 foi dimensionada para alimentar chips EEPROM com consumo da ordem de 10 mA a 20 mA, sendo protegida por um microfusível de 0.7A (`PS605`). O microcontrolador ESP32 com o rádio Wi-Fi em transmissão atinge picos de consumo entre 300 mA e 450 mA. Alimentar o microcontrolador a partir deste pino provoca quedas de tensão (*brownout*) e risco de queima do fusível da placa-mãe.

### 4.2. Instalação Recomendada: Regulador Step-Down DC-DC nos 7.6V
A porta de controles do PlayStation fornece **7.6V a 8.0V** no **Pino 3**, linha dimensionada pela Sony para alimentar os motores de vibração de controles DualShock (suporta até 1A contínuo através do fusível de potência `PS603`/`PS604`).

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
   * `titles.db`: Banco de dados de identificação automática de títulos por GameID.
   * `default.mc`: Imagem inicial padrão de 128 KB (formatação padrão Sony).
   * `config.txt`: Arquivo de parâmetros de inicialização de rede.

### 5.1. Estrutura do `config.txt`
```ini
# OpenWirelessMC - Parametros de Rede
# Modos validos: client, ap, none
mode=client
ssid=NomeDaSuaRede
password=SuaSenhaWPA2
```

Se o modo configurado for `client` e o roteador estiver inacessível, o sistema ativa automaticamente um Ponto de Acesso temporário de recuperação (`SSID: WirelessMC` / `Senha: SenhaDificil!`) em memória RAM, sem sobrescrever o arquivo de configurações.

---

## 6. Compilação e Gravação

O projeto utiliza o ecossistema **PlatformIO** integrado ao framework ESP-IDF:

```bash
# Compilacao do firmware
pio run

# Gravacao direta via porta serial
pio run --target upload
```

### 6.1. Gravação Manual com esptool
Para gravar o binário pré-compilado diretamente via cabo serial:
```bash
python -m esptool --chip esp32 --port COMx --baud 115200 write_flash 0x10000 releases/firmware_v1.0.bin
```

### 6.2. Atualização Remota (Web OTA)
Com o dispositivo conectado à rede local, atualizações subsequentes podem ser realizadas diretamente através da interface web (`http://wirelessmc.local/`) na seção de atualização de firmware, sem necessidade de conexão física USB.

---

## 7. Licença e Reconhecimentos

* **OpenWirelessMC:** Desenvolvido e mantido por **Schickacka**.
* **Engenharia Reversa OpenMC:** Baseado no trabalho original de John Baumann, rama, sickle, Nicolas Noble e danhans42.
* **Identificação de Jogos:** Catalogação derivada das bases comunitárias DuckStation e PCSX2. Todos os direitos sobre nomes e marcas registrados pertencem aos seus respectivos detentores.
