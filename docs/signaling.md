# Servidor de sinalização

O executável `native-call-signaling` é C++ e usa o servidor WebSocket do
libdatachannel. Ele organiza salas de até quatro pessoas e encaminha apenas SDP,
candidatos ICE e estado de compartilhamento. Voz, áudio compartilhado e vídeo
viajam diretamente entre os participantes por WebRTC/SRTP; um servidor TURN
encaminha mídia quando a conexão direta não é possível.

## Execução local

Use o mesmo token de acesso e nome de sala nos quatro clientes. O token precisa
ter de 24 a 256 bytes; não é enviado em logs. Não coloque o token na linha de
comando ou em um arquivo versionado.

PowerShell, a partir da pasta do projeto:

```powershell
$env:NATIVE_CALL_TOKEN = Read-Host 'Token de acesso com ao menos 24 caracteres'
.\build\windows\Release\native-call-signaling.exe --bind 127.0.0.1 --port 8080
```

Linux, para o servidor compilado nesse sistema:

```sh
read -r -s -p 'Token de acesso: ' NATIVE_CALL_TOKEN
export NATIVE_CALL_TOKEN
./build/native-call-signaling --bind 127.0.0.1 --port 8080
```

O cliente local usa `ws://127.0.0.1:8080`. Esse endereço não permite conexões de
outros computadores. Em uma rede local de confiança, `--bind 0.0.0.0` escuta em
todas as interfaces; configure o Firewall do Windows e use o IP privado da máquina
servidora nos clientes. O WebSocket simples não cifra o token de sinalização;
use `wss://` sempre que o tráfego sair da máquina local.

## Uso pela internet

Instale o servidor em um host com DNS e coloque-o atrás de um proxy com TLS válido.
Exemplo de configuração Caddy, com o serviço ouvindo apenas em loopback:

```caddyfile
chamadas.seu-dominio.example {
    reverse_proxy 127.0.0.1:8080
}
```

Configure `wss://chamadas.seu-dominio.example` nos clientes. O proxy precisa
suportar WebSocket, limitar conexões e tamanho de requisições e renovar o
certificado TLS. Mantenha a porta interna 8080 inacessível pela internet. O token
é uma senha compartilhada do serviço; todos que o conhecem podem acessar
qualquer sala cujo nome conheçam. Este serviço não implementa contas, convites,
permissões por sala, recuperação de senha ou proteção distribuída contra abuso.

O WebSocket não substitui STUN/TURN. Configure pelo menos um servidor STUN e um
servidor TURN/UDP com credenciais válidas no cliente. O backend libjuice padrão
deste projeto rejeita TURN TCP/TLS; redes que bloqueiam UDP exigem outro backend
ICE e uma compilação coerente com libnice, conforme `transport.md`.
DNS, hospedagem, certificado e TURN
são recursos externos que precisam ser providenciados pelo operador; nenhum
serviço público foi implantado por esta implementação.

## Protocolo e limites

Todas as mensagens são objetos JSON UTF-8. A primeira mensagem, em até dez
segundos, é:

```json
{"type":"join","room":"amigos","name":"Ana","token":"senha-compartilhada"}
```

O servidor responde `welcome`, com um `id` gerado pelo servidor e uma lista
`peers` de objetos `{id,name,sharing,source}`. O recém-chegado inicia uma oferta
WebRTC para cada par da lista. Os participantes existentes recebem `peer-joined`
e aguardam a oferta. A saída gera `peer-left` para os demais participantes.

Mensagens direcionadas:

- `description`: `to`, `descriptionType` (`offer`/`answer`) e `sdp`.
- `candidate`: `to`, `candidate` e `mid`.
- `reset`: `to`; solicita reconstrução da conexão WebRTC após falha.

O servidor acrescenta o `from` autenticado e só encaminha mensagens dentro da
mesma sala. Um `from` fornecido pelo cliente é ignorado. O cliente com menor ID
coordena a reconstrução de um par para evitar ofertas simultâneas.

`share`, com `source` e `active`, atualiza o estado do transmissor. `ping` recebe
`pong` e mantém a sessão ativa. `leave` encerra a sessão. O cliente deve mandar
um `ping` pelo menos a cada 30 segundos; sessões sem mensagens por 120 segundos
são removidas. O serviço também detecta o fechamento do socket.

Os limites são quatro pessoas por sala, 32 salas, 128 conexões, 128 KiB por
mensagem, 120 KiB de SDP, 4 KiB de candidato ICE e uma fila de 512 eventos. Há
um limite de 60 mensagens por segundo por conexão com tolerância inicial de
240 mensagens para a negociação ICE. Nomes de sala e de usuário aceitam até
64 bytes UTF-8 sem caracteres de controle. Respostas lentas com mais de 1 MiB
em espera são desconectadas. A quinta entrada recebe `error` com `code` igual a
`room_full`; as quatro conexões existentes permanecem ativas.

O serviço serializa seu estado em uma thread de trabalho. Chamadas de rede não
são feitas com o mutex da fila retido. Não grava SDP, ICE, tokens ou mídia em
disco.

## Teste automatizado do servidor real

O teste de integração inicia um processo do servidor em uma porta local livre e
usa conexões WebSocket reais. Python e `websockets` são ferramentas de teste,
sem participação no aplicativo C++ ou no serviço de produção.

```sh
python -m pip install 'websockets>=15,<17'
python tests/signaling_integration.py ./build/native-call-signaling
```

No Windows, passe `build/Release/native-call-signaling.exe` como argumento do
segundo comando. O script cobre entrada de quatro participantes, rejeição do
quinto, saída e nova entrada, encaminhamento de SDP/ICE/reset, associação do
remetente ao socket, metadados de transmissão, isolamento de salas, token
incorreto, JSON inválido e limite de profundidade do JSON. Ele verifica que as
conexões existentes seguem respondendo após clientes rejeitados. Esses testes
validam a sinalização; conversação, eco, captura Windows e sincronização de mídia
precisam dos testes específicos do aplicativo e das máquinas Windows.

O teste C++ `native-call-transport-tests`, iniciado pelo runner abaixo, usa quatro
instâncias reais do transporte WebRTC. O runner requer apenas a biblioteca padrão
do Python:

```sh
python tests/transport_runner.py ./build/native-call-signaling ./build/native-call-transport-tests
```

Ele estabelece as doze conexões de pares, confere as 24 rotas independentes de
microfone/compartilhamento entre remetentes e destinatários, a reconstrução de
unidades H.264 de 40 KiB fragmentadas em RTP, o relógio comum de envio, o pedido
de keyframe por RTCP PLI, a troca/parada de metadados da fonte e a continuidade
do áudio após a saída de um participante. Os payloads determinísticos existem
somente no teste de transporte, não substituem captura ou codecs do aplicativo.
O teste não decodifica essas unidades H.264 nem mede sincronização audiovisual
de dispositivos reais. No Windows, ajuste os caminhos para a pasta `Release` e
os nomes terminados em `.exe`.
