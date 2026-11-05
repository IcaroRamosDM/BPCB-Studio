Atualização 0.6.0 (17/10/2026): consulte [WORKBENCH.md](WORKBENCH.md) para mapas, quadros UART e o novo fluxo. O contexto anterior abaixo permanece como histórico.

# Laboratório BPCB Studio 0.5.0

Use o Studio para editar o projeto `.bpcb` e exportar o pacote `.bancada`. Os blocos do laboratório exigem o executor BPCB 0.15 ou posterior.

## Criar uma sequência

1. Cadastre placa, conexões e teste. Comece em simulação. Ative comunicação no teste.
2. Se usar um barramento, selecione os adaptadores permitidos e limites. Para vários, escolha multicanal e edite até oito canais nomeados no editor estruturado.
3. Adicione uma etapa de comunicação: bytes brutos, componente IoT ou protocolo UART. Escolha canal, componente, endereço, ação e grandeza. O formulário apresenta opções compatíveis com o componente. Selecione unidade/critério adequados na etapa de resultado; não utilize os exemplos como especificação universal da placa.
4. Para IoT: BME280/BMP280, INA219, MPU6050 e OLED SSD1306. INA219 requer shunt real e LSB; MPU permite faixas; OLED requer confirmação visual além do ACK. Inicializar/medir escreve configurações, ler preserva-as quando aplicável.
5. Para UART: AT (eco, sucesso, mensagens espontâneas), resposta delimitada/fixa ou Modbus RTU funções 1/2/3/4/5/6/15/16. Modbus confere CRC, endereço, função, exceções e eco de escrita. RS-485 requer transceptor apropriado e conferência física de temporização.
6. Defina nome da variável, valor simulado e se a resposta simulada é válida. Um valor digitado é identificado como exemplo declarado, sem leitura física. Resultados inválidos falham mesmo com valor numericamente aceitável.
7. Opcionalmente importe captura/trace JSON para a etapa, escolha falha e índice. A captura é incorporada ao .bpcb e ao pacote; não importe dados que não queira distribuir. Reprodução utiliza tempo virtual e nunca abre hardware. Timeout, corrupção, truncamento e desconexão podem fazer a simulação falhar deliberadamente. Limpar captura/retirar reprodução é explícito.
8. Valide, simule, confira leituras/unidades/gráficos e exporte para a plataforma correta. Instale no BPCB, associe cada canal ao adaptador real e confira novamente conexões. Simulação não qualifica hardware.

## Versões e compatibilidade

Projetos novos usam **.bpcb schema 2** para impedir descarte silencioso dos campos novos em editores anteriores. Studio 0.5 lê schemas 1 e 2. Projetos antigos simples mantêm schema 1; ao adicionar recurso do laboratório/multicanal, a gravação/exportação marca schema 2. Mantenha uma cópia original ao voltar a uma versão antiga. O .bancada permanece schema 1 e API de execução 2. Pacotes antigos conservam mínimo BPCB 0.11; blocos brutos requerem 0.14; laboratório/multicanal requerem **0.15**. Exportações Linux e Windows contêm executores distintos: exporte na plataforma de destino.

## Desempenho e segurança

Todas as operações são validadas antes da primeira transferência. O executor prepara uma cópia privada das operações por índice de etapa, reaproveitada em laços; argumentos entregues ao transporte são cópias para impedir alteração do cache. Cada resposta e limite de execução continuam conferidos. Falhas interrompem a sequência e tentam os comandos de encerramento previstos; isso não comprova o estado físico das saídas.
