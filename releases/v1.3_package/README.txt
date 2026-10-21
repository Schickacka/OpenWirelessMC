OpenWirelessMC v1.3 - Release Package
=====================================
Desenvolvido por Schickacka

Novidades da Versao 1.3:
- Restauração de Push-Pull ativo nos pinos MISO (DATA) e ACK com alta impedância dinâmica (Hi-Z).
- Correção definitiva para o Crash Bandicoot e rotinas estritas de detecção de formato do PS1.
- Documentação comparativa completa e aprofundada de engenharia reversa contra o OpenMC original no README.md.
- Manutenção rigorosa de sequências de ACK e tolerância total em leituras fora de limite.
- Incremento e padronização da versão para v1.3 na interface web e pacotes de lançamento.

Instrucoes de Gravacao Serial (Cabo USB):
1. Conecte o ESP32 ao computador.
2. Execute o arquivo flash_windows.bat.
3. Informe a porta COM correspondente.

Atualizacao via Web OTA (Sem Fio):
1. Acesse http://wirelessmc.local/ no navegador.
2. Na secao de Atualizacao de Firmware, selecione o arquivo firmware_v1.3.bin.
3. Clique em 'Atualizar Firmware'. O ESP32 reiniciara automaticamente.
