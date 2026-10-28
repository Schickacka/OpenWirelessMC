# Testes manuais (fora do suite)

Estes arquivos vieram dos ciclos de evidencia da v1.5 e **nao** fazem parte de
`tests/host/run.sh`:

- `test_items_2_4_6.cc` — chama handlers `static` de `src/web/file_server.cc`
  (`api_fs_put_handler`), portanto **nao compila nem linka** fora do arquivo.
  Foi mantido como referencia das checagens que se pretendia fazer; a cobertura
  equivalente hoje esta em `test_web.cc`.
- `test_evidencias_faltantes.cc` — mesmo modelo de mock, tambem sem link com o
  firmware. Referencia.
- `test_matcher.cc` — ferramenta de inspecao: imprime as sugestoes de
  `LookupGameByName` para consultas fixas. Nao tem assercoes.

Para compilar qualquer um deles a mao, use a mesma linha do `run.sh`
(`-I stubs -I ../../src`) e lembre que os tres precisam de ajuste antes de
entrar no suite.
