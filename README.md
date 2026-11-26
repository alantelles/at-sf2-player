# AT SF2 Player

Um plugin **VST3** leve e direto para **Linux** (`x86_64`) que carrega e toca arquivos SoundFont (`.sf2`), utilizando [TinySoundFont](https://github.com/schellingb/TinySoundFont) para síntese de áudio embutida (sem dependências externas de servidores de som).

## Recursos
- **Carregamento de `.sf2`**: botão **Load .sf2...** com seletor de arquivos nativo (`zenity`, com fallback para `kdialog`) e carregamento assíncrono sem travar o áudio.
- **Seleção de Bank e Patch**: seletores separados com navegação `<` / `>` e listas suspensas com barra de rolagem larga arrastável.
- **Busca de Presets por Texto**: campo de busca que abre uma lista com **Bank**, **Patch** e **Nome** (selecione com duplo-clique ou `Enter`).
- **Volume e Modulação**: controle de **Volume** (automação VST3 + **MIDI Volume CC #7**) e **Modulation** (**MIDI CC #1** / slider **Mod**).
- **Afinação**: **Coarse Tune** (`-24` a `+24` semitons), **Fine Tune** (`-100` a `+100` cents) e botão **Reset**.
- **Portamento (Glide)**: chave **Porta: ON/OFF**, modos **Retrig** e **Legato**, ajuste de tempo de **Glide** (`5 ms` a `2.00 s`) e mapeamento MIDI CC #65 / CC #5.
- **Envelope ADSR**: ligado por padrão (`ADSR: ON`), com **Attack**, **Decay**, **Sustain**, **Release**, botão **Reset** e piso anti-clique automático de `15 ms` mesmo no modo nativo do SF2.
- **Curva de Velocidade (Velocity Curve)**: gráfico interativo com 2 pontos arrastáveis (`P1`, `P2`) para ajustar a resposta entre velocity de entrada e saída (Linear, Soft, Hard e Fixed ao encostar os pontos nas bordas), além de presets rápidos.
- **MIDI & Projeto**: suporte a Note On/Off, Pitch Bend e Sustain Pedal, além de salvamento e restauração completa do estado na DAW.

## Instalação pelo Bundle Pronto (`.zip`)

1. Baixe `ATSF2Player-linux-x86_64.zip` (da aba *Releases* ou gerado via `make dist`).
2. Extraia diretamente na sua pasta de plugins VST3 (`~/.vst3/`):
   ```bash
   mkdir -p ~/.vst3
   unzip -o ATSF2Player-linux-x86_64.zip -d ~/.vst3/
   ```
3. Reescaneie os plugins na sua DAW (REAPER, Ardour, Bitwig, Carla, etc.).

*(Em runtime, o seletor de arquivos usa `zenity` ou `kdialog`.)*

## Dependências de Compilação (Linux)

- `g++` (C++17), `make`, `pkg-config`, `zip`
- `libx11-dev`, `libcairo2-dev`, `libpango1.0-dev`

No Ubuntu/Debian:
```bash
sudo apt install build-essential pkg-config libx11-dev libcairo2-dev libpango1.0-dev zip zenity
```

## Compilação, Testes e Empacotamento

```bash
make
make test
make dist      # gera build/ATSF2Player-linux-x86_64.zip
make install   # instala em ~/.vst3/ATSF2Player.vst3
```

### Build portátil via Docker (Ubuntu 20.04 / glibc 2.31)
Para gerar o `.zip` compatível com distros mais antigas e modernas (com `libstdc++`/`libgcc` estáticos):
```bash
./scripts/build-release.sh
```
O workflow [release.yml](.github/workflows/release.yml) também executa esse mesmo build no GitHub Actions (e publica o `.zip` automaticamente em tags `v*`).

## Licença

O código do **AT SF2 Player** é distribuído sob a licença **MIT** — veja o arquivo [LICENSE](LICENSE).

Componentes de terceiros em `third_party/`:
- `third_party/tsf.h`: TinySoundFont (licença MIT).
- `third_party/pluginterfaces/`: Steinberg VST 3 Plug-in Interfaces (sujeito aos termos em `third_party/pluginterfaces/LICENSE.txt`).
