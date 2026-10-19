# 🎵 CYD Audio Player — ESP32-2432S028R

Reprodutor de música para o **Cheap Yellow Display (CYD / ESP32-2432S028R)**:
lê arquivos **WAV do cartão SD**, transmite por **Bluetooth Classic (A2DP
Source)** para qualquer alto-falante/fone Bluetooth e usa o **alto-falante
on-board (SC8002D)** como saída alternativa — tudo controlado por uma
**interface gráfica touch** escrita do zero (sem LVGL), otimizada para
operação **sem PSRAM**.

- **Framework:** ESP-IDF v5.4.x (C)
- **Compilado e validado:** `idf.py build` limpo (0 erros / 0 warnings) +
  smoke test de boot no QEMU Espressif (sequência de inicialização, NVS,
  fallback sem SD, engine de UI e troca de telas).

---

## 1. Funcionalidades

| Recurso | Descrição |
| :-- | :-- |
| ▶️ Player | Tocar/pausar, faixa anterior/próxima, auto-avanço, barra de progresso |
| 🔊 Bluetooth A2DP **Source** | **Saída principal**: o CYD **transmite** áudio (SBC 44,1 kHz) para caixas/fones BT, com retry automático do START do stream |
| 🔍 Pareamento pela tela | Varredura GAP (**iniciada automaticamente no boot** se não houver dispositivo salvo), lista com RSSI, conexão com 1 toque, reconexão automática do último dispositivo |
| 🎚 Volume absoluto (AVRCP) | O volume do app controla o do alto-falante — e vice-versa (sincronizado) |
| 🎧 Botões do fone (AVRCP Target) | Play/pause/stop/next/prev do fone controlam o player; vol± do fone sincroniza o volume do app |
| 📁 Navegador de arquivos | Pastas + arquivos `.wav` do SD, rolagem por arrasto |
| 🔉 Alto-falante local | DAC 8 bits (GPIO26 → SC8002D) — **desligado por padrão** (v1.1: BT é a saída principal); ativável em *Ajustes* |
| 🖱 Calibração de touch | Assistente de 4 pontos (transformação afim, mínimos quadrados) gravado na NVS — **executado automaticamente no primeiro boot** |
| ⚙️ Ajustes | Volume, brilho (PWM), saída local, auto-avanço, info de heap/SD |
| 💾 Configuração persistente | NVS interna (48 KB); fallback de espelhamento em `/sdcard/cyd.cfg` se a NVS encher |
| 🚦 LED RGB de status | Azul = buscando/conectando, verde = BT conectado |

## 2. Hardware — mapeamento de pinos (ESP32-2432S028R)

O ESP32 tem **apenas 2 hosts SPI de uso geral**, mas a placa expõe **3
conjuntos SPI** (LCD, touch e SD). Solução adotada (a mesma usada pela
comunidade CYD):

| Periférico | Barramento | Pinos |
| :-- | :-- | :-- |
| LCD ILI9341 320×240 | **SPI2 (HSPI)** + DMA | MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, BL 21 |
| Cartão SD | **SPI3 (VSPI)** @ 20 MHz | MOSI 23, MISO 19, SCLK 18, CS 5 |
| Touch XPT2046 | **SPI software (bit-bang)** | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| Amplificador SC8002D | DAC canal 1 | GPIO 26 |
| LED RGB (ativo em nível baixo) | GPIO | R 4, G 16, B 17 |
| Sensor de luz (LDR) | não usado neste projeto | GPIO 34 |

Tudo definido em [`main/app_config.h`](main/app_config.h).

> **Orientação e cores da tela** (v1.3.2, NVS `xform` + `main/app_config.h`):
> - Clones do CYD variam em polaridade do bit **MV** (scan nativo paisagem ou
>   retrato), ordem de cor **BGR/RGB** e espelhos **MX/MY** — e alguns não
>   trazem o SDO conectado (readback inútil). A orientação é um **modo
>   (0–13)** escolhido no quiz do primeiro boot: tabela `s_modes` em
>   `display/lcd_spi_driver.c` (10 MADCTL diretos + 4 transposições por
>   software para scan fixo retrato).
> - Quiz: **toque/arraste** cicla o modo; **segurar 2 s** confirma e abre a
>   calibração de touch. Depois: *Ajustes → “Tela: modo N”* / *“Orient.”*.
> - **Unidades observadas:** CYD ILI9341 padrão → modo **0** (`0x28`);
>   clone com scan paisagem em MV=0 e RGB → modo **2** (`0x40`, confirmado
>   em unidade real com quiz + calibração).
> - A calibração de touch loga cada ponto (`calib: tentativa ponto N:
>   raw=… scr=… alvo=…`), o afim resultante e o desfecho — útil para
>   diagnosticar touch “invertido” (resíduos típicos: 2–4 px).

## 3. Arquitetura (conforme especificação)

```
+--------------------------------------------------------------------+
|                    CORE 0 (Áudio & Bluetooth)                      |
|  Bluedroid (GAP/A2DP Src/AVRCP) -> codificador SBC (nativo)        |
|  bt_work (prio 19)   audio_pipeline (prio 20)                      |
|  SD/WAV -> reamostra p/ 44,1 kHz estéreo -> RingBuffer 16 KB       |
+-----------------------------|--------------------------------------+
                              | filas FreeRTOS (IPC)
+-----------------------------|--------------------------------------+
|                    CORE 1 (UI & Aplicação)                         |
|  app_ctrl (prio 1/2): máquina de estados, comandos, NVS            |
|  ui_render (prio 10): dirty-strips + ping-pong DMA 2×10,24 KB      |
|  touch (prio 5): XPT2046 a cada 20 ms, filtro de ruído             |
+--------------------------------------------------------------------+
```

- **`ui_to_audio_queue`** — comandos da UI: `CMD_PLAY/PAUSE/TOGGLE/NEXT/PREV/
  PLAY_FILE/SET_VOLUME/SET_BRIGHTNESS/SET_LOCAL_OUT/BT_SCAN/BT_CONNECT/...`
- **`audio_to_ui_queue`** — eventos: `EVT_STATE/TRACK_INFO/PROGRESS/TRACK_END/
  ROUTE/BT_STATE/BT_DEVICE/SD/VOLUME/ERROR`
- **Renderização:** sem framebuffer completo (153 KB impossíveis sem PSRAM).
  A tela é dividida em **15 tiras de 320×16 px**; só as tiras "sujas" são
  redesenhadas, com **sobreposição CPU∥DMA** (renderiza a tira *n* enquanto o
  SPI envia a tira *n−1*) — ver `ui/ui_engine.c` e `display/lcd_spi_driver.c`.
- **Áudio:** PCM 44,1 kHz/16 bits estéreo → o callback de dados do Bluedroid
  puxa do RingBuffer e codifica em **SBC** (nativo da pilha). Rota automática:
  BT conectado → A2DP; senão → DAC local (se habilitado).

### Orçamento de memória (medido no build)

| Item | Valor |
| :-- | :-- |
| Binário do app | 1,32 MB (partição factory de 3 MB) |
| Heap livre no boot (antes do BT) | ~210 KB |
| Heap livre após Bluedroid + buffers | ~150 KB |
| Buffers LCD (2× 10,24 KB DMA) + Ring 16 KB + pilhas | ~55 KB |
| Margem livre em regime | **~100 KB** (meta da spec §6 ✅) |

## 4. Estrutura do projeto

```
cyd_audio_player/
├── CMakeLists.txt
├── partitions.csv          # NVS 48 KB + factory 3 MB
├── sdkconfig.defaults      # BR/EDR only, A2DP, FreeRTOS 1 kHz, 240 MHz...
├── tools/gen_font.py       # gera a fonte 8×16 (Pillow + DejaVu Mono Bold)
└── main/
    ├── main.c              # boot, filas IPC, controlador (máquina de estados)
    ├── app_config.h        # pinos e constantes da placa
    ├── app_types.h         # comandos/eventos das filas
    ├── storage/
    │   ├── sd_card.c       # SDSPI + FATFS (LFN) em /sdcard
    │   └── nvs_config.c    # config persistente + espelho no SD
    ├── audio/
    │   ├── wav_decoder.c   # parser RIFF + reamostragem linear + volume
    │   ├── playlist.c      # varredura de pastas/arquivos .wav
    │   ├── audio_pipeline.c# tarefa Core 0: ring buffer / rotas BT↔DAC
    │   ├── local_dac.c     # DAC contínuo GPIO26 (mono 8 bits)
    │   └── bt_a2dp_source.c# GAP discovery, A2DP Source, AVRCP volume
    ├── display/
    │   ├── lcd_spi_driver.c# esp_lcd panel_io ILI9341 + DMA + LEDC backlight
    │   ├── touch_xpt2046.c # SPI software + pressão Z + calibração afim
    │   ├── gfx_primitives.c# retângulos, linhas, texto, ícones, sliders
    │   └── font8x16.c      # fonte bitmap (gerada, 1,5 KB)
    └── ui/
        ├── ui_engine.c     # dirty-strips, ping-pong DMA, dispatch de touch
        └── screens/        # player, browser, bt, settings, calib (+ comum)
```

## 5. Como compilar e gravar

Pré-requisito: **ESP-IDF v5.4.x** instalado
([instruções oficiais](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32/get-started/)).
Outras versões 5.x devem funcionar; em caso de erro de API, use a v5.4.

```bash
cd cyd_audio_player
idf.py set-target esp32      # só na primeira vez
idf.py build

# grave via USB (o CYD usa a porta USB-C; ajuste o port):
idf.py -p /dev/ttyUSB0 flash monitor
```

> O `sdkconfig` já vai configurado. Se apagá-lo, `idf.py build` o regenera a
> partir de `sdkconfig.defaults` (Bluetooth Classic BR/EDR-only, A2DP,
> CPU 240 MHz, FreeRTOS 1 kHz, FATFS com nomes longos, sem PSRAM/Wi-Fi).

## 6. Preparando o cartão SD

O player lê **WAV PCM 16 bits** (1 ou 2 canais; qualquer taxa — 44,1 kHz é o
ideal, outras são reamostradas). Converta suas músicas com ffmpeg:

```bash
# um arquivo
ffmpeg -i musica.mp3 -ar 44100 -ac 2 -c:a pcm_s16le musica.wav

# pasta inteira (mantendo os nomes)
mkdir wav; for f in *.mp3; do ffmpeg -i "$f" -ar 44100 -ac 2 -c:a pcm_s16le "wav/${f%.mp3}.wav"; done
```

- Copie os `.wav` para a raiz do cartão (FAT32) ou para qualquer subpasta —
  o navegador de arquivos percorre tudo.
- Formatos MP3/AAC/FLAC **não** são decodificados nesta versão (ver §9).

## 7. Primeiro uso

1. **Calibração do touch** — aparece automaticamente no primeiro boot:
   toque no centro das 4 miras. (Refazer em *Ajustes → Calibrar*; *Pular*
   usa o mapeamento padrão de fábrica.)
2. **Bluetooth (saída principal):** se nenhum dispositivo foi salvo, o CYD
   **já inicia a varredura sozinho no boot** (LED azul). Abra
   *Player → Bluetooth* e toque na sua caixa/fone da lista. Pareamento SSP é
   confirmado automaticamente; se o dispositivo pedir PIN, use `1234`. O
   último dispositivo é reconectado automaticamente nos próximos boots.
3. **Lista** → escolha a pasta/arquivo → a reprodução começa. Se o stream BT
   ainda não subiu, a faixa fica **pré-carregada em espera** e começa assim
   que o alto-falante conecta (o START do stream é repetido automaticamente
   até 5× se a caixa recusar a primeira tentativa).
4. O alto-falante **on-board fica desligado por padrão** (v1.1). Para usá-lo
   como saída quando não há BT: *Ajustes → Alto-falante: LIGADO*.

## 8. Solução de problemas

| Sintoma | Causa provável / ação |
| :-- | :-- |
| Cores em "negativo" | `LCD_INVERT_COLORS` → `1` em `app_config.h` |
| Vermelho/azul trocados | `LCD_MADCTL` → remova o bit BGR (`0x28`→`0x20`) |
| Imagem de cabeça para baixo | `LCD_MADCTL` → `0xE8` e recalibre o touch |
| Conteúdo "espremido"/rotacionado (retrato) | Veja os logs `DIAG RDDMADCTL`/`DIAG RDDID` no boot: se o MADCTL relido ≠ `0x28`, o painel não latchou o comando — reduza `LCD_PCLK_HZ` (ex.: 10 MHz) e regrave; se o ID não for `00 93 41`, informe o valor (painel clone com init diferente) |
| Touch responde no lugar errado | Refaça a calibração (Ajustes → Calibrar) |
| "Cartão SD ausente" | Cartão não formatado FAT32, ou mal inserido; teste outro cartão. O boot continua normal sem SD |
| BT não encontra a caixa | A caixa precisa estar **visível/pareável**; aproxime e escaneie de novo. O filtro lista dispositivos com nome ou classe de áudio |
| Conecta mas não sai som | Verifique o volume; o START do stream é retentado automaticamente (log "START novamente"). Se persistir, desconecte e reconecte pela tela BT |
| Áudio "picota" no BT | Interferência 2,4 GHz (Wi-Fi próximo); o ring de 16 KB já absorve leituras do SD. Reduza a distância roteador/caixa |
| Som distorcido no alto-falante local | Esperado: é um DAC de 8 bits + amp pequeno. Use o BT para qualidade cheia |

Logs de diagnóstico saem pela serial (115200). O heap livre aparece em
*Ajustes* e nos logs de boot.

## 9. Transferir músicas sem tirar o SD (estudo)

USB MTP/MSC é **inviável** no ESP32 clássico (sem periférico USB device);
servidor HTTP/FTP via Wi-Fi é **viável** com “modo transferência” dedicado
(RAM/coex com BT é o ponto crítico). Estudo completo com orçamento de memória,
UX proposta e plano em fases: [`docs/transferencia_arquivos.md`](docs/transferencia_arquivos.md).

## 10. Ideias de extensão

- **MP3/AAC:** integrar o componente [`esp_new_codec`](https://components.espressif.com/components/espressif/esp_new_codec)
  (Helix MP3, ~30 KB heap) atrás da interface `wav_decoder` — o pipeline já
  isola a decodificação.
- **Seek (↔10 s):** o decoder já é posicional; falta expor `wav_seek()`.
- **Rádio web / Spotify Connect:** o CYD tem Wi-Fi — um modo alternativo
  pode alimentar o mesmo ring buffer via HTTP stream.
- **Metadados:** ler o bloco `LIST/INFO` dos WAV (título/artista) — o parser
  já percorre chunks desconhecidos.
- **Brilho automático:** LDR em GPIO34 + `lcd_set_backlight()`.

## 11. Notas de implementação

- Os callbacks do Bluedroid rodam na tarefa BTC; **nunca** chamam APIs da
  pilha diretamente — enfileiram itens leves para a tarefa `bt_work`
  (padrão do exemplo oficial `a2dp_source`, evita deadlocks).
- Símbolos globais usam prefixos (`appcfg_*`, `xpt2046_*`) porque nomes
  genéricos como `config_save`/`touch_init` **colidem com bibliotecas
  internas do IDF** (Bluedroid `osi` e `librtc`).
- `APP_BT_ENABLE=0` e `LCD_HW_PRESENT=0` em `app_config.h` geram um build
  sem rádio/display — útil para testes de lógica no QEMU
  (`qemu-system-xtensa -machine esp32`).
- **v1.2/v1.3 (display):** após unidades reais mostrarem que (a) o caminho
  `esp_lcd panel_io` e mesmo CS/DC manuais não resolviam em clones que
  ignoram/invertem o bit MV, e (b) alguns clones não têm SDO (readback
  inútil), o driver passou a: escrever registros com CS/DC por GPIO
  (sequência clássica ILI9341), tratar readback como informativo e oferecer
  **14 modos de orientação** (MADCTL diretos + transposição por software no
  flush), escolhidos no quiz do primeiro boot e persistidos na NVS.
- A fonte 8×16 é gerada por `tools/gen_font.py` (Pillow + DejaVu Sans Mono
  Bold); títulos usam escala 2× (dobrada em pixels).
