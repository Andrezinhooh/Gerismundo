# Dependências

O código-fonte das bibliotecas não é incorporado ao arquivo desta entrega.
CMake/vcpkg obtêm e compilam as dependências. Ao distribuir binários, acompanhe
os arquivos de licença/avisos fornecidos por cada pacote instalado.

| Biblioteca | Uso | Licença upstream |
|---|---|---|
| libdatachannel0.22.4 | WebRTC/SRTP, ICE e WebSocket | MPL-2.0 |
| Opus | Codec de voz/áudio | BSD-3-Clause |
| SpeexDSP | Cancelamento de eco e redução de ruído | BSD-3-Clause |
| nlohmann-json3.11.3 | Configuração/sinalização | MIT |
| OpenSSL | TLS/DTLS | Apache-2.0 nas versões atuais |
| libjuice | ICE/STUN/TURN | MPL-2.0 |
| libsrtp | SRTP | BSD-3-Clause |
| usrsctp | Transporte SCTP transitivo | BSD-3-Clause |
| plog | Logging transitivo | MIT |

APIs Windows vêm do sistema/Windows SDK. Python e `websockets` são dependências
exclusivas de teste e não são necessárias ao cliente ou servidor em produção.
