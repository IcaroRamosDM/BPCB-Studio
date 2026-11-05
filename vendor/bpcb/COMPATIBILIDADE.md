# Compatibilidade BPCB

Em 01/10/2026, a cópia privada do Studio recebeu os módulos `common`, `model`,
`packages`, `browse`, `pdf`, `assessment` e `windows_compat` do BPCB 0.8.0.
O cabeçalho preserva a interface do Studio, com as declarações de plataforma.
`jobs.c` mantém a execução usada pelo Studio, adaptada para processos Windows.
Não foram incorporados histórico de resultados, calibração ou aquisição Rigol.

A exportação mantém `axtor.bancada`, schema 1 e API 2. Pacotes Windows usam
`windows-x86_64`, executores `.exe` e suas DLLs verificadas por SHA-256.
A saída de diagnóstico do processo Windows usa um canal separado do protocolo
JSONL, impedindo que mensagens de bibliotecas prejudiquem o encerramento.
O despertar da interface é dirigido por eventos; não há temporizador em repouso.

Esta é uma cópia independente: alterações aqui não modificam o projeto BPCB.
