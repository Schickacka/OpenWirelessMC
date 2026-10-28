OpenWirelessMC v1.5 - Release Package
=====================================
Desenvolvido por Schickacka

Novidades da Versao 1.5:
- Hierarquia padronizada de Memory Cards sob /MemoryCards/PS1/ com navegacao simplificada e visualizacao direta.
- Recurso de Duplicacao de Cartoes (/api/card/duplicate): duplicacao instantanea de 128 KB com atribuicao automatica do proximo canal livre (ex: Cartao-1 -> Cartao-2) sob protecao StorageLock.
- Renomeacao direta e segura de arquivos (/api/rename): retencao obrigatoria de extensoes validas (.mc, .mcd, .mcr), validacao de colisoes e bloqueio contra renomeacao de cartoes em uso.
- Criacao de cartoes simplificada (/api/card/create): selecao de pasta ou criacao automatica com resolucao de canais livres e pre-formatacao autentica Sony.
- Ordenacao natural aprimorada na listagem: extracao limpa do titulo real do jogo, garantindo ordenacao alfanumerica correta de franquias e sequencias (ex: Dino Crisis 1 antes de Dino Crisis 2).
- Contadores de cartoes por pasta: exibicao de "X cartoes" (ou "0 cartoes") para todas as subpastas, permitindo rapida identificacao de diretorios vazios.
- Filtro rapido de pastas na interface: alternancia dinamica entre Todas as Pastas, Com Saves e Apenas Vazias.
- Exclusao via AJAX (/api/fs/delete) com Percent-Decode: remocao limpa e confiavel sem redirecionamento para telas JSON brutas, com suporte a caracteres escapados.
- Padronizacao visual completa dos botoes e acoes na interface web, mantendo consistencia tipografica e ergonomica.
- Remocao de botoes redundantes e otimizacao do servidor HTTP embarcado (aumento de pilha e tratadores de URI).

Instrucoes de Gravacao Serial (Cabo USB):
1. Conecte o ESP32 ao computador.
2. Execute o arquivo flash_windows.bat.
3. Informe a porta COM correspondente.

Atualizacao via Web OTA (Sem Fio):
1. Acesse http://wirelessmc.local/ no navegador.
2. Na secao de Atualizacao de Firmware, selecione o arquivo firmware_v1.5.bin.
3. Clique em 'Atualizar Firmware'. O ESP32 reiniciara automaticamente.
