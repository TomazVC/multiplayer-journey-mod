# Journey Coop

Mod para o **Journey (Steam, Windows)** que faz duas pessoas sempre jogarem juntas, usando um **código de sala** em vez do pareamento aleatório.

## Instalação rápida

1. Baixe `journey-coop.zip` em **[Releases](../../releases/latest)**.
2. Extraia, feche o Journey e rode `install.cmd`.
3. Digite o código da sala. Ele tem que ser **igual nos dois PCs**.
4. Abram o Journey pela Steam e comecem o mesmo capítulo.

O passo a passo completo está em [docs/LEIA-ME.txt](docs/LEIA-ME.txt), que também vai dentro do ZIP. Para desinstalar, rode `uninstall.cmd`.

## Como funciona

O Journey de PC encontra companheiros em **lobbies públicos da Steam** (as "meeting rooms") e depois conversa com eles por **P2P do SteamNetworking**. Antes de conectar, ele filtra o outro jogador por versão, metahash, nível e distância.

O mod é um proxy da `steam_api64.dll`. A original é renomeada para `steam_api64_o.dll`, e todas as ~800 funções são repassadas a ela pelo [`.def`](src/steam_api64.def). A única função interceptada é `SteamInternal_CreateInterface`, que entrega o `ISteamClient017`. A partir dele o mod faz patch das vtables de:

| Interface | O que o mod faz |
|---|---|
| `ISteamMatchmaking` (009) | Renomeia toda chave de lobby que o jogo usa para `jc<SALA>_<chave>`. Também grava `jcoop_room=<SALA>` e filtra as buscas por essa chave, com distância *worldwide*. Resultado: desconhecidos não veem a sala de vocês, e vocês não veem a deles. |
| `ISteamNetworking` (005) | Leva os pacotes do jogo pelo transporte próprio `src/rtp.h` (canal 8). São pacotes *unreliable* com confiabilidade, ordem e gerações por sessão implementadas pelo mod, para não depender do fluxo *reliable* da Steam. Mantém uma rota P2P permanente e aquecida com o parceiro (ping/pong no canal 7), para a conexão do jogo abrir na hora mesmo em longa distância. O SteamID do parceiro é descoberto pela sala e salvo em `partner=`. Depois disso, P2P de outros SteamIDs é descartado. Como a sessão Steam fica sempre aberta, o mod emula para o jogo os callbacks `P2PSessionRequest`/`P2PSessionConnectFail` e o fechamento de sessão com o parceiro. |

O instalador também ajusta `Data/Scripts/Vars.lua`. Ele aumenta os raios de encontro (`kJoinRadiusFar/Near`, `kLeaveRadius`), para a dupla se conectar logo ao entrar na fase e não se separar por distância. E aumenta os timeouts de rede (`ConnectionBarn.kTimeoutTime` 60 s, `kLevelChangeTimeoutTime` 30 s, `Matchmaker.kDefaultRequestTimeout` 60 s), para conexões de longa distância terem tempo de se estabelecer. O arquivo original fica salvo em `journey-coop-backup/`.

Com o `room=` vazio no `journey-coop.ini`, o mod não intercepta nada e só repassa as chamadas.

### Arquivos instalados na pasta do jogo

```
steam_api64.dll        <- o mod
steam_api64_o.dll      <- a DLL original da Steam
journey-coop.ini       <- room / partner / log
journey-coop.log       <- diagnóstico
journey-coop-backup/   <- cópias da steam_api64.dll e do Vars.lua originais
```

## Compilar

É preciso ter o [Zig](https://ziglang.org) instalado (testado com a versão 0.17.1):

```sh
./build.sh            # gera dist/journey-coop.zip
```

O CI (`.github/workflows/release.yml`) compila em todo push. Quando uma tag `v*` é criada, ele publica o ZIP em Releases.

Testado com `Journey.exe` sha256 `16a7c176…788f` e `steam_api64.dll` sha256 `38f6a1dd…9b29`.

## Versões

- **v0.4.0**: os pacotes do jogo passam a ir por um **transporte próprio** (`src/rtp.h`): pacotes *unreliable* da Steam com sequência, ACKs e retransmissão feitos pelo próprio mod. Cada sessão de jogo tem uma geração aleatória, e pacotes de sessões antigas são descartados. Isso corrige o fluxo *reliable* da Steam que travava num sentido depois da troca de capítulo (só ~7 pacotes passavam). O transporte é testado num simulador com perda, duplicação e reordenação (`test/rtp_test.c`, que roda no CI).
- **v0.3.1**: quando o jogo encerra a sessão com o parceiro (por exemplo, na troca de capítulo), o mod reseta também a sessão Steam e reaquece a rota na hora. Isso resolve a reconexão de mão única no capítulo 2. O log passa a mostrar quantos pacotes de jogo foram enviados e recebidos em cada sessão.
- **v0.3.0**: mantém uma rota P2P **permanente** com o parceiro, com um ping por segundo no canal 7. Assim, conexões de longa distância (Brasil ↔ Itália) não estouram mais o limite de ~20 s do jogo. O SteamID do parceiro é descoberto sozinho e salvo no ini. O mod também emula para o jogo a abertura e o fechamento das sessões com o parceiro.
- **v0.2.1**: o instalador também aumenta os timeouts de rede do jogo no `Vars.lua`. A conexão passa de 20 para 60 s, a troca de fase de 10 para 30 s e os pedidos de lobby de 20 para 60 s.
- **v0.2.0**: corrige a conexão de mão única em longa distância (ex.: Brasil ↔ Itália). Enquanto o parceiro ainda não respondeu, os pacotes vão como *reliable*, então a Steam guarda e entrega quando a rota P2P fica pronta. O log também registra a latência e o motivo de cada desconexão.
- **v0.1.1**: diagnóstico. O log registra as mensagens do jogo e cada chamada de matchmaking e rede.
- **v0.1.0**: primeira versão, com código de sala.
