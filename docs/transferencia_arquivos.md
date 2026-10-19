# Transferência de arquivos para o cartão SD — estudo de viabilidade

**Status:** pesquisa/documentação (não implementado).
**Contexto:** o player lê `.wav` do microSD soldado no soquete do CYD. Hoje os
arquivos entram tirando o cartão ou gravando via leitor USB no PC. Este doc
avalia os dois caminhos pedidos: **USB (MTP/MSC)** e **servidor LAN via Wi-Fi**.

---

## 1. USB (MTP, MSC, “modo pendrive”) — ❌ inviável neste SoC

O **ESP32 clássico (WROOM-32) não possui periférico USB device/OTG**. O que
existe na placa é apenas uma ponte **UART↔USB** (CP210x/CH340 ou similar) para
serial/flash — ela não expõe o SoC como dispositivo USB de armazenamento.

| Item | Situação |
| :-- | :-- |
| Peripheral USB device no ESP32 | **Não existe** (TRM: só UART, SPI, I2S, I2C, CAN, SDIO slave…) |
| SDIO slave como “cartão externo” | Teoricamente existe (SDIO slave), mas requer host Linux dedicado + driver custom; não é MTP/MSC e não resolve o caso “plugar no PC” |
| MTP/MSC device | Só em SoCs com **USB-OTG**: ESP32-**S2**, **S3**, **P4** (TinyUSB + MSC class funcionam bem aí) |

**Consequências/práticas:**
- Se um dia o projeto migrar para **ESP32-S3** (tem PSRAM, LCD SPI igual, USB-OTG),
  “modo pendrive MSC” vira ~1 dia de trabalho com TinyUSB (`tusb_msc` + FatFS
  compartilhado com bloqueio entre USB e player).
- No hardware atual, a única via “por cabo” é o próprio **leitor de cartão do PC**.

**Veredito:** descartar USB no ESP32-2432S028R; documentar como recurso de
uma futura migração de SoC.

---

## 2. Servidor LAN via Wi-Fi — ✅ viável, com atenção a RAM e coexistência

O ESP32 tem Wi-Fi 2,4 GHz e o IDF traz `esp_http_server` nativo. Caminhos:

### 2.1 Opções de protocolo

| Opção | Esforço | Prós | Contras |
| :-- | :-- | :-- | :-- |
| **HTTP upload (esp_http_server)** — SoftAP + página web | **Médio (1–2 dias)** | Nativo do IDF; funciona sem roteador (AP próprio); UX de navegador (múltiplos arquivos, progresso) | Precisa gerenciar RAM/coex |
| HTTP upload em **STA** (rede doméstica) + mDNS (`cyd-player.local`) | Médio (+0,5 dia) | Cômodo no dia a dia; mesma LAN do celular/PC | Precisa guardar credenciais (NVS) + provisioning sem teclado (SmartConfig/BLE prov ou página em AP fallback) |
| **FTP server** (componentes comunitários) | Médio | Clientes universais (FileZilla, Explorer) | Passive/active ports, UTF-8/paths chatos; menos UX mobile |
| WebDAV / SMB / NFS | Alto | Monta como unidade no SO | Sem servidor maduro p/ IDF; RAM alta — **não recomendado** |

**Recomendação:** HTTP upload em **SoftAP** (fase 1) → STA+mDNS (fase 2).

### 2.2 Orçamento de RAM e coexistência (o ponto crítico)

Medições atuais do firmware (v1.3.2, unidade real):

```
heap após init completo com BT A2DP ativo: ~75 KB livres
```

Custos típicos no ESP32:

| Componente | Heap aproximado |
| :-- | :-- |
| Wi-Fi driver (SoftAP) + lwIP + TCP | 45–65 KB |
| esp_http_server (1–2 sockets, buffers 4–16 KB) | 8–20 KB |
| Buffer de gravação no SD (chunk) | 16–32 KB (configurável) |

→ **Com BT A2DP ativo + Wi-Fi + upload simultâneos: margem negativa ou
perigosamente baixa.** Estratégias (em ordem de segurança):

1. **Modo transferência exclusivo:** ao entrar, pausa/para o player e
   **desativa o BT** (`esp_bt_controller_disable()`/`esp_bluedroid_deinit()`
   liberam ~40–60 KB); ao sair, reinicia a pilha e reconecta o último
   dispositivo. Coex deixa de ser problema; RAM sobra.
2. **Coex ativo (Wi-Fi+BT ligados):** possível no ESP32 (projetos de web
   radio + A2DP sink provam), mas só com upload **sem streaming simultâneo**
   e monitorando `esp_get_free_heap_size()`; aceitar quedas de sessão HTTP
   como degradação.
3. Chunk de gravação pequeno (8–16 KB) e `httpd` com `stack_size` enxuto.

### 2.3 Concorrência com o SD/FATFS

- O FATFS do IDF é reentrante (mutex), mas **upload + reprodução simultâneos**
  disputam o SPI3 e causam underrun no áudio. Regra do modo transferência:
  **player parado** (a UI já tem estado STOPPED; bloquear CMD_PLAY no modo).
- Escrever em `/sdcard/UPLOAD/` (ou raiz configurável) com `fopen/fwrite`
  diretos; ao concluir, `playlist` recarrega ao sair do modo.

### 2.4 UX proposta

- *Ajustes → “Modo transferência”*: para o player, (opcionalmente) desliga BT,
  sobe SoftAP `CYD-Player` / senha `cyd12345`, mostra na tela o SSID e a URL
  `http://192.168.4.1`.
- Página web: listagem simples de arquivos + `<input type=file multiple>` +
  barra de progresso por arquivo (POST `/upload?name=…`, corpo raw, chunked).
- Botão “Concluir” na tela e na web → derruba Wi-Fi, restaura BT, recarrega
  playlist, volta ao player.

### 2.5 Variante recomendada: “modo download” travado (estilo fastboot) — ideia do usuário

Em vez de ligar/desligar Wi-Fi+HTTP **dinamicamente** com o resto do sistema
vivo, tratar o modo servidor como um **modo de boot exclusivo**:

1. Entrada: *Ajustes → “Modo transferência”* **ou** botão BOOT (GPIO0)
   segurado no power-on → grava flag `xfer=1` na NVS → `esp_restart()`.
2. Boot com `xfer=1`: inicializa **apenas** NVS, SD, Wi-Fi (SoftAP) e
   `esp_http_server`; **não** inicializa Bluedroid/BT, DAC, UI/touch/LCD
   (ou exibe uma tela estática “Modo transferência: SSID/URL”). Heap livre
   passa de ~75 KB para **~200+ KB** — folga total para Wi-Fi+HTTP+buffers.
3. Saída: botão na página web (“Concluir”) ou toque longo na tela estática →
   `xfer=0` + `esp_restart()` → boot normal com reconexão BT automática.

Vantagens: sem `esp_bluedroid_deinit()`/re-init (ciclo sensível), sem coex
Wi-Fi+BT, sem concorrência FATFS com o player, RAM de sobra, e UX clara
(“a placa está em modo download”, como um fastboot). Custo: um branch de
boot no `app_main` + tela estática (~150 linhas no total).

### 2.6 Riscos conhecidos

- `esp_bluedroid_deinit()`/re-init: suportado, mas sensível; testar ciclo
  entrar/sair do modo ≥ 20× (alternativa: manter BT init e aceitar margem
  apertada com upload só ocioso).
- SoftAP + LCD/UI: sem conflitos de pino; Wi-Fi usa IRQ própria.
- Nomes longos/Unicode: FATFS LFN já habilitado; normalizar no servidor web
  (slug) para evitar surpresas.

---

## 3. Plano de implementação sugerido (quando for prioridade)

1. **Fase 1 (MVP, ~1–2 dias):** SoftAP + `esp_http_server` + upload raw
   chunked p/ SD + tela “Modo transferência” + player bloqueado; BT
   **desligado** durante o modo. Critério: subir 50 MB sem reboot, heap
   mínimo ≥ 15 KB.
2. **Fase 2 (+0,5–1 dia):** STA com credenciais na NVS + provisioning via
   página em AP fallback + mDNS; botão “esquecer rede”.
3. **Fase 3 (opcional):** listar/apagar/renomear arquivos pela web; criar
   pastas; retomar upload interrompido (header `Content-Range`).
4. **Fase 4 (outro SoC):** se migrar para S3, adicionar **MSC (pendrive)**
   via TinyUSB como segundo método.

## 4. Referências

- IDF: `esp_http_server` (docs API-GUIDE http-server) e exemplo
  `protocols/http_server/file_serve` (upload multipart).
- IDF: exemplo `wifi/getting_started/softAP`.
- Coex Wi-Fi/BT: docs “Coexistence” (esp32) — A2DP+Wi-Fi é cenário suportado.
- USB: TRM do ESP32 (sem OTG); TinyUSB MSC para S2/S3/P4.
