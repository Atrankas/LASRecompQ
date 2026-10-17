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
- [ ] Thread de render (`runtime/nvn/gpu_thread.c`): o executor + driver eram ~1/3 da thread do jogo
- [ ] O heap do jogo (3.15 GB) não é compartilhado com a GPU no Turnip (sem `VK_EXT_external_memory_host`):
      vértices/índices copiados a cada draw e leituras de memória global em shader voltam zero. Ideia: mapear
      só os memory pools da NVN (~300 MB acessíveis pela CPU) como dma-buf
- [ ] SPIR-V otimizado (`LARECOMP_VK_OPT=1`) trava o Turnip (device lost): investigar
- [ ] Ajustes de resolução / FSR para GPUs de celular (padrão no Android: 75% da tela + FSR 1)
- [ ] Afinidade de threads nos núcleos grandes, dicas de desempenho (ADPF), escala por temperatura
- [ ] Drivers customizados (Turnip via libadrenotools) para Adreno com driver antigo
- [ ] Cache de pipelines/shaders, sem travadas ao entrar em áreas
- [ ] Pré-rotação do swapchain (economiza a rotação feita pelo compositor)

### 5. Extras
- [ ] Randomizer no Android (seeds geradas no PC funcionam; gerador no aparelho depois)
- [ ] Tradução PT-BR do app
