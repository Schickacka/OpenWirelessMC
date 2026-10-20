OpenWirelessMC v1.2 - Release Package
=====================================
Desenvolvido por Schickacka

Novidades da Versao 1.2:
- Banco de dados de titulos titles.db carregado dinamicamente com cache versionado no navegador (renderizacao instantanea < 5ms).
- Zero jogos hardcoded nos assets HTML/JS (economia critica de DRAM e isolamento de dados).
- Suporte a titulos multi-disco / casos especiais adicionados ao titles.db (Policenauts SLPS-00215/6, Resident Evil 3 SLUS-00923).
- Identificacao oficial do cartao inicial 'default.mc' como 'Boot Card'.
- Otimizacao critica de memoria DRAM (buffers dinamicos de Wi-Fi restaurando ~45 KB de RAM livre).
- Suporte a Web OTA com verificacao de cabecalho de imagem.

Instrucoes de Gravacao Serial (Cabo USB):
1. Conecte o ESP32 ao computador.
2. Execute o arquivo flash_windows.bat.
3. Informe a porta COM correspondente.

Atualizacao via Web OTA (Sem Fio):
1. Acesse http://wirelessmc.local/ no navegador.
2. Na secao de Atualizacao de Firmware, selecione o arquivo firmware_v1.2.bin.
3. Clique em 'Atualizar Firmware'. O ESP32 reiniciara automaticamente.
