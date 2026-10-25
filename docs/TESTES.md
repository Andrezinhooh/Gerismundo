# Verificação executada e roteiro Windows

## Pacotes Windows e evidência por versão

O workflow Windows executa seis grupos: abertura real do cliente Win32,
roundtrip D3D11/Media Foundation em 720p/1080p, filas, Opus, quatro clientes
WebRTC/SRTP e sinalização. Em seguida extrai o ZIP e abre o executável portátil
novamente. Uma release só é publicada se essas etapas forem aprovadas. Os arquivos
`TESTES-AUTOMATIZADOS.txt` e `portable-smoke.json` da release registram o que de
fato foi executado para aquele commit. Testes físicos da matriz abaixo continuam
separados; os resultados Linux iniciais são mantidos como evidência histórica.

## Evidência desta sessão

Data: 25/10/2026. Ambiente de execução: Linux x64, GCC 14, CMake 4.4.4,
libdatachannel 0.22.4 compilada de seu código-fonte. Não houve acesso a um desktop
Windows, microfone, monitores ou GPU Windows. Testes de transporte usam pacotes
determinísticos apenas como fixtures; a aplicação captura mídia de dispositivos
reais e nunca utiliza essas fixtures.

| Verificação | Resultado e alcance |
|---|---|
| CMake/build Linux | Servidor WebSocket e biblioteca de transporte compilados e ligados |
| `core` | Passou: sobrecarga limitada, descarte de valores antigos, fechamento que acorda consumidor, drenagem e contabilidade concorrente |
| `opus-jitter` | Passou: 18 verificações sobre o mesmo encoder/jitter usado na produção, com libopus real; pacotes10ms, reordenação, duplicatas, atrasos, PLC/FEC, limite de memória e reinício de timestamps |
| `signaling` | Passou: conexões WebSocket reais, quatro entradas, rejeição do quinto, SDP/ICE/reset, remetente vinculado à conexão, isolamento de salas, metadados de fonte, saída/reentrada, token incorreto e JSON malformado |
| `webrtc-mesh` | Passou: quatro clientes reais com ICE/DTLS/SRTP, 12 pontas de conexão (seis pares), 24 caminhos independentes de microfone/áudio compartilhado; H.264 de40KiB fragmentado e reconstituído byte a byte, relógios RTP comuns, PLI, troca/parada de fonte e continuidade dos três restantes |
| Win32 `ui.cpp`, `main.cpp`, `controller.cpp` | Sintaxe verificada com compilador cruzado MinGW C++20 e cabeçalhos Windows; não executados |
| WASAPI `audio.cpp` | Sintaxe Windows verificada com MinGW e cabeçalho oficial Application Loopback moderno, Opus/SpeexDSP reais; não executado |
| MF `video.cpp` | Sintaxe com avisos estritos aprovada; vínculo PE com import libraries de Media Foundation/D3D aprovado mediante adaptação de UUID ausente em cabeçalho MinGW; não executado |
| WGC `capture.cpp` | Parsing diagnóstico com Clang19 e projeções oficiais SDK22621, usando adaptações temporárias de compatibilidade dos headers; sem erros na fonte do aplicativo; não equivale ao build MSVC |
| Compilação completa MSVC | Pendente de ambiente Windows; verificações cruzadas não substituem esse teste |
| NAT/TURN em redes diferentes | Pendente; os testes automatizados usam interfaces locais, sem servidor TURN |

O log do CTest do projeto está em `validation-linux.txt`. A compilação e os
testes manuais Windows são critérios restantes para considerar o aplicativo
validado. Não há declaração de que eco acústico, áudio de jogos ou aceleração de
GPU tenham sido testados nesta sessão.

## Reproduzir os testes automatizados

No Windows, após compilar conforme o README:

```powershell
python -m pip install "websockets>=15,<17"
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

O CMake registra o teste de sinalização quando o Python consegue importar
`websockets`. O runner WebRTC usa somente a biblioteca padrão Python. O teste de
Opus é registrado quando seus cabeçalhos/biblioteca estão disponíveis; verifique
a lista do CTest, pois ausência de dependência não deve ser interpretada como
aprovação do teste omitido.

Para servidor, transporte e testes em Linux:

```bash
cmake -S . -B build/linux -DNATIVE_CALL_BUILD_CLIENT=OFF \
  -DNATIVE_CALL_FETCH_DEPENDENCIES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j4
ctest --test-dir build/linux --output-on-failure
```

Instale os headers de Opus para executar `opus-jitter`; OpenSSL development,
compiladores C/C++, Git e Python são necessários para o build/transporte.

## Cenários obrigatórios em quatro computadores Windows

Preparação: quatro máquinas Windows11 atualizadas, fones/microfones, um jogo e
um navegador produzindo áudio distinguível. Use pelo menos duas redes externas,
um servidor `wss://` e TURN/UDP com credenciais reais. Inicialmente teste em LAN;
depois repita os cenários pela internet e force uma rede em que o peer precise de
relay. Registre build Windows, dispositivos, GPU, resolução/FPS e configuração de
rede sem credenciais.

| # | Cenário e critério de aprovação | Estado |
|---|---|---|
| 1 | Quatro entram e conversam ao mesmo tempo; um quinto é recusado | Transporte/sala aprovados automaticamente; voz em dispositivos pendente |
| 2 | A compartilha o monitor selecionado; B/C/D veem o conteúdo correto | Pendente Windows |
| 3 | A reproduz jogo e navegador simultaneamente, com sons identificáveis | Pendente Windows |
| 4 | B/C/D ouvem ambos no modo monitor, com microfone independente | Pendente Windows |
| 5 | B fala; C/D não recebem novamente essa voz no áudio compartilhado de A | Pendente Windows; verificar faixas separadamente e repetir com alto-falantes/fones |
| 6 | A troca para a janela do jogo; sala e microfone continuam | Metadados/continuidade de transporte aprovados; captura pendente |
| 7 | B/C/D veem somente a janela, inclusive após mover/sobrepor/redimensionar | Pendente Windows |
| 8 | Apenas áudio da árvore do jogo chega na faixa compartilhada; navegador não | Pendente Windows; registrar limitações de árvore multiprocesso |
| 9 | A fala enquanto compartilha; mute de uma fonte preserva a outra | Trilhas independentes aprovadas no transporte; captura/mixer pendentes |
| 10 | Alternar monitor/janela sem encerrar a chamada, inclusive com o áudio antes silenciado | Metadados aprovados; ponta a ponta pendente |
| 11 | Minimizar o aplicativo de chamadas; imagem, áudio e voz continuam | Pendente Windows; testar também com a fonte separadamente minimizada e registrar comportamento |
| 12 | B zera somente volume compartilhado de A; voz de A permanece, C/D não mudam | Pendente reprodução Windows |
| 13 | D sai; A/B/C continuam trocando voz e transmissão | Transporte aprovado automaticamente; dispositivos pendentes |
| 14 | Parar/fechar a fonte libera captura, encoder e loopback; chamada permanece | Pendente Windows/resource profiling |

## Recuperação, compatibilidade e desempenho

- Feche a janela capturada e desconecte o monitor; confirme mensagem, término do
  compartilhamento e continuidade da chamada.
- Repita iniciar/parar/trocar fonte100 vezes. Compare handles, threads, uso de RAM,
  memória de GPU e sessões de áudio antes/depois; procure crescimento contínuo.
- Desconecte/reconecte o dispositivo de áudio. Confirme recuperação ou mensagem
  explícita após tentativas limitadas, sem interface congelada.
- Interrompa a rede brevemente e reinicie o serviço de sinalização. Confirme nova
  negociação e entrada de IDR; não confunda sala reconectada com mídia conectada.
- Teste 720p30,720p60,1080p30,1080p60 com jogo pesado. Registre FPS recebidos,
  CPU/GPU, bitrate, perda, latência da voz e diferença temporal áudio/vídeo.
  Não existe medição de latência aprovada nesta entrega.
- Reduza banda e introduza perda/jitter. Confirme queda de bitrate, filas limitadas,
  recuperação de IDR e prioridade da voz. Observe uso total de upload mesh.
- Windows build<20348: áudio seletivo deve ficar indisponível, com explicação;
  vídeo sem áudio pode funcionar. Não deve aparecer áudio total por fallback.
- Em navegador multiprocesso, reproduza áudio em processos diferentes e confirme
  o limite do PID escolhido. Outro aplicativo de comunicação selecionado pode
  compartilhar suas vozes, conforme aviso exibido.
- Verifique conteúdo protegido e janela minimizada: mensagem/ausência de quadros
  conforme restrição do Windows, sem capturar outra região da tela por substituição.
- Confirme que mute de microfone/deafen/volumes sobrevivem à troca de dispositivo
  e que áudio compartilhado anteriormente silenciado volta somente quando solicitado.

## Aceite restante

Antes de distribuir como aplicativo funcional validado, executar o build MSVC,
abrir a interface em Windows e concluir os cenários acima em máquinas reais.
O workflow incluído é preparação para essa validação, não uma execução comprovada.
