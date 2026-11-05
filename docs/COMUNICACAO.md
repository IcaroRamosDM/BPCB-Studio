# Comunicação no Studio 0.4.0

O Studio continua separado do BPCB. Ele edita projetos `.bpcb`, simula e exporta
`.bancada`; o BPCB escolhe o adaptador e executa na placa. O botão **Simular** e o
CLI `simulate-samples` não acessam USB/I²C/SPI/UART/JTAG.

## Criar um teste

1. No cadastro do teste, ative **Usar comunicação selecionável?**.
2. Escolha I²C, SPI, UART ou JTAG; um barramento por teste. Indique os adaptadores
   permitidos, a faixa de clock, o máximo de bytes e de timeout. Não há porta salva
   nem consentimento persistente no projeto. Para usar CH347, configure seu modo
   físico e selecione a interface correspondente no BPCB.
3. Adicione a ação **Transferência I²C / SPI / UART / JTAG**. Configure o barramento
   igual ao teste, dados hex/ASCII, leitura exata, comparação/máscara opcional e
   uma resposta simulada do mesmo comprimento. UART pode acrescentar LF/CR/CRLF
   à escrita. SPI lê o mesmo comprimento que escreve; informe esse tamanho.
4. Em JTAG configure IR/DR/reset/idle, bits, ciclos e prefixo IR opcional antes de DR.
   Reset/Idle não recebem dados; o campo de leitura fica em zero. IR/DR têm leitura
   `ceil(bits/8)` e bits altos de escrita não usados em zero. A sessão é mantida
   durante o teste, preservando a instrução IR entre etapas.
5. A variável recebe quantidade de bytes ou inteiro u8/u16/u32, big/little endian;
   s16 é aceito em ambos os endians. A evidência guarda todos os bytes, máscara,
   configuração efetiva e origem. Resposta diferente é registrada e para o teste.
6. Valide e simule antes de exportar. Até 64 blocos de comunicação e 512 etapas por
   teste; os limites globais de fluxo, tempo e repetições continuam valendo.

Os blocos usam leitura **exata** para testes determinísticos. Para respostas com
comprimento variável, explore o protocolo primeiro no terminal UART do BPCB e
adapte o teste ao enquadramento real. O Studio não adivinha respostas AT nem o
transporte de um módulo por ser “Bluetooth”, “Wi-Fi” ou “IoT”.

## Dispositivos I²C e sinais existentes

TCA9534A, MCP4728, ADS7924 e registradores genéricos podem usar o transporte
**Adaptador I²C escolhido no BPCB**. Cada teste que os acessa deve habilitar I²C
selecionável e publicar limites suficientes (por exemplo, MCP4728 lê 24 bytes).
Não misture o transporte legado com o selecionável no mesmo teste. Diagnósticos
individuais gerados para esses dispositivos também publicam o contrato I²C.

Os transportes antigos continuam disponíveis. CH341 legado usa 20 kHz e passa
pela sessão comum, rejeitando múltiplos dispositivos e mantendo sua identidade.
Linux I²C requer `/dev/i2c-N` exato e um dispositivo de caracteres, sem links.

Os comandos de encerramento dos sinais cadastrados continuam sendo tentados na
ordem definida. Um bloco de bytes arbitrários não define um comando inverso ou
comprova descarga: cadastre controle por sinais e encerramento conforme a placa.
Cancelamento/falha não garante estado elétrico; não há reconexão automática.

## Exportação e compatibilidade

Os novos projetos publicam no manifesto `communication` por teste, com API 1,
barramento, adaptadores, limites e operações. BPCB 0.14.0 ou posterior é necessário.
O BPCB e o executor validam o contrato antes de transmitir. O executor mantém uma
sessão no adaptador selecionado e informa sua identidade efetiva ao BPCB.

Projetos antigos sem estes recursos conservam `min_app: 0.11.0`. Os pacotes continuam
específicos da plataforma: exporte no Studio Windows para BPCB Windows; exporte no
Studio Linux para BPCB Linux. O executor e suas DLLs não podem ser separados.

A cópia de comunicação em `vendor/bpcb/communication/` é independente, com hashes
em `COMMUNICATION-SNAPSHOT.json`; não usa links para a pasta de fontes do BPCB.

## Exemplos

Em `examples/Comunicacao/` há quatro projetos editáveis, inicialmente em simulação:

- `I2C.bpcb`: consulta chip ID do BME280 em 0x76; não inicializa nem compensa o sensor.
- `SPI.bpcb`: sequência ilustrativa de loopback A5 5A, a adaptar à montagem real.
- `UART.bpcb`: AT + CRLF e exemplo OK + CRLF, sem eco, para estudo de ESP-AT/Bluetooth.
- `JTAG.bpcb`: leitura bruta DR de 32 bits após reset TAP, resposta apenas ilustrativa.

Valide, simule, exporte e instale um desses exemplos em dados isolados. Para hardware,
edite conscientemente o modo do teste, as ligações, limites e respostas; no BPCB,
selecione o adaptador e confira as conexões. Nenhum desses exemplos aprova uma PCB.

O BPCB 0.14 também oferece mapas de TCA9534A, ADS7924, BME/BMP280, MPU-6050, INA219,
MCP4728 e comandos SSD1306, além de terminal UART e JTAG/OpenOCD para STM32/ESP32/FPGA.
OpenOCD é uma ferramenta externa opcional, escolhida e revisada pelo operador.
