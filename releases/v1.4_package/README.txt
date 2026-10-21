OpenWirelessMC v1.4 - Release Package
=====================================
Desenvolvido por Schickacka

Novidades da Versao 1.4:
- Realinhamento de protocolo GameID 0x21 (tick 2 comprimento, tick 3+ caracteres).
- Suporte a streaming continuo no FastRead 0x42 com encadeamento de paginas ate SEL subir.
- Validacao estrita de integridade para default.mc com recuperacao automatica e checksum Sony autentico (0x0E).
- Prevencao de vazamento do barramento SPI em falha de montagem do SD e no unmount.
- Restauracao do flag 0x08 ("diretorio nao lido") apos escrita no setor 0.
- Buffer de SSID Wi-Fi ampliado para 32 caracteres completos sem truncamento.
- Manutencao total da temporizacao comprovada e drive Push-Pull ativo em MISO e ACK (compatibilidade Crash Bandicoot).

Instrucoes de Gravacao Serial (Cabo USB):
1. Conecte o ESP32 ao computador.
2. Execute o arquivo flash_windows.bat.
3. Informe a porta COM correspondente.

Atualizacao via Web OTA (Sem Fio):
1. Acesse http://wirelessmc.local/ no navegador.
2. Na secao de Atualizacao de Firmware, selecione o arquivo firmware_v1.4.bin.
3. Clique em 'Atualizar Firmware'. O ESP32 reiniciara automaticamente.
