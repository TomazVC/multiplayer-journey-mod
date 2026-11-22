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
| `ISteamNetworking` (005) | É opcional (`partner=` no ini). Descarta P2P de qualquer SteamID que não seja o do parceiro e registra no log os SteamIDs vistos. |

O instalador também aumenta os raios de encontro em `Data/Scripts/Vars.lua` (`kJoinRadiusFar/Near`, `kLeaveRadius`), para a dupla se conectar logo ao entrar na fase e não se separar por distância. O arquivo original fica salvo em `journey-coop-backup/`.

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

- **v0.2.0**: corrige a conexão de mão única em longa distância (ex.: Brasil ↔ Itália). Enquanto o parceiro ainda não respondeu, os pacotes vão como *reliable*, então a Steam guarda e entrega quando a rota P2P fica pronta. O log também registra a latência e o motivo de cada desconexão.
- **v0.1.1**: diagnóstico. O log registra as mensagens do jogo e cada chamada de matchmaking e rede.
- **v0.1.0**: primeira versão, com código de sala.
