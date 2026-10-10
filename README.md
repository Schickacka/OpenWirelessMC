# 🎮 OpenWirelessMC — PlayStation Wireless Memory Card Emulator

<p align="center">
  <b>Emulador de Memory Card sem fio para PlayStation 1 & PlayStation 2 com armazenamento MicroSD, interface Web retrô estilo PS1, banco de dados global de títulos e inicialização ultrarrápida no ESP32.</b>
</p>

<p align="center">
  <b>Criador & Mantenedor:</b> Schickacka<br>
  <i>Baseado na engenharia reversa pioneira do projeto <a href="https://github.com/johnbaumann/OpenMC">OpenMC</a> (John Baumann / rama / sickle / Nicolas Noble / danhans42).</i>
</p>

---

## ⚡ Recursos do Projeto

- 🚀 **Boot Ultrarrápido do SIO (~1.75s):**  
  O Core 1 baremetal inicializa o barramento SPI/SIO imediatamente na inicialização, ficando 100% pronto e operacional **mais de 3,5 segundos antes** da animação da BIOS da Sony terminar no PlayStation 1.
- 📶 **Gestão 100% Sem Fio (Web Manager Retrô):**  
  Gerencie todos os seus saves pelo celular ou PC conectando diretamente no IP do ESP32 ou via mDNS:
  ```text
  http://wirelessmc.local/
  ```
- 📚 **Banco de Dados Integrado com 23.464 Títulos:**  
  Identificação automática em tempo real do nome oficial e região (`US 🇺🇸`, `JP 🇯🇵`, `EU 🇪🇺`, `ASIA 🌏`) de jogos de **PlayStation 1 (10.638 jogos)** e **PlayStation 2 (12.827 jogos)** a partir do GameID no nome do arquivo (ex: `SCUS-94183.mc` ➔ *Crash Bandicoot: WARPED*).
- 💾 **Desmontagem Segura & Remontagem a Quente (Hot Remount):**  
  Possibilidade de desmontar o MicroSD com segurança pela interface web, trocar de cartão físico e clicar em **"🔄 Montar Cartão SD"** para remontar na hora sem precisar desligar o videogame ou reiniciar o sistema!
- 🛡️ **Modo de Resgate Automático (Rescue AP):**  
  Se a rede Wi-Fi configurada não for encontrada (ex: ao levar o console para outro local), o sistema entra automaticamente em Ponto de Acesso temporário (`SSID: WirelessMC` / `Senha: SenhaDificil!`) em RAM sem apagar suas configurações salvas no `config.txt`.
- 🕹️ **Gestão Completa de Saves:**  
  - Baixar saves diretamente para o PC em formato padrão (`.mc`, `.mcd`, `.mcr`) compatível com emuladores (DuckStation, PCSX2, MemcardRex).
  - Enviar saves existentes para o MicroSD.
  - Criar novos Memory Cards virgens de 128 KB já formatados no padrão do console.
  - Renomear arquivos que não estejam montados.
  - Desmontar slot virtual (deixar ranhura vazia — perfeito para o easter egg do Psycho Mantis em *Metal Gear Solid*!).
- ⚡ **Monitor de Hardware Dinâmico (OWMC Hardware):**  
  Monitoramento em tempo real do silício (`ESP32-D0WD-V3`), frequência do clock (`160 MHz`), tamanho da Flash SPI externa (`4 MB`), Memória RAM DRAM Total e Livre (`(Min: ...)`), e tempo ativo (*uptime*).

---

## 🛠️ Esquema de Ligação (Hardware)

### 1. ESP32 (38 Pinos) ➔ Porta do Memory Card do PS1

```text
              [ 9   7   6 ]   [ 5   4   3 ]   [ 2   1 ]  (Porta do Console)
              -----------------------------------------
```

| Pino PS1 | Sinal | Função | Conexão no ESP32 | Nível Lógico |
| :---: | :---: | :--- | :---: | :---: |
| **1** | **DATA** | Dados saindo do Memory Card para o console | **GPIO 32** | Open-Drain (Pull-up 3.5V no console) |
| **2** | **CMND** | Comandos enviados do console para o Memory Card | **GPIO 34** | Entrada direta (3.5V) |
| **4** | **GND** | Referência comum de terra | **GND** | 0V |
| **6** | **ATT (SEL)** | Seleção do Memory Card (ativo em nível baixo) | **GPIO 35** | Entrada direta (3.5V) |
| **7** | **CLK** | Clock de sincronismo serial (~250 kHz) | **GPIO 39** *(SENSOR_VN)* | Entrada direta (3.5V) |
| **9** | **ACK** | Pulso de confirmação de recepção de byte | **GPIO 33** | Open-Drain (Pull-up 3.5V no console) |

> ⚠️ **Atenção:** Os pinos 3 (7.6V) e 8 (N/C) da porta do Memory Card não devem ser ligados diretamente aos pinos lógicos de I/O do ESP32.

---

### 2. Módulo Leitor MicroSD (SPI) ➔ ESP32

| Pino Módulo MicroSD | Função SPI | Conexão no ESP32 |
| :---: | :---: | :---: |
| **CS** | Chip Select | **GPIO 5** |
| **SCK / CLK** | Clock SPI | **GPIO 18** |
| **MOSI / DI** | Master Out, Slave In | **GPIO 23** |
| **MISO / DO** | Master In, Slave Out | **GPIO 19** |
| **VCC** | Alimentação | **3.3V** ou **5V (VIN)** |
| **GND** | Terra | **GND** |

---

## 🔌 Cuidados com a Alimentação (Como Funciona na Prática)

1. **Pino 5 da porta do Memory Card (3.5V):**
   - No PS1, a linha original de 3.5V da porta do Memory Card foi dimensionada pela Sony para alimentar apenas chips EEPROM minúsculos (~10mA a 30mA), passando por um fusível SMD (`PS605` / ~0.7A) e pequenos indutores de filtragem.
   - O ESP32 com Wi-Fi ativo consome picos de **250mA a 400mA** durante as transmissões de rádio de 2.4 GHz.
2. **Recomendação de Instalação Interna:**
   - Para instalação interna no videogame, alimente o pino `VIN` (5V) do ESP32 puxando a linha de **5V** da placa-mãe ou da fonte interna do console, mantendo o terra (**GND**) comum com o PS1.
3. **Instalação Externa (Sem abrir o console):**
   - É possível puxar a alimentação a partir do pino 3 da porta do controle (linha de vibração de 7.6V–9V / DualShock) utilizando um regulador step-down miniatura (como o MP1584 ou regulador LDO 3.3V), ou alimentar o módulo via porta Micro-USB/USB-C com carregador comum.

---

## 💾 Preparação do Cartão MicroSD

1. Formate o cartão MicroSD em **FAT32** (tamanho de alocação padrão).
2. Copie os arquivos da pasta [`sdcard_assets/`](sdcard_assets/) para a raiz do seu MicroSD:
   - `titles.db`: Banco de dados global com 23.464 títulos de PS1 e PS2.
   - `default.mc`: Memory Card virgem inicial montado automaticamente.
   - `config.txt`: Configurações de rede Wi-Fi.

### Exemplo de `config.txt`:
```ini
# OpenWirelessMC Configuration
# mode=client para conectar ao roteador de casa, ou mode=ap para rede própria
mode=client
ssid=SuaRedeWiFi
password=SuaSenhaWiFi
```

---

## 🚀 Compilação e Gravação

O projeto está configurado para compilar nativamente via **PlatformIO** (VS Code / CLI):

```bash
# Compilar o firmware
pio run

# Gravar no ESP32 conectado na porta COM
pio run --target upload
```

Os binários compilados prontos para gravação direta via `esptool` encontram-se na pasta [`releases/`](releases/).

Para gravar diretamente o binário final compilado:
```bash
python -m esptool --chip esp32 --port COM6 --baud 115200 write_flash 0x10000 releases/firmware_v10_openwirelessmc_production_final.bin
```

---

## 📜 Licença e Créditos

- **OpenWirelessMC:** Desenvolvido e mantido por **Schickacka**.
- **Engenharia Reversa OpenMC:** Baseado no código original pioneiro do projeto [OpenMC](https://github.com/johnbaumann/OpenMC) por John Baumann e contribuidores.
- **Banco de Dados de Jogos:** Catalogação extraída dos bancos de dados do DuckStation e PCSX2. Todos os direitos dos jogos pertencem às suas respectivas publicadoras.
