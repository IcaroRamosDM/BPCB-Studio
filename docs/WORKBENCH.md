# BPCB Studio 0.6.0 — mapas, quadros UART e depuração

## Fluxo de autoria

Em **Testes e etapas**, edite o teste e cadastre seu **Mapa de conexões**.
Use um identificador único por conexão, o conector/pino da PCB, o pino do
adaptador, a função, o canal e os limites de tensão declarados. Conexões manuais
podem usar o canal manual; os demais canais devem existir na declaração do teste.
Pinos repetidos, barramentos incompatíveis e tensões acima dos limites informados
são recusados ao validar/exportar.

O mapa é específico de cada teste. É incorporado ao .bancada e às instruções
apresentadas no BPCB. Os valores não constituem medição ou aprovação elétrica.

Em uma etapa **Comunicação**, escolha **Protocolo UART** e **Quadro binário
configurável**. Abra o editor de formato para cadastrar campos, ordem dos bytes,
escala, unidade, limites e checksum. A prévia decodifica bytes em memória.
O campo escolhido alimenta a variável da etapa. Todas as leituras decodificadas
ficam na aquisição. Sem captura, a simulação decodifica o quadro de exemplo
declarado; com captura, utiliza a reprodução e suas falhas.

Os exemplos Laboratorio/UART_Quadro.bpcb e Laboratorio/Mapa_I2C.bpcb começam em
simulação. UART_Quadro.formato.json pode ser importado no editor de ambos os
programas. Adapte pinagem, comandos e níveis à documentação do dispositivo real.

## Depurar

Selecione um teste e use **Depurar** no rodapé. O executor utiliza um pacote
temporário de simulação. A aba **Depurador** mostra:

- Pontos de parada, marcados na lista de etapas.
- Próxima etapa destacada e variáveis antes da execução.
- Continuar, Avançar etapa, Pausar e Inspecionar.
- Caminho percorrido, limitado às últimas 512 visitas.

A primeira etapa inicia pausada. **Avançar etapa** executa a etapa atual e pausa
antes da próxima. **Continuar** respeita os pontos marcados, inclusive em desvios
e repetições. **Pausar** aguarda a próxima fronteira entre etapas. Confirmações
normais e medições manuais mantêm seus diálogos próprios.

Pausas do depurador não consomem o prazo do teste. Cada espera tem limite de uma
hora e a sessão tem limite de duas horas. **Interromper** cancela a execução.
Pontos de parada ficam em memória e não são exportados. A inspeção apresenta até
256 variáveis/20 KiB por parada, com aviso de truncamento. O caminho e o log
também são limitados para evitar crescimento indefinido.

O motor e o protocolo recusam depuração no modo físico. Não é um recurso para
deixar saídas energizadas enquanto se navega pelo editor.

## Formatos e compatibilidade

Projetos novos usam schema 3. Studio 0.6 lê schemas 1, 2 e 3; arquivos antigos
simples conservam sua marca ao salvar. Adotar mapas ou quadros muda a marca para
3, evitando abertura silenciosa em um editor que descarte os novos campos.
.bancada permanece schema 1/API 2. Mapas/quadros requerem BPCB 0.16; laboratório
anterior requer 0.15 e blocos brutos mantêm os mínimos anteriores.

O BPCB 0.16 também oferece preparação guiada, registro explícito de compatibilidade
e pacotes portáteis .bpcbfirmware. Consulte o WORKBENCH.md do BPCB. Firmware e
pacotes de procedimentos .bancada têm funções diferentes.
