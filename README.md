# BPCB Studio

[![Checks](https://github.com/IcaroRamosDM/BPCB-Studio/actions/workflows/checks.yml/badge.svg)](https://github.com/IcaroRamosDM/BPCB-Studio/actions/workflows/checks.yml)

Editor de projetos de testes para PCBs. Produz arquivos `.bancada` que são instalados e executados pelo [BPCB](https://github.com/IcaroRamosDM/BPCB).

Na versão 0.6.0, você monta uma sequência no editor, escolhe os canais e simula cada etapa antes de exportar o pacote para o BPCB.

## Compilar no Linux

Requer Python 3.12+, CMake 3.20+, compiladores C17/C++17 e as bibliotecas de
desenvolvimento listadas no instalador. Em Debian/Ubuntu, revise e execute:

```sh
bash scripts/instalar-dependencias.sh
bash scripts/compilar.sh
./trabalho/build/bpcb_studio
```

O primeiro build baixa FLTK 1.4.5 de sua publicação oficial e confere SHA-256.
Dependências e builds ficam em pastas locais ignoradas pelo Git. Para uso offline,
coloque `fltk-1.4.5-source.tar.gz` oficial em `dependencias/downloads/` antes do build.

Para usar uma instalação FLTK 1.4 existente:

```sh
cmake -S . -B trabalho/build -DFLTK_DIR=/caminho/share/fltk -DCMAKE_BUILD_TYPE=Debug
cmake --build trabalho/build -j4
ctest --test-dir trabalho/build --output-on-failure -j1
```

## Montar uma sequência

Abra um projeto em `examples/`, confira as entradas e as unidades e rode a simulação. O depurador mostra as variáveis antes de cada etapa. Exporte o pacote para executá-lo no BPCB com a placa conectada.

[Etapas de comunicação](docs/COMUNICACAO.md) · [Exemplos do laboratório](docs/LABORATORIO.md) · [Depurador de sequências](docs/WORKBENCH.md)

## Autor

**Icaro Ramos Rodrigues dos Santos**<br>
Engenheiro Eletrônico

Embedded Systems | Firmware | C | C++ | Python | Dart | Assembly | Hardware Design | PCB Design | STM32 | ARM Cortex-M | Linux | Windows | RISC-V | Verilog | SystemVerilog | UVM | Design Verification | Instrumentation | Signal Processing | Qt | FLTK

GitHub: [IcaroRamosDM](https://github.com/IcaroRamosDM)<br>
LinkedIn: [icaro-ramos-r](https://www.linkedin.com/in/icaro-ramos-r/)<br>
[icaroelt@gmail.com](mailto:icaroelt@gmail.com)
