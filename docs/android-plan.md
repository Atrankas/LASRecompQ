# LAS Recomp no Android - plano de trabalho

Objetivo: *Link's Awakening* (Switch, v1.0.0) rodando nativo no Android (arm64), leve e jogável do começo ao fim,
a partir da recompilação estática que já roda no macOS / Windows / Linux. Legenda: [x] feito, [~] em andamento,
[ ] a fazer. Cada etapa é testada no aparelho antes de ser marcada.

## O que o jogo precisa (resumo do estudo)
- **Arquivos do jogo**: *não* é o .nsp / .xci direto. São os arquivos **extraídos** da v1.0.0 (sem update), Title ID
  `01006BB00C6F0000`: a pasta `exefs/` (com `main`, `main.npdm`, `sdk`, `rtld`...) e a pasta `romfs/` (com
  `region_common/`...), ~6 GB. O app confere o build id de `exefs/main` (offset 0x40). .nsp/.xci/.nsz/.xcz/.nca são
  recusados ("Those files can't be used").
- **Código do jogo**: `tools/gen.py` (Python + capstone/unicorn/lz4) traduz `exefs/main` e `exefs/sdk` (AArch64) em C
  (`generated/`, ~1,5 GB de C); compilado vira `lasgame.so`, carregado pelo app em tempo de execução.
  No Android (que já é AArch64) o FP/SIMD do jogo vira asm inline nativo: o caminho mais rápido possível.
- **Runtime** (`runtime/`): kernel Horizon próprio (threads, sincronização, memória), serviços (fs, hid, áudio,
  time...), HLE, NVN -> Vulkan (tradutor de shaders Maxwell -> GLSL -> SPIR-V), SDL3 (janela, entrada, áudio),
  FFmpeg (vídeos .webm VP9+Vorbis), ImGui (tela inicial e menu).
- **Randomizer**: LAS-Randomizer em Python (gera "seeds" = pasta com settings.json + romfs modificado).

## Decisões de arquitetura para o Android
1. **Espaço de endereços**: o código recompilado usa endereço do jogo = endereço do processo, numa janela fixa de
   12,5 GB em `0x70_0000_0000`. No Android (VA de 39 bits) essa faixa é onde o zygote põe bibliotecas e heap
   (~40% de chance de colisão por boot, sem re-exec possível). Solução: no Android tudo fica deslocado
   `GUEST_SHIFT = 0x60_0000_0000` para baixo (janela em `0x10_0000_0000`, livre). Os nomes das funções
   (`f_7000...`) e os patches continuam com os endereços do Switch; só os valores mudam. Desktop: shift 0.
2. **Build**: Gradle + CMake (NDK 27), `libmain.so` (runtime) + SDL3 do código-fonte; `SDLActivity` como base.
   Dependências fixadas: SDL3 3.4.18, FFmpeg 9.0.2 (LGPL, só VP9/Vorbis/WebM), shaderc do NDK, VMA.
3. **Código do jogo**: gerado no PC a partir dos arquivos do próprio usuário e compilado com o NDK em
   `liblasgame.so`; instalado só no aparelho dele (APK pessoal ou `adb push`). Nunca publicado.
4. **Dados**: armazenamento interno do app (`files/`): `extracted/`, `save/`, `cache/`, `larecomp.ini`.
   Importação dos arquivos do jogo por seletor de pasta (SAF), copiando para dentro do app.
5. **Gráficos**: Vulkan 1.1+ com extensões (dynamic rendering, sync2, extended dynamic state, timeline) ou 1.3;
   texturas BC decodificadas na CPU onde a GPU não tem BC (Mali); pré-rotação do swapchain; resolução interna
   ajustável + FSR1 (já existe no backend Vulkan).

## Etapas
### 1. Base do port
- [x] Relocação do espaço de endereços (`GUEST_SHIFT`): `runtime/cpu.h`, `rt.h`, `kernel.h`, literais no runtime,
      `tools/gen.py --shift`, `tools/patches.py`. Testado: módulo AArch64 sintético (tabela de saltos, PLT,
      realocações, FP/SIMD) recompilado com shift roda no emulador arm64 com o mesmo resultado do código nativo;
      no mapa real do app a faixa 0x70-0x73 estava lotada e a nova janela 0x10_0000_0000 livre.
- [x] Camada de plataforma Android (`runtime/platform/android.c`, `android_jni.c`): logcat + `lasrecomp.log`
      (+ `.prev.log`), crash handler com backtrace simbólico, `env.txt` para os ganchos de teste
- [x] Projeto Android (`android/`): Gradle, CMake, SDLActivity (`LasActivity`), manifest (paisagem, jogo, sem
      internet, sem tag de ponteiro), tela cheia imersiva, ícone
- [x] Dependências Android (`tools/android/build_deps.sh`): FFmpeg LGPL estático, shaderc, Vulkan-Headers, VMA, volk
- [x] Backend Vulkan no Android: volk, caminho 1.1/1.2 + extensões (e só recursos que o driver tem), SPIR-V
      conforme a versão, decodificação BC na CPU (`runtime/nvn/bcn.c`), swapchain sem recriar a cada frame,
      superfície recriada ao voltar do segundo plano, jogo pausado em segundo plano
- [x] APK compila e abre a tela inicial (emulador arm64, Android 15): toque, seletor de pasta, setup

### 2. Jogo rodando
- [x] `tools/android/build_game.sh <pasta do jogo> [--apk] [--push]`: gen com shift + `liblasgame.so` com o NDK
      (testado de ponta a ponta com o módulo sintético: carregado pelo app, "Game files ready")
- [x] Importação dos arquivos do jogo pelo app (SAF) direto para `extracted/`, ou cópia por USB + Set up
- [x] Imagem romfs fina (`romfs_build_thin`): só as tabelas, dados lidos da pasta `romfs/` (testado byte a byte
      contra a imagem completa); ~6 GB a menos no celular
- [x] Código real gerado, boot até o título, vídeo de abertura, áudio (S20 FE, Turnip)
- [x] Gameplay em Mabe Village (Field): o "device lost" vinha de `subgroupShuffleXor` em fragment shader
      (Turnip/Adreno 650: subgroups só em compute) - shuffles de quad emulados com derivadas
- [ ] Primeira dungeon, saves, troca de áreas

### 3. Controles e interface
- [x] Controles por toque (`runtime/ui/touch_controls.mm`): analógico flutuante, A/B/X/Y (deslizar entre eles),
      L/R/ZL/ZR, +/-, botão do menu; Auto/Sempre/Desligado, tamanho e opacidade no menu (aba Controls)
- [x] Toque na tela inicial e no menu (eventos de dedo direto no ImGui)
- [ ] Controles físicos (SDL3 cobre Xbox/PS/Switch/8BitDo; testar no aparelho)
- [ ] Editor de layout dos botões de toque; vibração

### 4. Leveza e desempenho
- [x] Medir no aparelho: `LARECOMP_GPUPROF_AT`, simpleperf (`setprop security.perf_harden 0`, `--app`)
- [x] Shaders estruturados (sem a máquina de estados `switch (pc)`): pilhas comparadas só nas entradas vivas,
      BRX de compute com seções que terminam o programa. Field: GPU 40 -> 28 ms
- [x] Meia precisão: `unpackHalf2x16` fora da NVIDIA (o caminho por bits era só para o driver da NVIDIA).
      Passo principal do Field 15.8 -> 8.5 ms, GPU 21 ms/frame
- [x] Cache de despacho por thread dentro do `Cpu` (sem `__thread`: no Android cada acesso passava pelo
      resolvedor de TLS dinâmico)
- [x] Thread de render (`runtime/nvn/gpu_thread.c`): o executor + driver eram ~1/3 da thread do jogo
- [x] Memória global dos shaders sem importar o heap (Turnip não tem `VK_EXT_external_memory_host`; 3 GB de
      heap não cabem como memória do driver): storage buffers espelhados num buffer persistente
      (`exec.c mirror_table`). Voltaram o mato alto e as plantações de Mabe Village
- [x] Código do jogo mais enxuto: `Cpu *restrict`, sem o `FN_ENTER` do profiler (-10% de instruções),
      cache de despacho com hash melhor
- [x] "Device lost" de vez em quando: era **GPU hang** (dmesg via `adb bugreport`: `kgsl: GPU hang detected`).
      Dispatches de compute pulados enquanto o pipeline compilava deixavam a lista de luzes sem escrever e o
      shader de fragmento percorria lixo até o guarda de 100000 iterações. Agora dispatches esperam o pipeline
      e laços de shaders gráficos têm guarda menor
- [x] Cache de pipelines salvo durante o jogo (no Android o app é fechado sem `exit()`: o cache nunca era gravado)
- [x] Snapdragon Game Super Resolution (BSD-3) como upscaler padrão no celular: ~1 ms contra 7.5 ms do FSR 1
- [x] Resolução dinâmica padrão no celular (50-75%, segurando os 30 fps do jogo): o overworld vai de 20 para ~30 fps
- [x] Envios antecipados entre passes (a GPU começa antes; o jogo espera menos pela fence do frame)
- [x] Breadcrumbs de GPU (`LARECOMP_VK_BREADCRUMBS`): no device lost, qual operação a GPU estava fazendo
- [x] Travamentos raros de GPU (~1 em 8 partidas no overworld): eram do driver Turnip 26.3 (Mesa main). Com o
      Turnip 24.1.0 R18 a6xx (agora o embutido): 24 de 24 partidas sem travar. Custo: o overworld fica em 20 fps
      (casas e dungeons seguem em 30). Testar drivers novos quando saírem (Video > driver)
- [ ] Overworld a 30 fps com o driver estável: reduzir CPU da thread de render (cópias de vértices: cache entre
      frames) e o custo de gravar comandos do driver antigo
- [ ] Limite do aparelho: `restricted_device_performance=1, 1` (configuração da Samsung) prende os núcleos
      grandes em 1.75 GHz; o núcleo principal (cpu7) fica isolado pelo core_ctl; sem ADPF, sem THP
- [ ] SPIR-V otimizado (`LARECOMP_VK_OPT=1`) trava o Turnip (device lost): investigar
- [ ] Ajustes de resolução / FSR para GPUs de celular (padrão no Android: 75% da tela + FSR 1)
- [ ] Afinidade de threads nos núcleos grandes, dicas de desempenho (ADPF), escala por temperatura
- [ ] Drivers customizados (Turnip via libadrenotools) para Adreno com driver antigo
- [ ] Cache de pipelines/shaders, sem travadas ao entrar em áreas
- [ ] Pré-rotação do swapchain (economiza a rotação feita pelo compositor)

### Situação em 2026-10-17 (fim do dia)
- [x] Atualizado com o projeto original (`beta_5` + branches Vulkan `vk-gmem-window`, `vk-native-h2f`, `ring-oom`,
      `log-getenv`, `intel-fix`); endereços do jogo (`runtime/game_addresses`) passam por `GUEST()` no Android
- [x] Gráfico escuro/sem cor: a culling de luzes lia seus parâmetros (uniform buffer) pela memória global, fora da
      tabela do espelho - sem luzes. Os uniform buffers do compute entram na tabela; slots do espelho reaproveitados
      (enchiam em ~1 min e a luz sumia de novo, personagens pretos)
- [x] Menu de configurações por toque: arrastar rola, toque curto clica (`runtime/ui/touch_ui.h`)
- [x] Capim alto do Campo 5 -> 1.8 ms de GPU: posição `invariant` sem `precise` nos celulares
- [ ] Campo com a iluminação certa: ~30 ms de GPU em resolução cheia (gramado 5 ms, capim 5 ms, sombras 1-3 ms);
      30 fps firmes com o aparelho frio e resolução dinâmica 0.5-0.75; quente (status 4) cai para ~20 fps
- [x] Pré-compilação de shaders (`cache/headstart.bin`): programas e pipelines vistos no jogo são preparados na
      abertura seguinte, em segundo plano (123 programas + 239 pipelines em 0,6 s); entrar no Campo: draws pulados
      1462 -> 3
- [x] 60 fps (gerado): quadro intermediário por reprojeção (profundidade da cena + câmera), como no port do WWHD;
      60 fps na tela na casa da Madam, Tail Cave e no Campo depois de assentar; pausa sozinho quando o jogo não segura 30
- [ ] Campo no trecho mais pesado: ~20-24 ms de GPU mesmo em 50% (capim alto 3-4 ms de geometria): a geração pausa ali
- [ ] Controles físicos (Xbox, PS, GameSir): suporte via SDL3 já existe, falta testar com um controle no aparelho
- [x] Resolução dinâmica lembra o fator de cada área (voltar ao Campo depois de uma casa não começa alto demais)
- [x] Geração de código dos shaders (com o Fable): leituras de cbuf alinhadas, meia precisão sem empacotar, min/max sem
      `isnan` onde o GPU já segue as regras do Maxwell, conhecimento propagado entre blocos. Campo: 0.5 27 -> 15 ms,
      720p 27.5 -> 22.3 ms, 1080p 42 -> 33.5 ms; shader iluminado com 16 registros (4 waves). Casa / Tail Cave em 720p:
      23/20 -> 30 fps. Campo andando no capim: 60 na tela sem quedas, resolução dinâmica ~0.65-0.70
- [x] Menu a 60 por cima do jogo a 30 (um segundo quadro com o menu redesenhado); arrastar rola 1:1 com o dedo
- [x] Água com blocos azuis (Martha's Bay, `Field_15J`): o passe de reflexo compara a própria profundidade com a de
      outro shader; sem `precise` as posições saíam um fio diferentes. `precise` de volta nos celulares (~0.3 ms)
- [x] Casa da Madam: a geração pausava por um quadro atrasado isolado (média lenta agora) e a resolução caía por causa
      da CPU (agora só cai quando a GPU está no limite): 57-59 na tela em 0.75-0.8
- [x] Quadro gerado com o movimento próprio dos objetos (blocos 16x16 comparados com o quadro anterior, do port do
      WWHD): o Link não treme mais ao andar (na vertical principalmente)
- [x] Resolução dinâmica no limite real da GPU: Campo andando no capim a 60 firmes em 0.9-0.95 (era 0.65)
- [ ] Ideias do port do mamcjr (github.com/mamcjr/LASRecompAndroid, MIT): pilhas SSY/PBK/PCNT resolvidas na
      tradução e janela do BRX (em porte); heap como memória da GPU (`VK_EXT_map_memory_placed`) descartado aqui: o
      jogo pede um heap de 3,2 GB e a memória posicionada é toda alocada de uma vez (o S20 FE tem 6 GB)
- [x] Energia: "Equilibrado" (padrão) não segura mais a GPU no clock máximo nem passa por cima do limite de
      temperatura; "Máximo desempenho" como antes
- [ ] Aquecimento: depois de ~1 h de testes seguidos o aparelho chega a status térmico 4 e tudo cai; a resolução
      dinâmica cobre a GPU, mas a CPU lenta atrasa quadros
- [ ] Campo, medido por passe (escala 0.5, ~27 ms de GPU em média, antes da geração de código nova): passe de cor principal ~13 ms, pré-passe de
      profundidade 2.4 ms, sombras 2.1 ms; ~16 ms não dependem da resolução (vértices: ~1.9 milhão por quadro; o
      capim alto tem um VS de 1101 instruções com 32 registros). O shader iluminado (2669 instruções, 2 waves) e os
      VS pesados são o alvo: propostas de geração de código em teste (sem `isnan` nos min/max/saturate, leituras de
      cbuf com componente constante, meias-precisões sem empacotar a cada instrução)
- [x] Tradução PT-BR (fã, Heroes of Time): idioma "Português (Brasil)" nas configurações; os arquivos da tradução
      (em `translations/pt-BR` na pasta de dados, ou importados pelo menu no Android) substituem os textos em inglês
      sem tocar nos arquivos do jogo. Nunca vão para o repositório (`/translations/` no `.gitignore`)
- [ ] Menus do app em português (`ui_language`: automático pelo idioma do sistema, inglês ou português)
- Ferramenta: `build/scratch/bench.sh` (média de GPU por quadro no Campo, escala fixa) - um quadro só varia ±1.5 ms
- Ferramentas: `LARECOMP_DUMP_UBO`, `LARECOMP_DBG_FS` (reg + 3000 = inteiro), `LARECOMP_VK_PASS_GPU_DRAWS=1` (GPU por
  shader), `LARECOMP_DISPLAY` (tela da janela no Mac)

### 5. Extras
- [ ] Randomizer no Android (seeds geradas no PC funcionam; gerador no aparelho depois)
- [ ] Tradução PT-BR do app
