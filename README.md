# Native Call — C++20 para Windows

Projeto independente de chamadas de até quatro pessoas com interface Win32,
microfone Opus, compartilhamento WGC e áudio seletivo por processo. O material
inicial continha apenas os requisitos, e o repositório de destino estava vazio.
Portanto, esta entrega não modifica nem valida integração com um aplicativo anterior.

[Baixar o código-fonte em ZIP](https://github.com/Andrezinhooh/Gerismundo/raw/refs/heads/main/dist/native-call.zip).
Extraia o ZIP e abra a pasta `native-call`. Você também pode clonar este repositório:

```powershell
git clone https://github.com/Andrezinhooh/Gerismundo.git
cd Gerismundo
```

O ZIP contém o código-fonte e a documentação; não inclui um executável Windows
compilado. O checksum está em `dist/native-call.sha256`.

**Estado da entrega:** código implementado, servidor e transporte compilados e
testados em Linux. Partes Win32 passaram por verificação sintática cruzada.
A compilação completa com MSVC e a execução de captura/reprodução no Windows
ainda precisam ser feitas. Não há executável Windows validado nesta entrega.

## O que o código faz

- Uma sala admite quatro participantes; um quinto recebe erro sem encerrar a sala.
- Cada par mantém três trilhas WebRTC RTP/SRTP identificadas: `microphone`,
  `shared-audio` e `shared-video`. O microfone não é misturado ao compartilhamento.
- WASAPI captura e reproduz PCM estéreo a 48 kHz; Opus usa pacotes de 10 ms.
  SpeexDSP trata eco/ruído somente na voz; o áudio compartilhado preserva estéreo.
- Volume de voz e de compartilhamento são locais e independentes por participante.
  Há mute de microfone, deafen, mute do áudio transmitido e indicação de atividade.
- O seletor nativo enumera monitores e janelas novamente a cada abertura.
  Exibe prévias reais, ícones das janelas e identificação do processo.
- Monitor: `CreateForMonitor` e Application Loopback em
  `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`, com o PID deste aplicativo.
- Janela: `CreateForWindow`, HWND/PID revalidados e Application Loopback em
  `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`, com o PID selecionado.
- O vídeo acompanha redimensionamento; o fechamento da fonte encerra o
  compartilhamento e preserva a chamada. A captura usa workers e continua quando
  o aplicativo de chamadas é minimizado.
- Opções 720p/1080p, 30/60 FPS, áudio habilitado inicialmente onde suportado,
  troca de fonte, reprodução remota e tela cheia por botão/F11.
- Media Foundation tenta encoder H.264 de hardware; há fallback de encoder por
  software. D3D11 faz captura, escala e conversão NV12 quando disponíveis.
- Filas de mídia têm limites. Perda/atraso de vídeo pede IDR por RTCP PLI;
  NACK recupera fragmentos. REMB e sobrecarga ajustam o bitrate.
- Sinalização WebSocket autenticada, STUN/TURN configuráveis, reconexão da
  sinalização e reconstrução dos peers após falha de ICE.

## Compilar no Windows

Requisitos:

1. Visual Studio 2022 com **Desenvolvimento para desktop com C++** e CMake.
2. Windows SDK **10.0.22621.0**, incluindo C++/WinRT, instalado no Visual Studio.
3. Git e CMake 3.25 ou posterior.
4. vcpkg com histórico de versões disponível. O manifest fixa o baseline e
   libdatachannel 0.22.4/nlohmann-json 3.11.3.

No PowerShell de desenvolvimento do Visual Studio, dentro deste projeto:

```powershell
git clone https://github.com/microsoft/vcpkg.git work/vcpkg
& ./work/vcpkg/bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = (Resolve-Path ./work/vcpkg).Path
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

O executável fica em `build/windows/Release/native-call.exe` e o servidor em
`build/windows/Release/native-call-signaling.exe`. O toolchain do vcpkg faz a
cópia das DLLs necessárias ao diretório dos executáveis. Para distribuir, inclua
essas DLLs e os avisos das dependências; teste em um Windows sem Visual Studio.

O workflow `.github/workflows/windows.yml` automatiza a compilação e os testes
quando este projeto for colocado em um repositório. Ele foi incluído, mas não foi
executado em um serviço externo nesta sessão.

## Executar localmente

Defina um token de pelo menos 24 caracteres no terminal do servidor:

```powershell
$env:NATIVE_CALL_TOKEN = 'troque-por-um-token-aleatorio-com-pelo-menos-24-caracteres'
./build/windows/Release/native-call-signaling.exe --bind 127.0.0.1 --port 8080
```

Em outro terminal:

```powershell
Copy-Item config.local.example.json build/windows/Release/config.json
notepad build/windows/Release/config.json
./build/windows/Release/native-call.exe
```

No JSON, preencha o mesmo token e o nome do participante. Também é possível
fornecer `native-call.exe --config caminho/config.json`. URL, sala, nome e token
podem ser editados na interface; os servidores ICE vêm do JSON. Não há gravação
de mídia em disco.

Entre na chamada, selecione os dispositivos e use **Compartilhar tela**. Escolha
uma fonte, resolução/FPS e **Iniciar transmissão**. O controle de áudio do
compartilhamento pode ser ativado depois: ele inicia a captura seletiva real
quando necessário. Os sliders de cada participante controlam somente sua escuta.

## Usar pela internet

Todos usam a mesma URL acessível de sinalização, sala e token. Para acesso remoto,
publique o servidor atrás de um proxy HTTPS com suporte a WebSocket e use `wss://`.
O servidor escuta somente localhost por padrão; `--bind 0.0.0.0` deve ser usado
apenas quando a implantação exigir exposição direta. Exemplo em `deploy/Caddyfile`.

Configure STUN e um servidor TURN/UDP real em `config.example.json`. Credenciais
de exemplo não funcionam. TURN é necessário quando NAT/firewall impede conexão
direta; abrir apenas a porta WebSocket não resolve o transporte de mídia.
Consulte `docs/signaling.md`, `docs/transport.md` e `deploy/turnserver.conf.example`.

O backend libjuice desta configuração suporta **TURN por UDP**. URLs TURN TCP/TLS
são rejeitadas explicitamente; redes que bloqueiam UDP não têm fallback automático
nesta entrega. Não foi provisionado nem testado um servidor TURN na internet.

## Compatibilidade e limites atuais

- Windows 11 atualizado é o alvo principal. Application Loopback exige build
  **20348 ou posterior**. Windows 10 comum, inclusive build 19045, não satisfaz
  esse requisito. Nesses sistemas a interface permite somente vídeo, quando WGC
  estiver disponível; nunca troca silenciosamente para loopback total.
- A exclusão de áudio protege este processo e seus descendentes. Toda reprodução
  da chamada ocorre neste executável; não há processo auxiliar independente.
  Ao integrar outro motor/processo de áudio, esse requisito precisa ser revisto.
- Aplicativos multiprocessos podem emitir áudio em processos fora da árvore do
  HWND escolhido. O seletor exibe esse aviso. A captura não identifica
  semanticamente abas de navegador nem garante isolamento entre todas as abas.
  Se selecionar outro aplicativo de comunicação, suas vozes também podem ser
  compartilhadas. Conteúdo protegido respeita as restrições do Windows.
- O compartilhamento pode não atualizar uma janela minimizada, suspensa ou com
  conteúdo protegido, conforme a aplicação e o Windows. Minimizar **o aplicativo
  de chamadas** não interrompe os workers de captura.
- Há fallback de codificação por CPU e cópias para decodificação/apresentação
  BGRA/GDI. A implementação não oferece pipeline integralmente sem cópias.
  Suporte e desempenho de aceleração precisam ser medidos em GPUs reais.
- Bitrate é adaptativo, mas resolução/FPS permanecem nos valores escolhidos;
  não há mudança automática de resolução nem controle de congestionamento GCC
  completo do libwebrtc. A topologia mesh envia uma cópia por espectador, podendo
  triplicar o upload de um compartilhamento.
- Áudio e vídeo usam a mesma linha de tempo RTP do emissor. O visualizador usa
  fila limitada e posição de áudio para aproximar a reprodução; latência acústica,
  desvio de relógio e sincronismo sob perda prolongada não foram medidos.
- Trocar dispositivos durante compartilhamento solicita interromper a transmissão
  primeiro. A chamada permanece conectada durante a troca de dispositivos.
- Não há login individual, persistência de preferências, serviço público de
  sinalização/TURN hospedado ou atualização automática. O token é compartilhado.

## Arquivos e dependências

Todos os arquivos são novos. Os módulos principais são:

| Arquivo | Responsabilidade |
|---|---|
| `src/audio.cpp`, `src/audio_codec.hpp` | WASAPI, process loopback, Opus, AEC, mixer e jitter |
| `src/transport.cpp` | WebRTC nativo, RTP/RTCP, mesh, sinalização e reconexão |
| `src/signaling_server.cpp` | Serviço WebSocket e capacidade das salas |
| `src/capture.cpp` | WGC, fontes HWND/HMONITOR, D3D11 e ciclo de vida |
| `src/video.cpp` | Encoder/decoder Media Foundation e conversão de vídeo |
| `src/controller.cpp` | Integração, comandos fora da UI e reprodução remota |
| `src/ui.cpp`, `src/main.cpp`, `app.manifest` | Interface Win32, prévias, DPI e configuração |
| `include/nativecall/` | Interfaces públicas para futura integração |
| `tests/`, `docs/` | Testes executáveis, resultados e roteiro de validação |
| `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json` | Build e dependências |

Dependências de produção: **libdatachannel 0.22.4**, **Opus**, **SpeexDSP** e
**nlohmann-json 3.11.3**, mais componentes transitivos de libdatachannel (OpenSSL,
libjuice, libsrtp, usrsctp e plog). Win32, WASAPI, WGC, D3D11 e Media Foundation
vêm do SDK/Windows. Não há Electron, servidor web para a interface, arquivos de
áudio de demonstração ou captura simulada na aplicação.

Nesta sessão foram usados CMake 4.4.4/Ninja 1.13.2 isolados no diretório de
trabalho, GCC 14, libdatachannel compilada do código-fonte, libopus disponível no
ambiente e um toolchain MinGW desempacotado para verificação sintática. Nenhuma
biblioteca foi instalada no computador Windows do usuário. Python e o pacote
`websockets` são usados somente pelos testes de sinalização.

## Testes

Veja `docs/TESTES.md` para resultados efetivamente executados e os 14 cenários
que exigem validação em computadores Windows. A aprovação dos testes de transporte
não significa aprovação de captura de microfone/tela, exclusão acústica de eco,
aceleração de hardware ou operação entre redes diferentes.
