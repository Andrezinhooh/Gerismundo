# Usar o pacote Windows

Extraia **todo** o arquivo `Gerismundo-Windows-x64.zip` para uma pasta gravável.
Abra a pasta extraída e dê dois cliques em **Iniciar.cmd**. Não execute os arquivos
diretamente de dentro do ZIP. Não é necessário instalar Visual Studio, CMake,
Python ou vcpkg para usar os executáveis do pacote.

O pacote usa Windows x64. Windows 11 atualizado é recomendado. O áudio seletivo
do sistema/aplicativo exige build 20348 ou posterior; Windows 10 build 19045 não
oferece essa API. Nesse caso o cliente informa a limitação e permite vídeo sem
áudio seletivo quando Windows Graphics Capture estiver disponível. Conteúdo
protegido e determinadas janelas minimizadas podem não produzir imagem.

## Primeiro uso

O assistente oferece quatro opções:

1. **Testar neste computador:** inicia o servidor somente em `127.0.0.1:8080` e
   abre o cliente. O token é gerado aleatoriamente e salvo apenas nesta pasta.
2. **Hospedar na rede local:** permite conexão de outros computadores da mesma
   LAN. O servidor escuta em todas as interfaces somente após essa escolha. O
   assistente mostra os endereços, sala e token para fornecer aos participantes.
3. **Entrar em servidor existente:** solicita URL, sala, nome e token recebidos de
   quem hospeda. Todos devem utilizar o mesmo servidor, sala e token.
4. **Abrir com configuração salva:** usa o `config.json` existente. O servidor
   precisa estar em execução; essa opção não inicia nem publica um servidor novo.

No aplicativo, clique em **Entrar**. Permita o acesso ao microfone nas configurações
de privacidade do Windows. Se necessário, selecione os dispositivos de áudio no
próprio cliente. Até quatro pessoas podem entrar na sala.

Clique em **Compartilhar tela**, escolha **Telas** ou **Aplicativos**, selecione
720p/1080p e 30/60 FPS e inicie a transmissão. Os volumes de voz e conteúdo são
independentes para cada participante. Para encerrar o compartilhamento, use
**Parar compartilhamento**; a chamada continua.

O aplicativo de chamadas exclui a própria árvore de processos da captura de áudio
de monitor. No modo janela ele inclui somente a árvore do processo selecionado.
Aplicativos multiprocessos podem emitir áudio fora dessa árvore; o aviso do
seletor explica esse limite. O microfone continua sendo uma trilha separada.

## Rede local e internet

Na LAN, os outros computadores escolhem a opção 3 e utilizam um endereço mostrado
pela máquina anfitriã, como `ws://192.168.1.20:8080`. O Windows pode pedir permissão
de rede ao cliente e ao servidor. O assistente não altera o firewall. A URL local
`127.0.0.1` funciona apenas na própria máquina e não deve ser enviada aos amigos.

Hospedar na LAN não publica automaticamente o serviço na internet. Para redes
diferentes são necessários um endereço de sinalização acessível com WSS e uma
configuração STUN/TURN válida. Ajuste `config.json` conforme `config.example.json`
e `docs/signaling.md`. O build padrão suporta TURN/UDP; redes que bloqueiam UDP
não possuem fallback TURN/TCP/TLS. Não há servidor público hospedado incluído.

## Arquivos pessoais e servidor

`config.json` e `host-config.json` são criados no primeiro uso e contêm o token
da sua instalação. Não os envie ao repositório nem compartilhe sua pasta pessoal
com desconhecidos. Os tokens mostrados no modo LAN devem ser fornecidos somente
a quem participará da chamada. Nenhum token real é incluído no ZIP distribuído.

O servidor abre em uma janela de console separada. Fechar o cliente não encerra
o servidor, pois outras pessoas podem continuar usando a sala. Para encerrar o
servidor, feche essa janela. Ao iniciar novamente no mesmo modo, o assistente
reutiliza somente um servidor que identifica como pertencente a esta pasta pelo
PID, caminho do executável e horário de início. Ele não encerra processos alheios.

Se a porta 8080 estiver ocupada por outro processo, o assistente informa o problema
e não tenta se conectar silenciosamente ao processo desconhecido. Feche esse
servidor manualmente ou escolha entrar nele com a URL/token corretos. Para trocar
entre hospedagem local e LAN, primeiro feche a janela do servidor desta pasta.

## O que foi testado

Os resultados executados acompanham o pacote em `TESTES-AUTOMATIZADOS.txt` e nas
evidências em `docs/`. A matriz `docs/TESTES.md` distingue testes de compilação,
Opus, transporte WebRTC, vídeo e inicialização do cliente dos testes que dependem
de microfones, monitores, drivers e redes externas reais. Um build automatizado
aprovado não comprova qualidade acústica, captura de todos os jogos ou desempenho
de GPU em qualquer computador.

O código-fonte da mesma compilação acompanha o pacote em `codigo-fonte.zip`.
`manifest.json` lista os arquivos, tamanhos e checksums. `licencas/` contém os
avisos das bibliotecas distribuídas. A mídia não é gravada em disco pelo cliente.
