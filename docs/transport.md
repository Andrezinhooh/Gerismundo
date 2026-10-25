# Transporte de mídia nativo

O cliente usa `libdatachannel` para ICE, DTLS, SRTP e WebSocket. A mídia não
trafega como JSON, arquivos ou mensagens de DataChannel: cada par de usuários
negocia três tracks RTP bidirecionais desde a entrada na chamada:

| MID | Conteúdo | Codec / relógio RTP |
| --- | --- | --- |
| `microphone` | Voz do usuário | Opus estéreo / 48 kHz |
| `shared-audio` | Áudio seletivo do conteúdo | Opus estéreo / 48 kHz |
| `shared-video` | Monitor ou janela | H.264 Annex B / 90 kHz |

A troca de fonte inicia ou interrompe somente os produtores de compartilhamento.
Os tracks continuam negociados e o microfone continua independente. A sinalização
distribui nome/estado da fonte aos espectadores. Os dois áudios têm decodificação,
fila e ganho próprios no receptor; conteúdo recebido não é ligado aos produtores
de mídia enviados pela rede.

## Temporização e descarte

Os timestamps RTP dos três tracks derivam do mesmo instante de entrada do
transmissor, convertido de `mediaNowUs()`. O receptor estende timestamps de 32 bits
e sequências de 16 bits, inclusive quando dão a volta; entrega PTS relativos ao
transmissor, comparáveis entre áudio compartilhado e vídeo. Esses PTS não devem
ser comparados diretamente ao relógio local do computador receptor. O controlador
usa a posição consumida pelo mixer para o buffer WASAPI como referência aproximada
de apresentação; ela não mede o instante acústico de saída do dispositivo.
RTCP Sender Reports também acompanham a mídia enviada.

A thread serial de transporte processa voz antes de vídeo. Há no máximo 20 pacotes
de áudio de saída,64 de entrada, quatro quadros H.264 de saída e oito quadros
H.264 por receptor. A captura e o encoder têm suas próprias filas limitadas.
Como quadros P dependem de quadros anteriores, o transporte nunca substitui um
quadro P arbitrariamente: no estouro da fila, descarta a cadeia pendente, pede
um IDR e só aceita novamente um quadro independente com SPS/PPS/IDR.

O remontador H.264 aceita NALs simples, STAP-A e FU-A, valida tamanhos, limita um
quadro a 4 MiB e espera no máximo 80 ms por fragmentos. Após uma tolerância de 8 ms
para reordenação, solicita até 64 sequências ausentes por RTCP NACK, com intervalo
mínimo de 20 ms. O remetente mantém 512 pacotes para retransmissão. Um quadro perdido
fora do prazo provoca RTCP PLI e espera por IDR, sem bloquear a chamada de voz.
Pedidos PLI gerados pelo próprio receptor têm intervalo mínimo de 500 ms.

RTCP REMB informa ao transmissor uma sugestão de bitrate limitada entre 250 kbit/s
e 12 Mbit/s. A sugestão considera quadros que não completaram a tempo e a menor
restrição dos espectadores. O controlador limita esse valor à qualidade escolhida.
Isso é uma adaptação simples; não é o controlador de congestionamento completo
do projeto Google libwebrtc, nem uma garantia de 1080p60 em qualquer rede.

## Conexões e recuperação

A sala é limitada a quatro participantes pelo servidor, formando até seis
conexões entre pares; cada transmissor envia uma cópia por espectador. O custo
de upload de um compartilhamento com três espectadores é aproximadamente três
vezes o bitrate do vídeo, além de áudio e overhead. Não há SFU nem transcodificação
no servidor de sinalização.

O recém-chegado inicia as ofertas SDP. Em falhas de ICE, o participante de menor
ID solicita `reset` e reconstrói o par, com até quatro tentativas. Isso evita
ofertas simultâneas. A perda do WebSocket reconstrói a participação na sala com
novo ID; há até oito tentativas automáticas, com atraso de 1 a 30 segundos. Depois
de falha definitiva, credenciais inválidas ou sala cheia, o callback `stopped`
encerra a captura e devolve o aplicativo ao estado desconectado.

O build padrão usa libjuice e suporta STUN e TURN por UDP. TURN por TCP ou TLS é
recusado explicitamente, pois essa versão do backend não oferece tais transportes.
Redes que bloqueiam todo UDP exigem recompilar libdatachannel com libnice e
habilitar `NATIVE_CALL_USE_NICE`; ligar apenas a opção no aplicativo não adiciona
capacidade ao backend. Os testes automatizados usam ICE direto em loopback.
TURN, NAT distintos, Wi-Fi congestionado e perda real de rede ainda precisam
ser verificados em máquinas diferentes, conforme a matriz de testes.

Todos os callbacks públicos do transporte executam em sua thread serial e devem
retornar rapidamente. O controlador transfere decodificação de vídeo e operações
Win32 para workers próprios. `join`, `leave` e destruição não devem ser chamados
de dentro de um callback do mesmo transporte; `leave` aguarda a thread terminar,
fecha os pares e impede callbacks posteriores ao retorno.
