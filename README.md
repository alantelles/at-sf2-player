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
- **MIDI & Projeto**: suporte a Note On/Off, Pitch Bend e Sustain Pedal, além de salvamento e restauração completa do estado na DAW.

## Dependências (Linux)

Para compilar:
- `g++` (C++17), `make`, `pkg-config`
- `libx11-dev`, `libcairo2-dev`, `libpango1.0-dev`

Em runtime (para o diálogo de seleção de arquivo):
- `zenity` (ou `kdialog`)

No Ubuntu/Debian:
```bash
sudo apt install build-essential pkg-config libx11-dev libcairo2-dev libpango1.0-dev zenity
```

## Compilação, Testes e Instalação

```bash
make
make test
make install   # instala em ~/.vst3/ATSF2Player.vst3
```

## Licença

O código do **AT SF2 Player** é distribuído sob a licença **MIT** — veja o arquivo [LICENSE](LICENSE).

Componentes de terceiros em `third_party/`:
- `third_party/tsf.h`: TinySoundFont (licença MIT).
- `third_party/pluginterfaces/`: Steinberg VST 3 Plug-in Interfaces (sujeito aos termos em `third_party/pluginterfaces/LICENSE.txt`).
