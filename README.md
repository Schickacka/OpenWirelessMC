# 🎮 ESP32 PlayStation 1 Wireless Memory Card & SIO Emulator

Emulador de Memory Card e interface SIO para PlayStation 1 utilizando **ESP32** e **cartão MicroSD**, com suporte a **gerenciamento e transferência de saves via Wi-Fi** por interface Web.

Baseado na arquitetura do projeto [OpenMC](https://github.com/johnbaumann/OpenMC) de John Baumann, adaptado e documentado para módulos de desenvolvimento padrão **ESP32-WROOM-32D (38 pinos)** e leitores MicroSD SPI.

---

## ⚡ Recursos Principais

- **Emulação Completa de Memory Card de PS1:** Salva e carrega arquivos `.mcr` / `.mcd` diretamente do cartão MicroSD (formatado em FAT32).
- **Gerenciamento via Wi-Fi:**
  - Cria um Ponto de Acesso Wi-Fi próprio (ex: `OpenMC` com IP `192.168.4.1`) ou conecta-se à sua rede local via `config.txt`.
  - Interface Web embutida para upload, download, backup e troca rápida de cartões virtuais pelo smartphone ou PC.
- **Arquitetura Dual-Core do ESP32:**
  - **Core 1 (Baremetal / Interrupts de alta velocidade):** Responde ao protocolo SIO/SPI do PlayStation com temporização em nível de microssegundos.
  - **Core 0 (FreeRTOS):** Cuida do sistema de arquivos FAT32 no MicroSD, servidor HTTP e pilha de rede TCP/IP Wi-Fi sem interromper o console.
- **Suporte a Emulação de Controles:** Suporte preliminar à emulação de gamepads pela mesma interface de comunicação SIO.

---

## 🛠️ Esquema de Ligação (Hardware)

### 1. Placa ESP32-WROOM-32D (38 Pinos) ➔ Porta do PlayStation 1

```
              [ 9   7   6 ]   [ 5   4   3 ]   [ 2   1 ]  (Porta do Console)
              -----------------------------------------
```

| Pino PS1 | Sinal | Função | Conexão no ESP32 |
| :---: | :---: | :--- | :---: |
| **1** | **DATA** | Dados saindo do Memory Card para o console | **GPIO 32** |
| **2** | **CMND** | Comandos enviados do console para o Memory Card | **GPIO 34** |
| **4** | **GND** | Referência comum de terra | **GND** |
| **6** | **ATT (SEL)** | Seleção do Memory Card (ativo em nível baixo) | **GPIO 35** |
| **7** | **CLK** | Clock de sincronismo serial (~250 kHz) | **GPIO 39** *(SENSOR_VN)* |
| **9** | **ACK** | Pulso de confirmação de recepção de byte | **GPIO 33** |

> ⚠️ **Atenção:** Os pinos 3 (7.6V) e 8 (N/C) da porta do Memory Card não devem ser conectados ao ESP32.

---

### 2. Módulo Leitor MicroSD (SPI) ➔ ESP32

| Pino Módulo MicroSD | Função | Conexão no ESP32 |
| :---: | :---: | :---: |
| **CS** | Chip Select | **GPIO 5** |
| **SCK / CLK** | Clock SPI | **GPIO 18** |
| **MOSI / DI** | Master Out, Slave In | **GPIO 23** |
| **MISO / DO** | Master In, Slave Out | **GPIO 19** |
| **VCC** | Alimentação | **5V (VIN)** ou **3.3V** |
| **GND** | Terra | **GND** |

---

### ⚠️ Cuidados Vitais com a Alimentação

1. **Nunca alimente o ESP32 pelo pino 3.3V original do Memory Card do PS1.** A trilha do console foi projetada para alimentar memórias EEPROM de baixo consumo (~30mA), enquanto o ESP32 com Wi-Fi ativo consome picos de **300mA a 400mA**, o que pode queimar o fusível SMD de proteção do console (`PS605`).
2. **No protótipo:** Alimente o ESP32 via cabo USB (carregador de celular ou PC), interligando apenas o **GND** do ESP32 com o **GND** do PS1.
3. **Na montagem final:** Puxe a linha de 5V ou 8V da fonte ou placa do console com um regulador DC-DC (step-down) para alimentar o pino VIN do ESP32.

---

## 💾 Preparação do Cartão MicroSD

1. Formate o cartão MicroSD em **FAT32** (tamanho de cluster padrão).
2. Na raiz do cartão, coloque um arquivo vazio ou com um save válido de PS1 nomeado como:
   ```text
   default.mc
   ```
3. *(Opcional)* Crie um arquivo `config.txt` na raiz para configurar o Wi-Fi:
   ```text
   mode=ap
   ssid=PlayStation_MC
   password=12345678
   ```
   *(Ou altere `mode=client`, colocando o SSID e senha da sua rede doméstica para acessar pelo IP atribuído pelo roteador).*

---

## 🌐 Como Acessar a Interface Web

1. Ligue o PlayStation 1 com o adaptador conectado.
2. No seu computador ou celular, conecte na rede Wi-Fi criada pelo ESP32 (ex: `PlayStation_MC`).
3. Abra o navegador e acesse:
   ```text
   http://192.168.4.1
   ```
4. Na página, você verá os arquivos do cartão SD, podendo fazer download dos saves para o PC ou enviar novos cartões virtuais (`.mcr` / `.mcd`) diretamente sem precisar desligar o console!

---

## 🔧 Compilação e Gravação

### Pré-requisitos
- **ESP-IDF v4.3+ / v4.4** instalado.
- Python 3 com `esptool` instalado (`pip install esptool`).

### Compilando via ESP-IDF:
```bash
idf.py set-target esp32
idf.py build
```

### Gravando no ESP32:
```bash
python ../working-dir/flash_firmware.py
```
*Ou diretamente via esptool:*
```bash
esptool.py -p COMx -b 921600 write_flash 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/openmc.bin
```

---

## 🎮 Expansão: Controles

O console utiliza o mesmo barramento serial SIO para comunicação com os controles e com os cartões de memória, diferenciando o dispositivo ativo através das linhas independentes de seleção (**ATT**).
- O arquivo `src/playstation/sio_controller.cc` contém a implementação base para controle digital.
- Para controles modernos sem fio (Bluetooth) como PS4/PS5/Xbox, recomendamos o projeto irmão **[BlueRetro](https://github.com/darthcloud/BlueRetro)** para ESP32.
