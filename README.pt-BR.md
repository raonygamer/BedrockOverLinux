# BedrockOverLinux

[English](README.md) · **Português (Brasil)**

CLI em C++ para gerenciar Minecraft Bedrock GDK no Linux. Rode `bedrock` para ver a ajuda ou acrescente `--help` a um comando.

## Instalar

No Arch, Ubuntu, Debian ou Fedora x86_64, clone o repositório e execute o instalador.

```sh
git clone https://github.com/raonygamer/BedrockOverLinux.git
cd BedrockOverLinux
./install.sh
```

O instalador obtém as dependências de compilação, compila o CLI e instala o comando em `~/.local/bin`, junto com as ferramentas independentes em sua pasta de configuração. Use `./install.sh --cli-only` para instalar somente o CLI. Em sistemas apt sem o pacote .NET 8, o xvdtool usa o runtime oficial da Microsoft em uma pasta privada, verificado por SHA-512. Para jogar, use uma sessão gráfica com systemd de usuário, Secret Service e drivers Vulkan.

Com `~/.local/bin` no PATH, entre na conta Microsoft que possui o Minecraft for Windows e crie um perfil que acompanha a versão estável.

```sh
bedrock tools check
bedrock auth login
bedrock profile create servidor --version latest_release
bedrock profile servidor launch
```

O primeiro lançamento baixa o jogo. Para escolher uma versão específica, consulte `bedrock version list`, baixe-a com `bedrock version <versão> download` e use `bedrock profile servidor version set <versão>`.

## Ferramentas externas

`bedrock tools list` mostra os caminhos locais. `tools info [ferramenta]` detalha a seleção, `tools check [ferramenta]` consulta as releases e verifica se o programa executa. `tools install [ferramenta]` e `tools update [ferramenta]` obtêm a release Linux, conferem o SHA-256 publicado pelo GitHub e só ativam a versão depois de testar o executável. Sem argumento, operam sobre as três ferramentas. Cada versão fica em seu próprio diretório. Um processo em execução continua usando seus arquivos após um update.

- [Xodus](https://github.com/BedrockNative/xodus) fornece login, licenças, instalação e execução licenciada. O binário Linux requer as bibliotecas nativas do upstream, incluindo WebKitGTK 4.1 e um Secret Service funcional no desktop.
- [WineGDK](https://github.com/BedrockNative/WineGDK) executa o cliente Windows.
- [xvdtool](https://github.com/AmethystAPI/xvdtool) inspeciona os metadados do pacote com `bedrock version <versão|uuid> info --package`. A distribuição Linux v1.0.3 requer o runtime .NET 8.

Para usar instalações próprias, forneça caminhos absolutos em `BEDROCK_XODUS_CLI`, `BEDROCK_XODUS_SERVICE`, `BEDROCK_WINE`, `BEDROCK_WINESERVER` ou `BEDROCK_XVDTOOL`. O WineGDK é identificado como `winegdk` ao procurar no PATH.

## Comandos

```text
bedrock auth login
bedrock auth list
bedrock auth refresh
bedrock auth logout <gamertag>
bedrock auth default info
bedrock auth default set <gamertag>

bedrock profile list
bedrock profile create <nome> [--version <versão|uuid|latest_release|latest_preview>]
bedrock profile delete <nome|uuid>
bedrock profile <nome|uuid> delete
bedrock profile <nome|uuid> info
bedrock profile <nome|uuid> launch
bedrock profile <nome|uuid> env list
bedrock profile <nome|uuid> env set <chave> <valor>
bedrock profile <nome|uuid> env unset <chave>
bedrock profile <nome|uuid> prelaunch|wrapper|args info
bedrock profile <nome|uuid> prelaunch|wrapper|args set -- <argumentos...>
bedrock profile <nome|uuid> prelaunch|wrapper|args clear
bedrock profile <nome|uuid> account info
bedrock profile <nome|uuid> account set <gamertag>
bedrock profile <nome|uuid> version info
bedrock profile <nome|uuid> version set <versão|uuid|latest_release|latest_preview>
bedrock profile <nome|uuid> prefix info
bedrock profile <nome|uuid> prefix set <prefixo>
bedrock prefix list

bedrock services list
bedrock services info <gamertag>
bedrock services licence <gamertag>
bedrock services ciks <gamertag>
bedrock services kill <gamertag>

bedrock version list
bedrock version <versão|uuid> info [--package]
bedrock version <versão> download [--copy]

bedrock env COM_MOJANG set <nome|uuid>
```

O login fica salvo assim que o Xodus confirma a conta. Se a consulta do gamertag falhar, use o UUID listado ou o nome Microsoft. `bedrock auth refresh` tenta obter o gamertag novamente.

Os comandos de perfil exigem login. O primeiro uso cria `release` e `preview`. Cada perfil guarda o UUID da conta padrão vigente na criação. Um perfil recém-criado sem versão precisa de `version set` antes de jogar. Os nomes e gamertags servem para seleção interativa. As referências salvas usam UUID.

Com `--version latest_release` ou `--version latest_preview`, o perfil consulta o canal a cada lançamento e instala a versão compartilhada se necessário. Se o catálogo estiver offline, ele pode usar a instalação já resolvida. Use `version set` com um número ou UUID para voltar a fixar a versão.

O catálogo é o [minecraft-windows-gdk-version-db](https://github.com/LukasPAH/minecraft-windows-gdk-version-db). Uma instalação compartilhada é selecionada por número de versão. Repetir o download pede confirmação e preserva o UUID. Com `--copy`, cada download recebe outro UUID. Um download interrompido pode deixar uma pasta oculta de staging e preserva a instalação publicada.

Por exemplo, `bedrock profile servidor env set MANGOHUD 1` e `bedrock profile servidor wrapper set -- gamemoderun` aplicam essas opções ao lançar esse perfil. `prelaunch` executa um comando na pasta do perfil antes da partida; se falhar, o lançamento para. `args` passa argumentos individuais ao Minecraft. Use aspas do shell para preservar espaços. Os comandos recebem `BEDROCK_PROFILE_DIR`, `BEDROCK_GAME_DIR` e o ambiente configurado no perfil.

A exclusão de um perfil pede `y` e depois o nome exato. Ela remove os mundos, packs e configurações particulares do perfil. Os perfis iniciais também podem ser excluídos.

Todos os perfis começam no prefixo `default`. `prefix set` aceita um prefixo existente ou cria um novo pelo nome. Os dados de cada perfil ficam separados mesmo quando dois perfis usam o mesmo prefixo. A listagem indica disponibilidade em texto e usa verde/vermelho em um terminal com cores. O prefixo é bloqueado durante a partida. Falhas de lançamento também geram uma notificação no desktop.

Cada login usa um diretório privado do Xodus. O serviço systemd de usuário daquela conta sobe sob demanda. `services kill` pede confirmação se o serviço está em uso. `services ciks` solicita ao Xodus as chaves licenciadas dos conteúdos Bedrock instalados e as imprime na saída padrão.

## Arquivos

A raiz padrão é `~/.config/bedrockoverlinux`, respeitando `XDG_CONFIG_HOME`.

```text
profiles/<uuid>.json
profiles/<uuid>/Users/Shared/games/com.mojang/
prefixes/default/
prefix-registry/<uuid>.json
versions/client/<uuid>/installation.json
versions/client/<uuid>/game/
accounts/<uuid>.json
xodus/<uuid>/
tools/<ferramenta>/<tag>/
```

`bedrock env COM_MOJANG set <perfil>` grava o caminho absoluto de `Shared/games/com.mojang` em `/etc/environment` com sudo e pede confirmação para substituir um valor existente. A configuração é da máquina e vale para shells que herdam o ambiente de uma nova sessão de login que lê esse arquivo. Sessões já abertas mantêm sua cópia, e o acesso aos mundos continua sujeito às permissões de arquivos do perfil.

## Testes

Depois da compilação, `bash tests/cli.sh build/bedrock` testa o CLI offline, inclusive a criação simultânea de perfis, a exclusão com confirmação e argumentos com espaços.

Execute `bash tests/integration.sh build/bedrock` numa sessão com systemd de usuário, Perl e jq. O teste usa armazenamento temporário, contas fictícias e um serviço de teste em um socket Unix. A validação de login, compra e jogo reais exige uma conta dona de Minecraft for Windows.
